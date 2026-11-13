# PNR sandbox (slices B2, B2b, D2a and the signal fix)

Status: **Linux x86-64: Implemented + Tested here (real namespaces, real seccomp, real pivot_root filesystem, real exec). Label: PARTIAL (stronger since B2b, still not "strong"). Windows: Planned, nothing built or tested.**

Opt-in per launch: `pnr-launcher launch <game> --sandbox`. Check the host first with `pnr-launcher sandbox probe`.

## What it enforces (Linux)

- **User namespace** with the same uid and gid inside as outside (no fake root, no capabilities outside the namespace). A helper process outside the namespace writes the id maps, because some environments make `/proc/self/uid_map` unwritable from inside.
- **Network namespace**: no interfaces except a loopback that is down, so no network at all. Listeners on the host are unreachable.
- **PID namespace** with a fresh private `/proc`: the game is PID 1 and sees only its own processes. PNR forks once more after unsharing, waits, and mirrors the game's exit code or fatal signal to the launcher.
- **IPC and UTS namespaces**, and a **private mount namespace** (mounts do not propagate).
- **no_new_privs** and a **seccomp-bpf denylist** answering EPERM for: ptrace, process_vm_readv/writev, mount and the new mount API calls, umount2, pivot_root, chroot, setns, unshare, kexec, module loading, bpf, perf_event_open, keyctl family, reboot, swapon/off, acct, clock and time setting, open_by_handle_at, userfaultfd, iopl/ioperm, syslog, quotactl, sethostname/setdomainname. A call from another ABI (x32) or architecture kills the process.

## What it does NOT do

- The read-only system dirs (`/usr`, `/lib*`, `/bin`, `/sbin`) are visible, so the game can read any world-readable file there. Anything you put in those dirs is exposed.
- Sub-mounts under a bound directory are not carried in (they appear as empty dirs).
- A **denylist** is weaker than an allowlist. An allowed syscall used the wrong way, or a kernel bug, can still escape. Namespaces are not a security boundary against a kernel exploit.
- No resource limits (CPU, memory, processes) for games yet. The `ask` model program is the exception: it gets CPU-time and address-space limits, a wall timeout and an output cap (see below).
- It is **not** a reason to run untrusted code. Filesystem isolation now exists, but without a syscall allowlist and resource limits, still treat PNR as running trusted developer demos or packages you trust.
- x86-64 only. On other architectures or non-Linux the sandbox reports itself unusable.

## Filesystem isolation (slice B2b)

With `launch --sandbox` the game runs in a new root built by `pivot_root`, not your real filesystem. The root is a fresh tmpfs holding only:

| Path | Access | What it is |
| --- | --- | --- |
| `/usr /lib /lib64 /bin /sbin` | read-only | host system dirs (symlinks such as `/lib -> usr/lib` are recreated) |
| `/etc/ld.so.cache` | read-only | the only `/etc` file, so shared libraries resolve |
| the game's install dir | read-only | at its own host path, so the exe path and env vars stay valid |
| the PNR jail root of the game (`PNR_JAIL_ROOT`) | read-write | created if missing; writes land on the host here |
| `LD_LIBRARY_PATH` dirs (the PS-ABI shim dir) | read-only | same path as on the host |
| `/tmp`, `/dev/shm` | read-write tmpfs | private; writes vanish with the sandbox |
| `/dev` | `null zero full random urandom` only | bound from the host |
| `/proc` | fresh | the game's own PID namespace |

Everything else is simply absent: `/home`, `/root`, the rest of `/etc` (so no `/etc/passwd`), `/sys`, `/var`, other mounts. `/` itself is read-only. All mounts are nosuid, and nodev except the five device files. The old root is detached and removed before the game starts, and seccomp then denies `mount`, `pivot_root`, `chroot` and `setns`, so the game cannot undo it. The working directory is the game dir. A symlink the game creates to a host path resolves inside the new root, not on the host.

Failure is loud: a relative path, a missing read-only dir, or a host that cannot do this stops the launch. `sandbox probe` now includes a real attempt to build and enter such a root, and `launch --sandbox` refuses to start if it fails. The old "namespaces + seccomp only" entry point still exists in the library for tests and is clearly named in the header; the launcher never uses it.

Limits of this step: it is one tested Linux host (kernel and container policy differ). Bind mounts are non-recursive. There are still no CPU, memory or process limits. The syscall filter is still a denylist. The scan and `store` commands run outside the sandbox.

## The `ask` model program (slice D2a)

