// See sandbox.hpp.
#include "sandbox/sandbox.hpp"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#if defined(__linux__) && defined(__x86_64__)
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#define PNR_SANDBOX_SUPPORTED 1
#endif

namespace pnr::sandbox {
#ifdef PNR_SANDBOX_SUPPORTED
namespace {
bool write_file(const char* path, const std::string& s) {
  int fd = ::open(path, O_WRONLY);
  if (fd < 0) return false;
  ssize_t n = ::write(fd, s.data(), s.size());
  ::close(fd);
  return n == static_cast<ssize_t>(s.size());
}

// Some environments make /proc/self/*_map unwritable from inside the new user
// namespace, so (like unshare --map-root-user) a helper process OUTSIDE it
// writes the maps. Same uid/gid inside as outside: no fake root.
bool new_userns(std::string* err) {
  int to_helper[2], from_helper[2];
  if (::pipe(to_helper) != 0 || ::pipe(from_helper) != 0) { *err = "pipe failed"; return false; }
  const pid_t parent = ::getpid(); const uid_t uid = ::getuid(); const gid_t gid = ::getgid();
  pid_t h = ::fork();
  if (h < 0) { *err = "fork failed"; return false; }
  if (h == 0) {
    char c; ::close(to_helper[1]); ::close(from_helper[0]);
    if (::read(to_helper[0], &c, 1) != 1) _exit(1);
    const std::string base = "/proc/" + std::to_string(parent) + "/";
    bool ok = write_file((base + "setgroups").c_str(), "deny") &&
              write_file((base + "uid_map").c_str(), std::to_string(uid) + " " + std::to_string(uid) + " 1\n") &&
              write_file((base + "gid_map").c_str(), std::to_string(gid) + " " + std::to_string(gid) + " 1\n");
    char r = ok ? 1 : 0; if (::write(from_helper[1], &r, 1) != 1) _exit(1);
    _exit(0);
  }
  ::close(to_helper[0]); ::close(from_helper[1]);
  if (::unshare(CLONE_NEWUSER) != 0) { *err = std::string("unshare(user namespace): ") + std::strerror(errno); ::close(to_helper[1]); ::close(from_helper[0]); int st; ::waitpid(h, &st, 0); return false; }
  char go = 1; if (::write(to_helper[1], &go, 1) != 1) { *err = "helper write failed"; return false; }
  char r = 0; ssize_t n = ::read(from_helper[0], &r, 1);
  ::close(to_helper[1]); ::close(from_helper[0]);
  int st = 0; while (::waitpid(h, &st, 0) < 0 && errno == EINTR) {}
  if (n != 1 || r != 1) { *err = "could not write uid/gid maps for the new user namespace"; return false; }
  return true;
}

const int kDeny[] = {
  SYS_ptrace, SYS_process_vm_readv, SYS_process_vm_writev, SYS_mount, SYS_umount2, SYS_pivot_root, SYS_chroot,
  SYS_setns, SYS_unshare, SYS_kexec_load, SYS_init_module, SYS_finit_module, SYS_delete_module, SYS_bpf,
  SYS_perf_event_open, SYS_keyctl, SYS_add_key, SYS_request_key, SYS_reboot, SYS_swapon, SYS_swapoff, SYS_acct,
  SYS_settimeofday, SYS_clock_settime, SYS_clock_adjtime, SYS_adjtimex, SYS_open_by_handle_at, SYS_name_to_handle_at,
  SYS_userfaultfd, SYS_iopl, SYS_ioperm, SYS_syslog, SYS_quotactl, SYS_sethostname, SYS_setdomainname,
#ifdef SYS_kexec_file_load
  SYS_kexec_file_load,
#endif
#ifdef SYS_mount_setattr
  SYS_mount_setattr,
#endif
#ifdef SYS_move_mount
  SYS_move_mount,
#endif
#ifdef SYS_open_tree
  SYS_open_tree,
#endif
#ifdef SYS_fsopen
  SYS_fsopen, SYS_fsconfig, SYS_fsmount, SYS_fspick,
#endif
};

bool install_seccomp(std::string* err) {
  std::vector<sock_filter> f;
  auto stmt = [&](uint16_t code, uint32_t k) { f.push_back(BPF_STMT(code, k)); };
  auto jump = [&](uint16_t code, uint32_t k, uint8_t jt, uint8_t jf) { f.push_back(BPF_JUMP(code, k, jt, jf)); };
  stmt(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch));
  const size_t arch_jump = f.size(); jump(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 0, 0);  // jf -> KILL (patched)
  stmt(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr));
  const size_t x32_jump = f.size(); jump(BPF_JMP | BPF_JGE | BPF_K, 0x40000000u, 0, 0);          // x32 ABI bypass -> KILL (patched)
  const size_t first = f.size();
  for (int nr : kDeny) jump(BPF_JMP | BPF_JEQ | BPF_K, static_cast<uint32_t>(nr), 0, 0);       // jt -> DENY (patched)
  const size_t allow = f.size(); stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
  const size_t deny = f.size(); stmt(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
  const size_t kill = f.size(); stmt(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
  (void)allow;
  f[arch_jump].jf = static_cast<uint8_t>(kill - arch_jump - 1);
  f[x32_jump].jt = static_cast<uint8_t>(kill - x32_jump - 1);
  for (size_t i = first; i < allow; i++) f[i].jt = static_cast<uint8_t>(deny - i - 1);
  sock_fprog prog{static_cast<unsigned short>(f.size()), f.data()};
  if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) { *err = "no_new_privs failed"; return false; }
  if (::syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog) != 0) { *err = std::string("seccomp filter rejected: ") + std::strerror(errno); return false; }
  return true;
}


