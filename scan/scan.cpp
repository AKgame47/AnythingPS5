// See scan.hpp.
#include "scan/scan.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "package/sha256.hpp"
#include "package/tar.hpp"
#include "psn/json.hpp"

namespace fs = std::filesystem;
namespace pnr::scan {
namespace {
struct RunResult { bool started = false, timed_out = false; int exit_code = -1; std::string out; };

RunResult run_capture(const std::vector<std::string>& argv, int timeout_s) {
  RunResult r;
  int fd[2];
  if (::pipe(fd) != 0) return r;
  pid_t p = ::fork();
  if (p < 0) { ::close(fd[0]); ::close(fd[1]); return r; }
  if (p == 0) {
    ::dup2(fd[1], 1); ::dup2(fd[1], 2); ::close(fd[0]); ::close(fd[1]);
    std::vector<char*> a; for (auto& s : argv) a.push_back(const_cast<char*>(s.c_str())); a.push_back(nullptr);
    ::execvp(a[0], a.data());
    _exit(127);
  }
  r.started = true; ::close(fd[1]);
  const auto t0 = std::chrono::steady_clock::now();
  char buf[4096];
  for (;;) {
    const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (el > timeout_s) { ::kill(p, SIGKILL); r.timed_out = true; break; }
    pollfd pf{fd[0], POLLIN, 0};
    int pr = ::poll(&pf, 1, 200);
    if (pr > 0) { ssize_t n = ::read(fd[0], buf, sizeof buf); if (n <= 0) break; if (r.out.size() < (1u << 20)) r.out.append(buf, static_cast<size_t>(n)); }
  }
  ::close(fd[0]);
  int st = 0; while (::waitpid(p, &st, 0) < 0 && errno == EINTR) {}
  r.exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  return r;
}

bool on_path(const std::string& name) {
  if (name.find('/') != std::string::npos) return ::access(name.c_str(), X_OK) == 0;
  const char* path = std::getenv("PATH"); if (!path) return false;
  std::string p = path; size_t i = 0;
  while (i <= p.size()) { size_t j = p.find(':', i); if (j == std::string::npos) j = p.size(); std::string d = p.substr(i, j - i); if (!d.empty() && ::access((d + "/" + name).c_str(), X_OK) == 0) return true; i = j + 1; }
  return false;
}
std::string trim(std::string s) { while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back(); return s; }
std::string num(double d) { char b[32]; std::snprintf(b, sizeof b, "%.4f", d); return b; }
}  // namespace

Config config_from_env() {
  Config c;
  if (const char* v = std::getenv("PNR_SCAN_PYTHON")) if (*v) c.interpreter = v;
  if (const char* v = std::getenv("PNR_SCAN_HELPER")) c.helper = v;
  if (const char* v = std::getenv("PNR_SCAN_MODEL")) c.model = v;
  if (const char* v = std::getenv("PNR_SCAN_THRESHOLD")) { char* e = nullptr; double d = std::strtod(v, &e); if (e && *e == 0 && d > 0 && d <= 1) c.threshold = d; }
  if (const char* v = std::getenv("PNR_SCAN_CLAMSCAN")) if (*v) c.clamscan = v;
  return c;
}

bool Report::flagged() const { for (auto& s : stages) if (s.state == "flagged") return true; return false; }

std::string Report::text() const {
  std::string o = "SCAN\n";
  for (auto& s : stages) o += "  " + s.stage + ": " + s.state + (s.detail.empty() ? "" : " - " + s.detail) + "\n";
  o += "  note: detection is high but not absolute. A clean result is NOT a guarantee the package is safe.\n";
  return o;
}

Report scan_package(const std::string& path, const std::string& sha, long long size, const Config& cfg) {
  Report rep;
  // 1) hash: always on
  {
    std::ifstream f(path, std::ios::binary); std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (!f && data.empty()) rep.stages.push_back({"hash", "error", "cannot read the package file"});
    else if (static_cast<long long>(data.size()) != size || pkg::sha256_hex(data) != sha) rep.stages.push_back({"hash", "flagged", "file does not match the index size and sha256"});
    else rep.stages.push_back({"hash", "ok", "size and sha256 match the index"});
  }
  // 2) model score
  {
    StageResult st{"model", "off", ""};
    if (cfg.helper.empty() || cfg.model.empty()) st.detail = "not configured (needs PNR_SCAN_MODEL and PNR_SCAN_HELPER); no model scoring happened";
    else if (!fs::exists(cfg.model)) st.detail = "model file not found: no model scoring happened";
    else if (!fs::exists(cfg.helper)) st.detail = "helper script not found: no model scoring happened";
    else if (!on_path(cfg.interpreter)) st.detail = "interpreter '" + cfg.interpreter + "' not found: no model scoring happened";
    else {
      std::vector<pkg::TarEntry> entries;
      if (!pkg::tar_read(path, entries)) { st = {"model", "error", "could not read the package container"}; }
      else {
        char tmpl[] = "/tmp/pnr_scan_XXXXXX"; char* d = ::mkdtemp(tmpl);
        if (!d) st = {"model", "error", "could not create a temp directory"};
        else {
          int elfs = 0, scored = 0; double best = -1; std::string errs;
          for (auto& e : entries) {
            if (e.data.size() < 4 || e.data[0] != 0x7f || e.data[1] != 'E' || e.data[2] != 'L' || e.data[3] != 'F') continue;
            ++elfs;
            const std::string fp = std::string(d) + "/elf" + std::to_string(elfs) + ".bin";
            { std::ofstream o(fp, std::ios::binary); o.write(reinterpret_cast<const char*>(e.data.data()), static_cast<std::streamsize>(e.data.size())); }
            RunResult r = run_capture({cfg.interpreter, cfg.helper, "--model", cfg.model, "--file", fp}, cfg.timeout_seconds);
            psn::Json j; std::string je;
            const psn::Json* sc = nullptr;
            if (r.timed_out) errs += e.name + ": helper timed out; ";
            else if (r.exit_code != 0) errs += e.name + ": helper failed (" + trim(r.out).substr(0, 160) + "); ";
            else if (!psn::json_parse(trim(r.out), &j, &je) || !(sc = j.get("score")) || !sc->is(psn::Json::T::Num) || sc->n < 0 || sc->n > 1) errs += e.name + ": helper gave no valid score; ";
            else { ++scored; best = std::max(best, sc->n); }
          }
          fs::remove_all(d);
          if (elfs == 0) st = {"model", "not-applicable", "no ELF files inside the package; the model does not score the container itself"};
          else if (scored == 0) st = {"model", "error", errs};
          else {
            const bool bad = best >= cfg.threshold;
            st = {"model", bad ? "flagged" : "ok", "highest score " + num(best) + " over " + std::to_string(scored) + " of " + std::to_string(elfs) + " ELF file(s); threshold " + num(cfg.threshold) + " (not calibrated by PNR)" + (errs.empty() ? "" : "; some failed: " + errs)};
          }
        }
      }
    }
    rep.stages.push_back(st);
  }
  // 3) ClamAV
  {
    StageResult st{"clamav", "off", ""};
    if (!on_path(cfg.clamscan)) st.detail = "clamscan not found: no ClamAV scan happened";
    else {
      RunResult r = run_capture({cfg.clamscan, "--no-summary", path}, cfg.timeout_seconds);
      if (r.timed_out) st = {"clamav", "error", "timed out"};
      else if (r.exit_code == 0) st = {"clamav", "ok", "no signature matched (signature databases may be out of date)"};
      else if (r.exit_code == 1) st = {"clamav", "flagged", trim(r.out).substr(0, 200)};
      else st = {"clamav", "error", "clamscan exit " + std::to_string(r.exit_code) + ": " + trim(r.out).substr(0, 160)};
    }
    rep.stages.push_back(st);
  }
  return rep;
}
}  // namespace pnr::scan
