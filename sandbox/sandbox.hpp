// PNR Linux sandbox (slice B2). GPL-2.0.
//
// HONEST SCOPE. This is process and kernel-surface isolation for a launched
// game, NOT a complete jail:
//   enforced when probe says so:  new user namespace (no real privileges),
//     new network namespace (no interfaces except a down loopback, so no
//     network), new PID namespace (the game is PID 1 and sees only itself in
//     a fresh /proc), new IPC and UTS namespaces, private mounts,
//     no_new_privs, and a seccomp-bpf DENYLIST (EPERM) of dangerous syscalls.
//   filesystem (slice B2b, enter(Policy)): the game runs in a pivot_root'ed
//     root that is a fresh tmpfs holding ONLY: the system dirs /usr /lib /lib64
//     /bin /sbin and /etc/ld.so.cache (read-only), the game directory
//     (read-only), the game's rw dirs (its jail root), extra read-only dirs
//     (e.g. the PS-ABI shim dir), a private /tmp, a minimal /dev and a fresh
//     /proc. The host's /home, /etc, /sys and everything else are not there.
//     NOT covered: a kernel bug, an allowed syscall misused, the read-only
//     system dirs being readable, and no CPU/memory limits. A denylist is
//     weaker than an allowlist. Do not run untrusted code on the strength of
//     this alone.
//   enter(err) without a Policy is the older namespaces+seccomp mode and does
//     NOT isolate the filesystem.
// Overall label is therefore "partial", never "strong". x86-64 Linux only.
// There is no silent fallback: with require=true an unusable sandbox means the
// game is not started.
#pragma once
#include <string>
#include <vector>

namespace pnr::sandbox {

struct Probe {
  bool supported_platform = false;  // Linux x86-64 build
  bool userns = false, netns = false, pidns = false, mountns_procfs = false, ipc_uts = false;
  bool seccomp = false;
  bool filesystem = false;          // a real pivot_root filesystem root could be built and entered here
  std::string notes;                // why something failed
  bool usable() const { return supported_platform && userns && netns && pidns && mountns_procfs && ipc_uts && seccomp && filesystem; }
};

// Each capability is tested in a forked child; the caller is never affected.
Probe probe();
// Honest multi-line description of what is and is not enforced.
std::string describe(const Probe& p);
// To be called in a forked child before exec. Unshares namespaces, forks once
// more so the game is PID 1 of the new PID namespace (the caller then waits and
// mirrors the game's exit status or fatal signal), mounts a private /proc,
// sets no_new_privs and installs the seccomp denylist. Returns true only in the
// process that should exec the game; false (with *err) on any failure.
bool enter(std::string* err);

// What the game may see. All paths must be absolute. They are mounted at the
// SAME path inside the new root, so the exe path and env vars stay valid.
struct Policy {
  std::string game_dir;                 // read-only
  std::vector<std::string> ro_dirs;     // extra read-only dirs (missing ones are an error)
  std::vector<std::string> rw_dirs;     // read-write dirs, created on the host if missing
};
// Like enter(err) plus the filesystem root described above. cwd becomes game_dir.
bool enter(const Policy& policy, std::string* err);

}  // namespace pnr::sandbox
