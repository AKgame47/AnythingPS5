// Slice B3 tests: scan pipeline against fake helpers and a fake clamscan
// (tiny shell scripts). No real model, thrember or ClamAV is involved: these
// tests prove the pipeline's plumbing and its honest "off" reporting, NOT
// detection quality.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "package/sha256.hpp"
#include "package/tar.hpp"
#include "scan/scan.hpp"

namespace fs = std::filesystem;
namespace {
std::string stage_state(const pnr::scan::Report& r, const std::string& n) { for (auto& s : r.stages) if (s.stage == n) return s.state; return "missing"; }
std::string stage_detail(const pnr::scan::Report& r, const std::string& n) { for (auto& s : r.stages) if (s.stage == n) return s.detail; return ""; }
void write_script(const std::string& p, const std::string& body) { { std::ofstream f(p); f << body; } ::chmod(p.c_str(), 0755); }
}  // namespace

void pnr_scan_tests(int& pass, int& fail) {
  auto check = [&](bool ok, const std::string& label) { if (ok) ++pass; else ++fail; std::printf("%s %s\n", ok ? "PASS" : "FAIL", label.c_str()); };
  const std::string d = "/tmp/pnr_scan_t_" + std::to_string(::getpid());
  fs::remove_all(d); fs::create_directories(d);
  auto read_all = [](const std::string& p) { std::ifstream f(p, std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); };
  auto make_pkg = [&](const std::string& name, bool with_elf) {
    std::vector<pnr::pkg::TarEntry> es;
    es.push_back({"meta.txt", std::vector<uint8_t>{'h', 'i'}});
    if (with_elf) es.push_back({"bin/game", std::vector<uint8_t>{0x7f, 'E', 'L', 'F', 1, 2, 3, 4}});
    pnr::pkg::tar_write(d + "/" + name, es);
    return d + "/" + name;
  };
  const std::string withelf = make_pkg("a.pnrgame", true), noelf = make_pkg("b.pnrgame", false);
  const std::string content = read_all(withelf), sha = pnr::pkg::sha256_hex(content); const long long size = static_cast<long long>(content.size());
  pnr::scan::Config off; off.clamscan = "/nonexistent/clamscan-for-tests"; off.interpreter = "/bin/sh";

  auto r = pnr::scan::scan_package(withelf, sha, size, off);
  check(stage_state(r, "hash") == "ok", "scan: hash stage is always on and passes for a matching file");
  check(stage_state(r, "model") == "off" && stage_detail(r, "model").find("no model scoring happened") != std::string::npos, "scan: model stage is OFF, with the reason, when nothing is configured");
  check(stage_state(r, "clamav") == "off", "scan: ClamAV stage is OFF when clamscan is absent");
  check(!r.flagged() && r.text().find("NOT a guarantee") != std::string::npos, "scan: off stages never flag; the report says a clean result is not a guarantee");
  check(stage_state(pnr::scan::scan_package(withelf, std::string(64, '0'), size, off), "hash") == "flagged", "scan: a hash mismatch is flagged");
  check(stage_state(pnr::scan::scan_package(withelf, sha, size + 1, off), "hash") == "flagged", "scan: a size mismatch is flagged");

  const std::string model = d + "/fake.model"; { std::ofstream f(model); f << "x"; }
  auto cfg_with = [&](const std::string& body) { pnr::scan::Config c = off; c.helper = d + "/helper.sh"; c.model = model; write_script(c.helper, body); return c; };
  r = pnr::scan::scan_package(withelf, sha, size, cfg_with("echo '{\"score\": 0.1}'\n"));
  check(stage_state(r, "model") == "ok" && stage_detail(r, "model").find("not calibrated") != std::string::npos, "scan: low score is ok, labelled not calibrated");
  r = pnr::scan::scan_package(withelf, sha, size, cfg_with("echo '{\"score\": 0.93}'\n"));
  check(stage_state(r, "model") == "flagged" && r.flagged(), "scan: score at or above the threshold is flagged");
  r = pnr::scan::scan_package(withelf, sha, size, cfg_with("echo 'missing dependency' >&2; exit 2\n"));
  check(stage_state(r, "model") == "error" && !r.flagged(), "scan: a failing helper is an error, never a silent pass or a fake score");
  r = pnr::scan::scan_package(withelf, sha, size, cfg_with("echo 'not json'\n"));
  check(stage_state(r, "model") == "error", "scan: a helper without a valid score is an error");
  r = pnr::scan::scan_package(withelf, sha, size, cfg_with("echo '{\"score\": 7}'\n"));
  check(stage_state(r, "model") == "error", "scan: an out-of-range score is rejected");
  { auto c = cfg_with("sleep 5\n"); c.timeout_seconds = 1; r = pnr::scan::scan_package(withelf, sha, size, c); check(stage_state(r, "model") == "error" && stage_detail(r, "model").find("timed out") != std::string::npos, "scan: a hung helper is killed at the timeout"); }
  r = pnr::scan::scan_package(noelf, pnr::pkg::sha256_hex(read_all(noelf)), static_cast<long long>(read_all(noelf).size()), cfg_with("echo '{\"score\": 0.99}'\n"));
  check(stage_state(r, "model") == "not-applicable", "scan: a package with no ELF files is not-applicable, the container itself is never scored");
  { auto c = cfg_with("echo '{\"score\": 0.1}'\n"); c.model = d + "/absent.model"; r = pnr::scan::scan_package(withelf, sha, size, c); check(stage_state(r, "model") == "off", "scan: a missing model file means OFF"); }

  auto clam = [&](int code) { pnr::scan::Config c = off; c.clamscan = d + "/clamscan"; write_script(c.clamscan, "echo 'sample: Fake.Sig FOUND'\nexit " + std::to_string(code) + "\n"); return c; };
  check(stage_state(pnr::scan::scan_package(withelf, sha, size, clam(0)), "clamav") == "ok", "scan: clamscan exit 0 is ok");
  check(stage_state(pnr::scan::scan_package(withelf, sha, size, clam(1)), "clamav") == "flagged", "scan: clamscan exit 1 is flagged");
  check(stage_state(pnr::scan::scan_package(withelf, sha, size, clam(2)), "clamav") == "error", "scan: clamscan exit 2 is an error");
  fs::remove_all(d);
}