`pnr-launcher ask` runs the user's own Needle 2 executable through this same
sandbox (`--no-model-sandbox` opts out, and the output then says "NOT
sandboxed"). Differences from a game launch:

- The root holds the system dirs (read-only), `/tmp` and `/dev/shm` (private) and one extra
  mount: a private 0700 temp directory with a COPY of the executable and the
  tools file, mounted read-only at its own path. No jail dir, no
  `LD_LIBRARY_PATH` dirs, no user data.
- Same limits as games: world-readable files in the system dirs are visible,
  the seccomp filter is a denylist, kernel bugs are not covered.
- Extra limits that games do not have: CPU-time limit, 1 GiB address-space
  limit, 30 s wall timeout, 1 MiB output cap, cleared environment, no shell. The CPU and memory limits are applied last, right before exec, because an address-space limit set earlier starved the sandbox setup itself (an intermittent failure seen only under AddressSanitizer, fixed).
- No silent fallback: if the probe fails the model is not run.
- Not sandboxed: `manifest suggest`, `scan`, `store` and `research` commands
  run as normal PNR code. They only read the paths you give them (and `store`
  and `research fetch` use the network); none of them executes downloaded code.
- Tested with a probe script (user file hidden, system dirs read-only, only
  `lo`, PID 1) plus a control run without the sandbox. The real binary ran
  sandboxed on the one test host.

## Signals and leftovers (follow-up to D2a)

Before this fix an external SIGTERM, SIGINT or SIGHUP killed only the small
sandbox parent process. The game inside kept running as an orphan (it has no
`PR_SET_PDEATHSIG`, by reading the old code; not re-demonstrated) and the
temporary mountpoint dir `/tmp/pnr-sb-XXXXXX` stayed behind. Now:

- TERM, INT and HUP on the sandbox parent are forwarded to the game. If it is
  still alive after 5 s it gets SIGKILL (PID 1 of a namespace ignores TERM
  without a handler). The parent then removes the mountpoint dir.
- The game sets `PR_SET_PDEATHSIG=SIGKILL`, so if the sandbox parent is
  killed with SIGKILL (which cannot be handled) the whole namespace still dies.
- **Remaining gap:** after a SIGKILL of the parent (or a crash of PNR itself)
  one EMPTY `/tmp/pnr-sb-*` directory can remain. It is harmless; remove it
  by hand. PNR does not sweep old ones on its own because an empty one can
  belong to a sandbox that is still running (the mount lives in another
  namespace). Only `ask`'s timeout path removes its own, best effort.
- Tested: SIGTERM stops the game and removes the dir; SIGKILL leaves no
  orphan process. One test host, not a proof.

## No silent fallback

`launch --sandbox` first runs the probe in forked children. If any piece is unavailable (many distributions and containers disable unprivileged user namespaces), the game is **not started**: `LAUNCH NOT STARTED: --sandbox was requested but the sandbox is not usable here (...)`. Without `--sandbox`, launches behave as before and are not isolated.

## Real captured probe output (this build host)

```
SANDBOX (Linux, partial isolation; filesystem root is a private pivot_root tmpfs)
  user namespace (no real privileges): available
  network namespace (no network):      available
  PID namespace + private /proc:       available
  IPC + UTS namespaces:                available
  seccomp denylist + no_new_privs:     available
  filesystem isolation (pivot_root):   available: game sees only system dirs (ro), its own dir (ro), its jail (rw), private /tmp
  status: usable. Label: PARTIAL. A denylist is not an allowlist; do not run untrusted code on this alone.
```

## Tests

Signal handling and orphan checks are in the same file (see above). `tests/sandbox_tests.cpp` forks, enters the sandbox and execs real programs: PID 1 and exit status mirroring, SIGSEGV mirroring, only own processes in /proc, only `lo` in /proc/net/dev, same uid, seccomp EPERM for ptrace and mount, outbound connect fails, and the honest check that plain `enter()` without a Policy still shares the filesystem. 16 more tests (B2b) enter a real pivot_root root: the game dir and shim dir are visible at their paths; a host file beside the game dir, `/etc/passwd`, `/home`, `/root`, `/sys/kernel` and `/.oldroot` are not; game dir, `/` and `/usr` are read-only; the rw dir is writable and lands on the host; `/tmp` is private; symlinks to outside paths do not escape; `/dev` is minimal and its nodes work; `/proc` is fresh and the game is PID 1; a script in the game dir executes at its own path; cwd is the game dir; relative and missing policy paths are refused; no mountpoint dirs are left in /tmp. If the host does not permit user namespaces the effect tests print SKIP. The `pnr_store_workflow` CTest launches the demo game with `--sandbox` through the real loader (or checks it refuses when the sandbox is unusable).

## Windows (notes only, Planned, untested)

Windows has no namespaces or seccomp. The nearest tools are: a **Job object** (limit active processes, memory, CPU time, kill on close, UI restrictions), a **restricted token** or low integrity level, and **AppContainer** for file and network capability limits, plus process mitigation policies. None of this is built, and none has been run. The Windows launch backend itself is still Planned. Until then, `--sandbox` on Windows would be refused, not faked.