bool mkdir_p(const std::string& p) {
  std::error_code ec; std::filesystem::create_directories(p, ec);
  return !ec || std::filesystem::is_directory(p);
}
bool abs_ok(const std::string& p) { return p.size() > 1 && p[0] == '/' && p.find("/../") == std::string::npos && (p.size() < 3 || p.compare(p.size() - 3, 3, "/..") != 0); }

// Bind src (dir or file) to root+src. nosuid,nodev always; read-only when asked.
// Inherited flags are carried over, as the kernel requires for mounts from outside the userns.
bool bind_into(const std::string& root, const std::string& src, bool ro, std::string* err, bool devnode = false) {
  struct stat st;
  if (::stat(src.c_str(), &st) != 0) { *err = "cannot bind " + src + ": " + std::strerror(errno); return false; }
  const std::string dst = root + src;
  if (S_ISDIR(st.st_mode)) { if (!mkdir_p(dst)) { *err = "cannot create mountpoint " + dst; return false; } }
  else {
    if (!mkdir_p(std::filesystem::path(dst).parent_path().string())) { *err = "cannot create mountpoint dir for " + dst; return false; }
    int fd = ::open(dst.c_str(), O_CREAT | O_WRONLY, 0644); if (fd < 0) { *err = "cannot create mountpoint " + dst; return false; } ::close(fd);
  }
  if (::mount(src.c_str(), dst.c_str(), nullptr, MS_BIND, nullptr) != 0) { *err = "bind mount " + src + ": " + std::strerror(errno); return false; }
  struct statvfs sv; unsigned long fl = MS_BIND | MS_REMOUNT | MS_NOSUID | (devnode ? 0 : MS_NODEV) | (ro ? MS_RDONLY : 0);
  if (::statvfs(dst.c_str(), &sv) == 0) {
    if (sv.f_flag & ST_NOEXEC) fl |= MS_NOEXEC;
    if (sv.f_flag & ST_NOATIME) fl |= MS_NOATIME;
    if (sv.f_flag & ST_NODIRATIME) fl |= MS_NODIRATIME;
    if (sv.f_flag & ST_RELATIME) fl |= MS_RELATIME;
  }
  if (::mount(nullptr, dst.c_str(), nullptr, fl, nullptr) != 0) { *err = "remount " + src + (ro ? " read-only: " : ": ") + std::strerror(errno); return false; }
  return true;
}

