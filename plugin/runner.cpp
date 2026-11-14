// PNR plugin system - isolated execution + permission broker (slice P2), GPL-2.0. See runner.hpp.
#include "plugin/runner.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "psn/json.hpp"
#include "sandbox/sandbox.hpp"

namespace fs = std::filesystem;
namespace pnr::plugin {
namespace {

bool granted(const InstalledPlugin& p, const char* perm) {
  return std::find(p.granted.begin(), p.granted.end(), perm) != p.granted.end();
}
bool valid_data_name(const std::string& n) {
  if (n.empty() || n.size() > 64) return false;
  for (char c : n) if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) return false;
  return n != "." && n != "..";
}
std::string jstr(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += (char)c;
  }
  return o + "\"";
}
std::string err_reply(const std::string& m) { return "{\"ok\":false,\"error\":" + jstr(m) + "}\n"; }

}  // namespace

RunResult run_plugin(const InstalledPlugin& p, const std::string& home, const RunOptions& opt) {
  RunResult r;
  if (p.manifest.entry_type != "process") { r.error = "plugin has no process entry (it is data-only)"; return r; }
  if (!p.enabled) { r.error = "plugin is not enabled"; return r; }
  if (std::string(host_platform()) != "linux") { r.error = "plugin execution is Blocked on this platform"; return r; }
  sandbox::Probe pr = sandbox::probe();
  if (!pr.usable()) { r.error = "sandbox is not usable here, so the plugin was not started (" + pr.notes + ")"; return r; }

  std::error_code ec;
  std::string entry = (fs::path(p.dir) / p.manifest.entry_path).string();
  fs::path canon = fs::weakly_canonical(entry, ec);
  fs::path root = fs::weakly_canonical(p.dir, ec);
  if (canon.string().compare(0, root.string().size(), root.string()) != 0 || !fs::is_regular_file(canon, ec)) {
    r.error = "entry file is missing or outside the plugin directory"; return r;
  }
  std::string data_dir = fs::absolute(fs::path(home) / "plugins" / "data" / p.manifest.id).string();
  bool can_data = granted(p, "fs_write_plugin_data");
  if (can_data) fs::create_directories(data_dir, ec);
  sandbox::Policy pol;
  pol.game_dir = root.string();
  if (can_data) pol.rw_dirs.push_back(data_dir);

  int in_pipe[2], out_pipe[2];
  if (pipe(in_pipe) || pipe(out_pipe)) { r.error = "pipe failed"; return r; }
  pid_t pid = fork();
  if (pid < 0) { r.error = "fork failed"; return r; }
  if (pid == 0) {
    dup2(in_pipe[0], 0); dup2(out_pipe[1], 1);
    int dn = open("/dev/null", O_WRONLY); if (dn >= 0) dup2(dn, 2);
    close(in_pipe[0]); close(in_pipe[1]); close(out_pipe[0]); close(out_pipe[1]);
    std::string err;
    if (!sandbox::enter(pol, &err)) _exit(126);
    std::string ep = canon.string();
    std::string e1 = "PATH=/usr/bin:/bin", e2 = "PNR_PLUGIN_ID=" + p.manifest.id;
    std::string e3 = can_data ? "PNR_PLUGIN_DATA=" + data_dir : "PNR_PLUGIN_DATA=";
    char* argv[] = {(char*)ep.c_str(), nullptr};
    char* envp[] = {(char*)e1.c_str(), (char*)e2.c_str(), (char*)e3.c_str(), nullptr};
    execve(ep.c_str(), argv, envp);
    _exit(127);
  }
  close(in_pipe[0]); close(out_pipe[1]);
  r.started = true;
  signal(SIGPIPE, SIG_IGN);

  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(opt.timeout_ms);
  std::string buf; std::size_t total = 0; bool eof = false;
  auto reply = [&](const std::string& s) { ssize_t w = write(in_pipe[1], s.data(), s.size()); (void)w; };
  while (!eof && !r.completed) {
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    if (left <= 0) { r.timed_out = true; r.error = "plugin timed out"; break; }
    struct pollfd pf{out_pipe[0], POLLIN, 0};
    int pn = poll(&pf, 1, (int)std::min<long>(left, 200));
    if (pn < 0 && errno != EINTR) break;
    if (pn > 0) {
      char tmp[4096]; ssize_t n = read(out_pipe[0], tmp, sizeof tmp);
      if (n <= 0) { eof = true; }
      else { buf.append(tmp, (std::size_t)n); total += (std::size_t)n; }
      if (total > opt.max_output_bytes) { r.error = "plugin output limit exceeded"; break; }
    }
    std::size_t nl;
    while (!r.completed && (nl = buf.find('\n')) != std::string::npos) {
      std::string line = buf.substr(0, nl); buf.erase(0, nl + 1);
      if (r.served + r.denied >= opt.max_requests) { r.error = "plugin request limit exceeded"; eof = true; break; }
      psn::Json j; std::string je;
      if (!psn::json_parse(line, &j, &je) || !j.is(psn::Json::T::Obj)) { ++r.denied; reply(err_reply("malformed request")); continue; }
      const psn::Json* c = j.get("call");
      std::string call = c && c->is(psn::Json::T::Str) ? c->s : "";
      auto sval = [&](const char* k) { const psn::Json* v = j.get(k); return v && v->is(psn::Json::T::Str) ? v->s : std::string(); };
      if (call == "log") {
        if (r.logs.size() < 100) r.logs.push_back(sval("msg").substr(0, 500));
        ++r.served; reply("{\"ok\":true}\n");
      } else if (call == "result") {
        r.result = sval("value").substr(0, 65536); r.completed = true; ++r.served;
      } else if (call == "data_put" || call == "data_get") {
        if (!can_data) { ++r.denied; reply(err_reply("permission fs_write_plugin_data was not granted")); continue; }
        std::string name = sval("name");
        if (!valid_data_name(name)) { ++r.denied; reply(err_reply("invalid data name")); continue; }
        fs::path f = fs::path(data_dir) / name;
        if (call == "data_put") {
          std::string v = sval("value");
          if (v.size() > 65536) { ++r.denied; reply(err_reply("value too large")); continue; }
          std::ofstream(f, std::ios::binary | std::ios::trunc) << v;
          ++r.served; reply("{\"ok\":true}\n");
        } else {
          std::ifstream in(f, std::ios::binary);
          if (!in) { ++r.denied; reply(err_reply("no such data")); continue; }
          std::stringstream ss; ss << in.rdbuf();
          ++r.served; reply("{\"ok\":true,\"value\":" + jstr(ss.str().substr(0, 65536)) + "}\n");
        }
      } else if (call == "library_list" || call == "store_add_source" || call == "download_request") {
        const char* need = call == "library_list" ? "library_read" : call == "store_add_source" ? "store_add_source" : "download_request";
        if (!granted(p, need)) { ++r.denied; reply(err_reply(std::string("permission ") + need + " was not granted")); continue; }
        if (!opt.host) { ++r.denied; reply(err_reply("not available: no library or store is connected to this run")); continue; }
        std::string e2, js;
        bool ok = call == "library_list" ? opt.host->library_list(&js, &e2)
                : call == "store_add_source" ? opt.host->store_add_source(sval("id"), sval("url"), sval("rights").substr(0, 500), &e2)
                : opt.host->download_request(sval("source"), sval("pkg"), &e2);
        if (!ok) { ++r.denied; reply(err_reply(e2.empty() ? "refused" : e2.substr(0, 300))); continue; }
        ++r.served; reply(call == "library_list" ? "{\"ok\":true,\"value\":" + jstr(js.substr(0, 65536)) + "}\n" : "{\"ok\":true}\n");
      } else {
        ++r.denied; reply(err_reply("unknown call: " + call.substr(0, 40)));
      }
    }
  }
  close(in_pipe[1]); close(out_pipe[0]);
  kill(pid, SIGKILL);
  int st = 0; waitpid(pid, &st, 0);
  if (WIFEXITED(st)) r.exit_status = WEXITSTATUS(st);
  if (!r.completed && r.error.empty()) r.error = "plugin exited without sending a result";
  return r;
}

}  // namespace pnr::plugin
