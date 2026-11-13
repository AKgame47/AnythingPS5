# PNR sandbox (slice B2)

Status: **Linux x86-64: Implemented + Tested here (real namespaces, real seccomp, real exec). Label: PARTIAL. Windows: Planned, nothing built or tested.**

Opt-in per launch: `pnr-launcher launch <game> --sandbox`. Check the host first with `pnr-launcher sandbox probe`.

## What it enforces (Linux)

- **User namespace** with the same uid and gid inside as outside (no fake root, no capabilities outside the namespace). A helper process outside the namespace writes the id maps, because some environments make `/proc/self/uid_map` unwritable from inside.
- **Network namespace**: no interfaces except a loopback that is down, so no network at all. Listeners on the host are unreachable.
- **PID namespace** with a fresh private `/proc`: the game is PID 1 and sees only its own processes. PNR forks once more after unsharing, waits, and mirrors the game's exit code or fatal signal to the launcher.
- **IPC and UTS namespaces**, and a **private mount namespace** (mounts do not propagate).
- **no_new_privs** and a **seccomp-bpf denylist** answering EPERM for: ptrace, process_vm_readv/writev, mount and the new mount API calls, umount2, pivot_root, chroot, setns, unshare, kexec, module loading, bpf, perf_event_open, keyctl family, reboot, swapon/off, acct, clock and time setting, open_by_handle_at, userfaultfd, iopl/ioperm, syslog, quotactl, sethostname/setdomainname. A call from another ABI (x32) or architecture kills the process.

## What it does NOT do

- **No filesystem isolation.** The game can read and write whatever your user can. (A test asserts this honestly.) `/sys` is also the host's. Planned: Landlock or a pivot_root root.
- A **denylist** is weaker than an allowlist. An allowed syscall used the wrong way, or a kernel bug, can still escape. Namespaces are not a security boundary against a kernel exploit.
- No resource limits (CPU, memory, processes) yet.
- It is **not** a reason to run untrusted code. Until filesystem isolation and an allowlist exist, treat PNR as running trusted developer demos or packages you trust.
- x86-64 only. On other architectures or non-Linux the sandbox reports itself unusable.

## No silent fallback

`launch --sandbox` first runs the probe in forked children. If any piece is unavailable (many distributions and containers disable unprivileged user namespaces), the game is **not started**: `LAUNCH NOT STARTED: --sandbox was requested but the sandbox is not usable here (...)`. Without `--sandbox`, launches behave as before and are not isolated.

## Real captured probe output (this build host)

```
SANDBOX (Linux, partial isolation; filesystem NOT isolated)
  user namespace (no real privileges): available
  network namespace (no network):      available
  PID namespace + private /proc:       available
  IPC + UTS namespaces:                available
  seccomp denylist + no_new_privs:     available
  filesystem isolation:                NOT PROVIDED (the game can read and write what your user can)
  status: usable. Label: PARTIAL. A denylist is not an allowlist; do not run untrusted code on this alone.
```

## Tests

`tests/sandbox_tests.cpp` forks, enters the sandbox and execs real programs: PID 1 and exit status mirroring, SIGSEGV mirroring, only own processes in /proc, only `lo` in /proc/net/dev, same uid, seccomp EPERM for ptrace and mount, outbound connect fails, and the honest check that a file written inside is visible outside. If the host does not permit user namespaces the effect tests print SKIP. The `pnr_store_workflow` CTest launches the demo game with `--sandbox` through the real loader (or checks it refuses when the sandbox is unusable).

## Windows (notes only, Planned, untested)

Windows has no namespaces or seccomp. The nearest tools are: a **Job object** (limit active processes, memory, CPU time, kill on close, UI restrictions), a **restricted token** or low integrity level, and **AppContainer** for file and network capability limits, plus process mitigation policies. None of this is built, and none has been run. The Windows launch backend itself is still Planned. Until then, `--sandbox` on Windows would be refused, not faked.