// Runs in the PID 1 grandchild after the mount ns has been made private.
// newroot is an existing empty host directory.
bool build_root(const Policy& pol, const std::string& newroot, std::string* err) {
  if (::unshare(CLONE_NEWNS) != 0) { *err = std::string("second mount namespace: ") + std::strerror(errno); return false; }
  if (::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) { *err = std::string("making mounts private: ") + std::strerror(errno); return false; }
  if (::mount("tmpfs", newroot.c_str(), "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755,size=4m") != 0) { *err = std::string("mounting the new root tmpfs: ") + std::strerror(errno); return false; }
  auto tmpfs_at = [&](const char* sub, const char* opts) {
    const std::string d = newroot + sub;
    if (!mkdir_p(d)) { *err = std::string("mkdir ") + d; return false; }
    if (::mount("tmpfs", d.c_str(), "tmpfs", MS_NOSUID | MS_NODEV, opts) != 0) { *err = std::string("mounting tmpfs on ") + sub + ": " + std::strerror(errno); return false; }
    return true;
  };
  if (!tmpfs_at("/tmp", "mode=1777,size=256m") || !tmpfs_at("/dev", "mode=0755,size=1m") || !tmpfs_at("/dev/shm", "mode=1777,size=64m")) return false;
  // system dirs: a host symlink (e.g. /lib -> usr/lib) is recreated as a symlink
  for (const char* d : {"/usr", "/lib", "/lib64", "/lib32", "/bin", "/sbin"}) {
    struct stat ls;
    if (::lstat(d, &ls) != 0) continue;
    if (S_ISLNK(ls.st_mode)) {
      char buf[512]; ssize_t n = ::readlink(d, buf, sizeof buf - 1);
      if (n <= 0) { *err = std::string("readlink ") + d; return false; }
      buf[n] = 0; if (::symlink(buf, (newroot + d).c_str()) != 0) { *err = std::string("symlink ") + d; return false; }
    } else if (!bind_into(newroot, d, true, err)) return false;
  }
  if (::access("/etc/ld.so.cache", F_OK) == 0 && !bind_into(newroot, "/etc/ld.so.cache", true, err)) return false;
  for (const char* d : {"null", "zero", "full", "random", "urandom"})
    if (!bind_into(newroot, std::string("/dev/") + d, false, err, true)) return false;
  if (!bind_into(newroot, pol.game_dir, true, err)) return false;
  for (const auto& d : pol.ro_dirs) if (!bind_into(newroot, d, true, err)) return false;
  for (const auto& d : pol.rw_dirs) if (!bind_into(newroot, d, false, err)) return false;
  if (!mkdir_p(newroot + "/proc") || ::mount("proc", (newroot + "/proc").c_str(), "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr) != 0) { *err = std::string("mounting a private /proc: ") + std::strerror(errno); return false; }
  const std::string old = newroot + "/.oldroot";
  if (::mkdir(old.c_str(), 0700) != 0) { *err = "mkdir .oldroot"; return false; }
  if (::syscall(SYS_pivot_root, newroot.c_str(), old.c_str()) != 0) { *err = std::string("pivot_root: ") + std::strerror(errno); return false; }
  if (::chdir("/") != 0) { *err = "chdir /"; return false; }
  if (::umount2("/.oldroot", MNT_DETACH) != 0) { *err = std::string("detaching the old root: ") + std::strerror(errno); return false; }
  ::rmdir("/.oldroot");
  if (::mount(nullptr, "/", nullptr, MS_REMOUNT | MS_BIND | MS_RDONLY | MS_NOSUID | MS_NODEV, nullptr) != 0) { *err = std::string("making / read-only: ") + std::strerror(errno); return false; }
  if (::chdir(pol.game_dir.c_str()) != 0) { *err = "chdir to the game dir"; return false; }
  return true;
}

// Runs fn in a forked child, returns its exit status == 0.
template <class F> bool in_child(F fn) {
  pid_t p = ::fork();
  if (p < 0) return false;
  if (p == 0) { _exit(fn() ? 0 : 1); }
  int st = 0; while (::waitpid(p, &st, 0) < 0 && errno == EINTR) {}
  return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}
static bool enter_impl(const Policy* pol, std::string* err) {
  std::string e, newroot;
  if (pol) {
    if (pol->game_dir.empty() || !abs_ok(pol->game_dir)) { *err = "policy: game_dir must be an absolute path"; return false; }
    for (const auto& d : pol->ro_dirs) if (!abs_ok(d)) { *err = "policy: ro dir must be an absolute path: " + d; return false; }
    for (const auto& d : pol->rw_dirs) if (!abs_ok(d)) { *err = "policy: rw dir must be an absolute path: " + d; return false; }
    for (const auto& d : pol->rw_dirs) if (!mkdir_p(d)) { *err = "cannot create the writable dir " + d; return false; }
    char tmpl[] = "/tmp/pnr-sb-XXXXXX";
    if (!::mkdtemp(tmpl)) { *err = std::string("cannot create a mountpoint dir in /tmp: ") + std::strerror(errno); return false; }
    newroot = tmpl;
  }
  auto cleanup = [&] { if (!newroot.empty()) ::rmdir(newroot.c_str()); };
  if (!new_userns(&e)) { *err = e; cleanup(); return false; }
  if (::unshare(CLONE_NEWNS | CLONE_NEWNET | CLONE_NEWIPC | CLONE_NEWUTS | CLONE_NEWPID) != 0) { *err = std::string("unshare(mount/net/ipc/uts/pid): ") + std::strerror(errno); cleanup(); return false; }
  pid_t child = ::fork();
  if (child < 0) { *err = "fork failed"; cleanup(); return false; }
  if (child > 0) {  // intermediate: wait for the game (PID 1 of the new namespace) and mirror its fate
    int st = 0; while (::waitpid(child, &st, 0) < 0 && errno == EINTR) {}
    cleanup();
    if (WIFSIGNALED(st)) { ::signal(WTERMSIG(st), SIG_DFL); ::kill(::getpid(), WTERMSIG(st)); _exit(128 + WTERMSIG(st)); }
    _exit(WIFEXITED(st) ? WEXITSTATUS(st) : 1);
  }
  // grandchild: PID 1 in the new PID namespace
  if (pol) {
    if (!build_root(*pol, newroot, err)) return false;
  } else {
    if (::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) { *err = std::string("making mounts private: ") + std::strerror(errno); return false; }
    if (::mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr) != 0) { *err = std::string("mounting a private /proc: ") + std::strerror(errno); return false; }
  }
  if (!install_seccomp(&e)) { *err = e; return false; }
  return true;
}
}  // namespace

