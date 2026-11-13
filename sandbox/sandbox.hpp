// PNR Linux sandbox (slice B2). GPL-2.0.
//
// HONEST SCOPE. This is process and kernel-surface isolation for a launched
// game, NOT a complete jail:
//   enforced when probe says so:  new user namespace (no real privileges),
//     new network namespace (no interfaces except a down loopback, so no
//     network), new PID namespace (the game is PID 1 and sees only itself in
//     a fresh /proc), new IPC and UTS namespaces, private mounts,
//     no_new_privs, and a seccomp-bpf DENYLIST (EPERM) of dangerous syscalls.
//   NOT enforced: the filesystem. The game still reads and writes whatever your
//     user can. (Planned: Landlock or a pivot_root root.) A denylist is weaker
//     than an allowlist: a kernel bug, or an allowed syscall misused, can still
//     escape. Do not run untrusted code on the strength of this alone.
// Overall label is therefore "partial", never "strong". x86-64 Linux only.
// There is no silent fallback: with require=true an unusable sandbox means the
// game is not started.
#pragma once
#include <string>

namespace pnr::sandbox {

struct Probe {
  bool supported_platform = false;  // Linux x86-64 build
  bool userns = false, netns = false, pidns = false, mountns_procfs = false, ipc_uts = false;
  bool seccomp = false;
  std::string notes;                // why something failed
  bool usable() const { return supported_platform && userns && netns && pidns && mountns_procfs && ipc_uts && seccomp; }
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

}  // namespace pnr::sandbox
