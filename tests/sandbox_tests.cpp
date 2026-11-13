// Slice B2 tests: the Linux sandbox is exercised for real (fork, namespaces,
// seccomp, exec). If this host does not permit user namespaces the real-effect
// tests are SKIPPED and printed as such; they are never faked as passes of the
// isolation itself.
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
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
std::string slurp(const std::string& p) { std::string s; if (FILE* f = std::fopen(p.c_str(), "r")) { char b[512]; size_t n; while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n); std::fclose(f); } return s; }
}  // namespace

void pnr_sandbox_tests(int& pass, int& fail) {
  auto check = [&](bool ok, const std::string& label) { if (ok) ++pass; else ++fail; std::printf("%s %s\n", ok ? "PASS" : "FAIL", label.c_str()); };
  auto p = pnr::sandbox::probe();
  const std::string d = pnr::sandbox::describe(p);
  check(d.find("filesystem isolation:                NOT PROVIDED") != std::string::npos, "describe() states that the filesystem is not isolated");
  check(d.find("PARTIAL") != std::string::npos || !p.usable(), "describe() never claims a strong label");
  check(!p.usable() || (p.userns && p.netns && p.pidns && p.mountns_procfs && p.ipc_uts && p.seccomp), "probe flags are consistent with usable()");
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
  check(rc == 0 && ::access(probe_file.c_str(), F_OK) == 0, "HONEST: the filesystem is NOT isolated (a write inside is visible outside)");
  ::unlink(probe_file.c_str()); ::unlink(out.c_str());
  { std::string e; pid_t c = ::fork(); if (c == 0) { if (!pnr::sandbox::enter(&e)) _exit(125); _exit(::getpid() == 1 ? 0 : 9); } int st = 0; ::waitpid(c, &st, 0);
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "enter() returns true only in the PID 1 process"); }
}