bool enter(std::string* err) { return enter_impl(nullptr, err); }
bool enter(const Policy& policy, std::string* err) { return enter_impl(&policy, err); }

Probe probe() {
  Probe p; p.supported_platform = true;
  std::string notes;
  p.userns = in_child([] { std::string e; return new_userns(&e); });
  if (!p.userns) notes += "user namespaces are not permitted here (kernel setting or container policy); ";
  auto with_user = [&](int flags) { return p.userns && in_child([flags] { std::string e; return new_userns(&e) && ::unshare(flags) == 0; }); };
  p.netns = with_user(CLONE_NEWNET);
  p.ipc_uts = with_user(CLONE_NEWIPC | CLONE_NEWUTS);
  p.pidns = with_user(CLONE_NEWPID);
  if (!p.netns) notes += "network namespace unavailable; ";
  if (!p.pidns) notes += "PID namespace unavailable; ";
  p.mountns_procfs = p.userns && in_child([] {
    std::string e;
    if (!new_userns(&e) || ::unshare(CLONE_NEWNS | CLONE_NEWPID) != 0) return false;
    pid_t c = ::fork(); if (c < 0) return false;
    if (c == 0) _exit((::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) == 0 && ::mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr) == 0) ? 0 : 1);
    int st = 0; ::waitpid(c, &st, 0); return WIFEXITED(st) && WEXITSTATUS(st) == 0; });
  if (!p.mountns_procfs) notes += "private mount namespace or fresh /proc unavailable; ";
  p.seccomp = in_child([] { std::string e; return install_seccomp(&e); });
  if (!p.seccomp) notes += "seccomp filter could not be installed; ";
  char pg[] = "/tmp/pnr-probe-XXXXXX";
  const bool have_pg = ::mkdtemp(pg) != nullptr;
  std::string pgs = have_pg ? pg : "";
  p.filesystem = p.userns && have_pg && in_child([pgs] {
    Policy pol; pol.game_dir = pgs; std::string e;
    if (!enter(pol, &e)) { std::fprintf(stderr, "probe: %s\n", e.c_str()); return false; }
    // only the PID 1 grandchild reaches here, inside the new root
    _exit((::access("/usr", F_OK) == 0 && ::access("/home", F_OK) != 0 && ::access("/.oldroot", F_OK) != 0 && ::access(pgs.c_str(), F_OK) == 0) ? 0 : 1); });
  if (have_pg) ::rmdir(pg);
  if (!p.filesystem) notes += "a pivot_root filesystem root could not be built (see stderr); ";
  p.notes = notes;
  return p;
}
#else
bool enter(std::string* err) { *err = "the sandbox is only built for Linux x86-64"; return false; }
Probe probe() { Probe p; p.notes = "the sandbox is only built for Linux x86-64 (Windows: Planned, see docs/sandbox.md)"; return p; }
#endif

std::string describe(const Probe& p) {
  auto yn = [](bool b) { return b ? "available" : "UNAVAILABLE"; };
  std::string s = "SANDBOX (Linux, partial isolation; filesystem root is a private pivot_root tmpfs)\n";
  s += std::string("  user namespace (no real privileges): ") + yn(p.userns) + "\n";
  s += std::string("  network namespace (no network):      ") + yn(p.netns) + "\n";
  s += std::string("  PID namespace + private /proc:       ") + yn(p.pidns && p.mountns_procfs) + "\n";
  s += std::string("  IPC + UTS namespaces:                ") + yn(p.ipc_uts) + "\n";
  s += std::string("  seccomp denylist + no_new_privs:     ") + yn(p.seccomp) + "\n";
  s += std::string("  filesystem isolation (pivot_root):   ") + (p.filesystem ? "available: game sees only system dirs (ro), its own dir (ro), its jail (rw), private /tmp" : "UNAVAILABLE") + "\n";
  s += p.usable() ? "  status: usable. Label: PARTIAL. A denylist is not an allowlist; do not run untrusted code on this alone.\n"
                  : "  status: NOT usable here: " + p.notes + "\n";
  return s;
}
}  // namespace pnr::sandbox
