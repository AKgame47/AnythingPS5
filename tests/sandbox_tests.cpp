// Slice B2 tests: the Linux sandbox is exercised for real (fork, namespaces,
// seccomp, exec). If this host does not permit user namespaces the real-effect
// tests are SKIPPED and printed as such; they are never faked as passes of the
// isolation itself.
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <string>

#include "sandbox/sandbox.hpp"

namespace {
// Runs `sh -c script` inside the sandbox; returns exit status, output via file.
int run_sb(const std::string& script, const std::string& outfile, std::string* err) {
  pid_t c = ::fork();
  if (c < 0) return -1;
  if (c == 0) {
    std::string e;
    if (!pnr::sandbox::enter(&e)) { std::fprintf(stderr, "ENTER FAILED: %s\n", e.c_str()); _exit(125); }
    std::string full = "exec >" + outfile + " 2>&1; " + script;
    ::execl("/bin/sh", "sh", "-c", full.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  int st = 0; while (::waitpid(c, &st, 0) < 0) {}
  if (WIFSIGNALED(st)) { *err = "signaled " + std::to_string(WTERMSIG(st)); return 128 + WTERMSIG(st); }
  return WEXITSTATUS(st);
}
int run_fs(const pnr::sandbox::Policy& pol, const std::string& script, const std::string& outfile) {
  pid_t c = ::fork();
  if (c < 0) return -1;
  if (c == 0) {
    std::string e;
    if (!pnr::sandbox::enter(pol, &e)) { std::fprintf(stderr, "ENTER FAILED: %s\n", e.c_str()); _exit(125); }
    std::string full = "exec >" + outfile + " 2>&1; " + script;
    ::execl("/bin/sh", "sh", "-c", full.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  int st = 0; while (::waitpid(c, &st, 0) < 0) {}
  return WIFSIGNALED(st) ? 128 + WTERMSIG(st) : WEXITSTATUS(st);
}
std::string slurp(const std::string& p) { std::string s; if (FILE* f = std::fopen(p.c_str(), "r")) { char b[512]; size_t n; while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n); std::fclose(f); } return s; }
}  // namespace

void pnr_sandbox_tests(int& pass, int& fail) {
  auto check = [&](bool ok, const std::string& label) { if (ok) ++pass; else ++fail; std::printf("%s %s\n", ok ? "PASS" : "FAIL", label.c_str()); };
  auto p = pnr::sandbox::probe();
  const std::string d = pnr::sandbox::describe(p);
  check(d.find("filesystem isolation (pivot_root):") != std::string::npos && (p.filesystem ? d.find("available: game sees only") != std::string::npos : d.find("UNAVAILABLE") != std::string::npos), "describe() states honestly whether the filesystem is isolated");
  check(d.find("PARTIAL") != std::string::npos || !p.usable(), "describe() never claims a strong label");
  check(!p.usable() || (p.userns && p.netns && p.pidns && p.mountns_procfs && p.ipc_uts && p.seccomp && p.filesystem), "probe flags are consistent with usable()");
  if (!p.usable()) { std::printf("SKIP sandbox effect tests: %s\n", p.notes.c_str()); check(!p.notes.empty(), "an unusable sandbox says why (no silent fallback)"); return; }
  const std::string out = "/tmp/pnr_sb_" + std::to_string(::getpid()) + ".txt";
  std::string err;
  int rc = run_sb("test $$ -eq 1 && echo pid1; exit 7", out, &err);
  check(rc == 7 && slurp(out).find("pid1") != std::string::npos, "game is PID 1 and its exit status is mirrored");
  rc = run_sb("exec python3 -c 'import ctypes; ctypes.string_at(0)'", out, &err);
  if (rc == 127) std::printf("SKIP signal mirror test: python3 not found\n");
  else check(rc == 128 + 11, "a fatal signal (SIGSEGV) in the game is mirrored to the caller");
  rc = run_sb("n=0; for f in /proc/[0-9]*; do n=$((n+1)); done; test $n -le 4", out, &err);
  check(rc == 0, "only the sandbox's own processes are visible in /proc");
  rc = run_sb("tail -n +3 /proc/net/dev | cut -d: -f1 | tr -d ' '", out, &err);
  check(rc == 0 && slurp(out) == "lo\n", "network namespace has only the loopback device");
  rc = run_sb("test \"$(id -u)\" = \"" + std::to_string(::getuid()) + "\"", out, &err);
  check(rc == 0, "no fake root: same uid inside as outside");
  rc = run_sb("python3 - <<'PY'\nimport ctypes,sys\nl=ctypes.CDLL(None,use_errno=True)\nr=l.ptrace(0,0,0,0)\nsys.exit(0 if (r==-1 and ctypes.get_errno()==1) else 3)\nPY", out, &err);
  if (rc == 127) std::printf("SKIP seccomp ptrace test: python3 not found\n");
  else check(rc == 0, "seccomp denies ptrace with EPERM");
  rc = run_sb("python3 - <<'PY'\nimport ctypes,sys\nl=ctypes.CDLL(None,use_errno=True)\nr=l.mount(b'x',b'/mnt',b'tmpfs',0,None)\nsys.exit(0 if (r==-1 and ctypes.get_errno()==1) else 3)\nPY", out, &err);
  if (rc == 127) std::printf("SKIP seccomp mount test: python3 not found\n");
  else check(rc == 0, "seccomp denies mount with EPERM");
  rc = run_sb("python3 - <<'PY'\nimport socket,sys\ns=socket.socket(); s.settimeout(1)\ntry:\n  s.connect(('127.0.0.1',9)); sys.exit(4)\nexcept OSError:\n  sys.exit(0)\nPY", out, &err);
  if (rc != 127) check(rc == 0, "outbound connect fails inside the network namespace");
  rc = run_sb("echo x > /tmp/pnr_sb_fs_probe_" + std::to_string(::getpid()) + "; exit 0", out, &err);
  const std::string probe_file = "/tmp/pnr_sb_fs_probe_" + std::to_string(::getpid());
  check(rc == 0 && ::access(probe_file.c_str(), F_OK) == 0, "HONEST: plain enter() without a Policy does NOT isolate the filesystem (a write inside is visible outside)");
  ::unlink(probe_file.c_str()); ::unlink(out.c_str());
  { std::string e; pid_t c = ::fork(); if (c == 0) { if (!pnr::sandbox::enter(&e)) _exit(125); _exit(::getpid() == 1 ? 0 : 9); } int st = 0; ::waitpid(c, &st, 0);
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "enter() returns true only in the PID 1 process"); }

  {  // slice B2b: filesystem isolation, exercised for real
    namespace fs = std::filesystem;
    const std::string base = "/tmp/pnr_fs_" + std::to_string(::getpid());
    fs::create_directories(base + "/game"); fs::create_directories(base + "/data"); fs::create_directories(base + "/shim");
    { std::ofstream(base + "/secret.txt") << "TOPSECRET"; std::ofstream(base + "/game/asset.txt") << "asset";
      std::ofstream(base + "/shim/lib.txt") << "shim"; std::ofstream(base + "/game/run.sh") << "#!/bin/sh\necho started-from-game-dir; exit 5\n"; }
    fs::permissions(base + "/game/run.sh", fs::perms::owner_all);
    pnr::sandbox::Policy pol; pol.game_dir = base + "/game"; pol.rw_dirs = {base + "/data"}; pol.ro_dirs = {base + "/shim"};
    const std::string o2 = base + "/out.txt";
    // out.txt lives under base, which the game cannot see: redirect to the rw dir instead
    const std::string o3 = base + "/data/out.txt";
    auto ok = [&](const std::string& s) { return run_fs(pol, s, o3) == 0; };
    check(ok("test -f " + base + "/game/asset.txt && test -f " + base + "/shim/lib.txt"), "fs: the game dir and extra ro dirs are visible at their own paths");
    check(ok("test ! -e " + base + "/secret.txt"), "fs: a host file next to the game dir is NOT visible");
    check(ok("test ! -e /etc/passwd && test ! -e /home && test ! -e /root && test ! -e /sys/kernel"), "fs: host /etc/passwd, /home, /root and /sys are not there");
    check(ok("test ! -e /.oldroot"), "fs: the old root is gone (detached and removed)");
    check(ok("! touch " + base + "/game/x 2>/dev/null && ! touch /x 2>/dev/null && ! touch /usr/x 2>/dev/null") && !fs::exists(base + "/game/x"), "fs: game dir, / and /usr are read-only");
    check(ok("echo hi > " + base + "/data/w.txt") && fs::exists(base + "/data/w.txt"), "fs: the rw dir (jail) is writable and the write lands on the host");
    check(ok("echo t > /tmp/pnr_inside_probe; test -f /tmp/pnr_inside_probe") && !fs::exists("/tmp/pnr_inside_probe"), "fs: /tmp is private (a write there is not visible outside)");
    check(ok("ln -s /etc/passwd " + base + "/data/l1; ln -s " + base + "/secret.txt " + base + "/data/l2; ! cat " + base + "/data/l1 >/dev/null 2>&1 && ! cat " + base + "/data/l2 >/dev/null 2>&1"), "fs: symlinks to outside paths resolve inside the sandbox root, not the host");
    check(ok("echo x > /dev/null && head -c 4 /dev/urandom >/dev/null && test \"$(head -c 3 /dev/zero | wc -c)\" = 3"), "fs: /dev/null, /dev/urandom and /dev/zero work");
    check(ok("n=0; for f in /dev/*; do n=$((n+1)); done; test $n -le 6"), "fs: /dev is minimal");
    check(ok("test -d /proc/self && test $$ -eq 1"), "fs: a fresh /proc is mounted and the game is still PID 1");
    // exec the game binary at its own path
    { pid_t c = ::fork(); if (c == 0) { std::string e; if (!pnr::sandbox::enter(pol, &e)) _exit(125);
        int fd = ::open(o3.c_str(), O_WRONLY | O_TRUNC | O_CREAT, 0644); if (fd >= 0) { ::dup2(fd, 1); }
        ::execl((base + "/game/run.sh").c_str(), "run.sh", static_cast<char*>(nullptr)); _exit(127); }
      int st = 0; ::waitpid(c, &st, 0);
      check(WIFEXITED(st) && WEXITSTATUS(st) == 5 && slurp(o3).find("started-from-game-dir") != std::string::npos, "fs: a script in the game dir executes at its own path (shebang + libs resolve in the new root)"); }
    check(ok("test \"$(pwd)\" = " + base + "/game"), "fs: cwd is the game dir");
    { pnr::sandbox::Policy bad; bad.game_dir = "relative/dir"; std::string e; pid_t c = ::fork();
      if (c == 0) { _exit(pnr::sandbox::enter(bad, &e) ? 0 : 1); } int st = 0; ::waitpid(c, &st, 0);
      check(WIFEXITED(st) && WEXITSTATUS(st) == 1, "fs: a non-absolute policy path is refused, not guessed"); }
    { pnr::sandbox::Policy bad = pol; bad.ro_dirs = {base + "/does-not-exist"}; std::string e; pid_t c = ::fork();
      if (c == 0) { _exit(pnr::sandbox::enter(bad, &e) ? 0 : 1); } int st = 0; ::waitpid(c, &st, 0);
      check(WIFEXITED(st) && (WEXITSTATUS(st) == 125 || WEXITSTATUS(st) == 1), "fs: a missing ro dir fails loudly instead of being skipped"); }
    std::error_code ec; fs::remove_all(base, ec); (void)o2;
    bool left = false; for (auto& de : fs::directory_iterator("/tmp", ec)) if (de.path().filename().string().rfind("pnr-sb-", 0) == 0) left = true;
    check(!left, "fs: no leftover mountpoint dirs in /tmp");
  }
}
