# AnythingPS5 by AK

**PNR — PlayStation Native Runtime.** A research project exploring native
x86-64 game compatibility on Windows and Linux: a compatibility-layer
runtime, Vulkan graphics backend (planned), and an adaptive performance
engine for ordinary PCs and laptops, including low-end hardware.

> **Honest status:** PNR cannot run any commercial PS5 game. No Sony
> firmware, SDK code, keys, or game files are used or distributed, and no
> DRM is bypassed. What exists today is a real, tested foundation: a core
> runtime, a native executable loader, a compatibility services layer, a
> CLI, and a demo application — all building and passing tests on Linux.

## What works right now (Phase 0 + Phase 1 prototype)

| Capability | Status |
|---|---|
| Core runtime: lifecycle state machine, event bus, config, capability registry, error reporting | Implemented, Tested |
| ELF64 binary parser + validator (bounds-checked, specific errors) | Implemented, Tested |
| Native loader: validated binaries executed in a supervised separate process | Implemented (Linux), Tested |
| PS NID computation + symbol lookup (SHA-1 based; algorithm adapted from [AnyPS5](https://github.com/boykopovar/AnyPS5), GPL-2.0) | Implemented, Tested |
| PRX system-library layer: libkernel (usleep/sleep/alloc/free/thread/mmap/mprotect/munmap/query; jailed file calls open/read/write/close/lseek/fstat) + libSceLibcInternal (printf-bounded/malloc/free/memcpy/memset/strlen/strcmp) bound by NID; taxonomy per AnyPS5, original implementations | Implemented, Tested |
| Loader import resolution: ELF dynamic imports classified host / prx-implemented / prx-declared / unknown at launch; unimplemented never faked | Implemented, Tested |
| PS-ABI shim + PS-style demo: `libpnr_psabi.so` exports real sce functions (alloc/free, usleep, thread, mmap family, jailed file I/O); `ps_demo` imports only sce* + libc and `pnr run` dispatches every sce import for real | Implemented, Tested |
| Compat services: memory, timing, jailed filesystem, threads | Implemented, Tested |
| Host OS abstraction (IF-OS): Linux backend (CPU/memory/power/process launch) | Implemented (Linux), Tested |
| `.pnrgame` package format: ustar + SHA-256 manifests, builder | Implemented, Tested |
| Controlled importer: checksum verification, ELF validation, traversal protection, library registration | Implemented, Tested |
| Game library metadata: SQLite store (games, installations, compatibility, launches) | Implemented, Tested |
| CLI driving the full 10-step launch workflow with honest per-step results | Implemented, Tested |
| Developer-controlled demo app running only on compat services | Implemented, Tested |
| Vulkan graphics backend | Blocked in the dev sandbox (no GPU); Planned (Phase 2) |
| Adaptive Performance Engine (deterministic rules, hardware class heuristic, closed-loop controller) | Implemented, Tested |
| Laptop Optimization Manager (battery/balanced/max-performance caps) | Implemented, Tested |
| Game services: virtual filesystem (jailed areas, per-game/user save isolation) | Implemented, Tested |
| Game services: input (evdev enumeration, mapping profiles, virtual device) | Implemented, Tested |
| Game services: audio (interface + honest null backend) | Interface Implemented, host backends Planned |
| Windows backend | Implemented (hwinfo via Win32; ELF launch Blocked, cross-compile CI-verified) |
| Rust components | Planned (no Rust toolchain in current sandbox) |
| Any commercial PS5 software support | Blocked (proprietary formats, authorization, DRM — see docs) |

Latest measured test result on Linux x86-64 (g++ 11.4, CMake 3.22):
**194/194 unit tests pass, 3/3 CTest suites pass** (unit, end-to-end native
launch, and full package workflow: build -> import -> list -> run imported
game). No benchmarks exist yet; none are claimed.

## Build (Linux)

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Run the demo

```sh
cd build
./pnr run ./pnr_test_app     # validate + natively execute the demo app
./pnr validate ./pnr_test_app
./pnr hwinfo

# package workflow
./pnr_make_package demo.pnrgame demo-game "PNR Demo Game" 0.1.0 bin/game bin/game=./pnr_test_app
./pnr import demo.pnrgame    # verifies checksums + ELF, installs, registers
./pnr games                  # library listing from SQLite
./pnr run ./pnr_data/games/demo-game-0.1.0/bin/game
```


## Example output

Captured verbatim from a real session on this repo's Linux build (the
dev sandbox has no GPU or audio device, and that is exactly what the
output says - nothing is faked):

```text
PNR hardware assessment (heuristic, honest bounds):
hardware.class=A (budget / ultra low-end) (CPU/RAM only; GPU unknown in this build)
power.default_mode=balanced (battery state unknown: balanced)
caps.fps_cap=60 caps.min_resolution_scale=75% caps.max_background_tasks=2
note: compatibility is never decided from class alone - real frame telemetry drives the adaptive controller

=== $ pnr hwinfo ===
PNR hardware snapshot (host truth, no estimates):
host.platform=linux
cpu.logical_cores=2
mem.total_mb=1982
mem.available_mb=1700
power.on_battery=unknown (no power_supply node on this host)
gpu.vulkan=UNAVAILABLE in this sandbox (no GPU/Vulkan driver); capability-based detection lands with Phase 2

=== $ pnr input ===
input.backend=evdev
input.devices=0
  (no input devices visible on this host)

=== $ pnr audio ===
audio.backend=null
audio.devices=0
  (no audio devices available; host audio backends Planned)

=== $ pnr games (empty library) ===
library is empty (import a .pnrgame first)

=== $ pnr import demo.pnrgame ===
IMPORT OK: PNR Test App 1.0.0 (x86-64)
  id=pnr-test-app
  installed=./pnr_data/games/pnr-test-app-1.0.0
  compatibility=Untested (no execution test yet)

=== $ pnr games ===
PNR Test App  v1.0.0  [Untested]
    ./pnr_data/games/pnr-test-app-1.0.0

=== $ pnr run pnr_test_app ===
pnr_test_app: native x86-64 demo starting
pnr_test_app: args=0
pnr_test_app: memory ok (4096 bytes outstanding)
pnr_test_app: thread result=100000
pnr_test_app: sleep(5ms) measured 5.066ms
pnr_test_app: save-data write+readback ok
pnr_test_app: memory released (0 outstanding)
pnr_test_app: ALL OK - exiting 0
[1-validate] SUCCESS   ./build-rel/pnr_test_app is a valid x86-64 ELF (13 segments)
[2-analysis] SUCCESS   logical cores=2; Vulkan=unavailable in sandbox (recorded, not guessed)
[3-config] SUCCESS   default profile 'balanced' (no game profile found; defaults used openly)
  [event] lifecycle -> VALIDATING
  [event] lifecycle -> INITIALIZING
  [event] lifecycle -> READY
[4-runtime] SUCCESS   compat memory/fs/thread services up
[5-graphics] BLOCKED   no Vulkan device in this environment; demo app is non-graphical. Blocked is reported, not papered over.
[6-content] SUCCESS   save-data dir jailed at ./pnr_data/saves
--- game output begin ---
  [event] lifecycle -> RUNNING
  [monitor] wall=0.007s exit=0
--- game output end ---
[7-start] SUCCESS   game ran natively in child process, exit code 0
[8-monitor] SUCCESS   wall time + exit status measured (frame-time telemetry needs the graphics phase)
[9-optimize] SKIPPED   deterministic adaptive controller is Phase 4 work
  [event] lifecycle -> STOPPING
  [event] lifecycle -> TERMINATED
[10-shutdown] SUCCESS   resources released, 0 errors recorded
RESULT: RUN OK (real native execution)
```

Note: this sandbox has the SQLite runtime but no dev headers, so the repo
carries a minimal API shim (`library/sqlite3_shim.h`). On a normal dev
machine define `PNR_USE_SYSTEM_SQLITE3` and use the system header.

`pnr run` prints the master workflow's ten steps with explicit
SUCCESS / FAILED / BLOCKED / SKIPPED markers — a BLOCKED graphics step is
reported, never hidden.

### `pnr run ./ps_demo` - the PS-style showcase

`ps_demo` imports only `sceKernel*` (plus plain libc) symbols. The CLI
preloads the PS-ABI shim (`LD_PRELOAD`), sets `PNR_JAIL_ROOT`, classifies
the binary's imports, and runs it natively. Captured verbatim:

```text
ps_demo: heap alloc/fill/free ok
ps_demo: map/protect/unmap ok
ps_demo: jailed file write/stat/seek/read ok
ps_demo: usleep measured 20066us
ps_demo: thread ran
ps_demo: ALL SCE CALLS DISPATCHED OK
[1-validate] SUCCESS   ./ps_demo is a valid x86-64 ELF (13 segments)
[1-imports] SUCCESS   22 imports: 8 host-resolved, 14 prx-implemented, 0 prx-declared-only, 0 unknown
  [import] sceKernelMTypeprotect            prx-implemented
  [import] sceKernelMapMemory               prx-implemented
  [import] sceKernelMunmap                  prx-implemented
  [import] sceKernelCreateThread            prx-implemented
  [import] sceKernelWrite                   prx-implemented
  [import] sceKernelOpen                    prx-implemented
  [import] sceKernelFstat                   prx-implemented
  [import] sceKernelLseek                   prx-implemented
  [import] sceKernelFree                    prx-implemented
  [import] strcmp                           prx-implemented
  [import] sceKernelUsleep                  prx-implemented
  [import] sceKernelClose                   prx-implemented
  [import] sceKernelRead                    prx-implemented
  [import] sceKernelAlloc                   prx-implemented
[2-analysis] SUCCESS   logical cores=2; Vulkan=unavailable in sandbox (recorded, not guessed)
[3-config] SUCCESS   default profile 'balanced' (no game profile found; defaults used openly)
  [event] lifecycle -> VALIDATING
  [event] lifecycle -> INITIALIZING
  [event] lifecycle -> READY
[4-runtime] SUCCESS   compat memory/fs/thread services up
[5-graphics] BLOCKED   no Vulkan device in this environment; demo app is non-graphical. Blocked is reported, not papered over.
[6-content] SUCCESS   save-data dir jailed at ./pnr_data/saves
--- game output begin ---
  [event] lifecycle -> RUNNING
  [monitor] wall=0.023s exit=0
--- game output end ---
[7-start] SUCCESS   game ran natively in child process, exit code 0
[8-monitor] SUCCESS   wall time + exit status measured (frame-time telemetry needs the graphics phase)
[9-optimize] SKIPPED   deterministic adaptive controller is Phase 4 work
  [event] lifecycle -> STOPPING
  [event] lifecycle -> TERMINATED
[10-shutdown] SUCCESS   resources released, 0 errors recorded
RESULT: RUN OK (real native execution)
```

Every `sceKernel*` import classifies **prx-implemented** and the calls do
real work through the shim: heap via `malloc`, anonymous memory via
`mmap`/`mprotect`, files through the jailed file service (the proof file
lands inside `./pnr_data/jail`), and a real measured sleep + thread.

## Windows build

Cross-compile from Linux with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw)
(UCRT, x86-64) and the SQLite amalgamation:

    PATH=<llvm-mingw>/bin:$PATH cmake -S . -B build-win \
      -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
      -DPNR_SQLITE3_AMALGAMATION=<dir with sqlite3.c/sqlite3.h>
    cmake --build build-win -j

This produces `pnr.exe`, `pnr_make_package.exe` and `pnr_test_app.exe`,
statically linked (no MinGW runtime DLLs needed). Verified: compiles and
links with clang/llvm-mingw. Not yet executed on real Windows hardware in
this project - stated, not assumed. `pnr hwinfo` works via the Win32 API;
`pnr run` is Blocked on Windows because PNR loads ELF (Linux-format)
binaries, which Windows cannot execute - a PE/Win32 toolchain path for the
runtime is Phase 1 follow-up work.

## Releases

Prebuilt binaries (Linux x86-64, Windows x86-64 preview) will be published
on the [Releases](https://github.com/AKgame47/AnythingPS5/releases) page
once the current code increments settle. Any release stays a prototype
demo, not a real PS5 runtime - the release notes say exactly what works.

## Repository layout

```
apps/        pnr CLI now; desktop launcher + diagnostics later
core/        MODULE 01 - runtime, lifecycle, events, config, capabilities
loader/      MODULE 02 - ELF parser (A), native loader (B), NID support, format adapters (C, planned)
prx/         MODULE 02 companion - PS system-library layer bound by NID (libkernel subset implemented);
             import resolution for launched binaries (loader/import_resolver.*)
compat/      MODULE 03 - memory, timing, filesystem, thread services
platform/    MODULE 07 - host OS abstraction, Linux + Windows backends (Windows launch blocked)
ape/         MODULE 05 - adaptive performance engine (classification, frame stats, controller)
laptop/      MODULE 06 - laptop optimization manager (power modes, caps)
package/     .pnrgame format: ustar, SHA-256, importer
library/     MODULE 09 - SQLite game/installation/compatibility/launch store
services/    MODULE 08 - virtual filesystem, input, audio backends
tools/       pnr_make_package - dev package builder
examples/    pnr_test_app - developer-controlled native demo "game"
tests/       unit tests (no framework) + end-to-end CTest + package workflow
docs/        architecture, feasibility, risk register, interface contracts
```

Planned (per the master prompt's structure): `graphics/`, `performance/`,
`platform/`, `services/`, `security/`, `library/`, `benchmarks/`, `ci/`.

## Documentation

- [Architecture specification](docs/architecture.md)
- [Feasibility report + assumptions + PoC test plan](docs/feasibility-report.md)
- [Risk register](docs/risk-register.md)
- [Interface contracts](docs/interface-contracts.md)

## Ground rules (from the master prompt)

1. Never fabricate PS5 functionality, benchmarks, or test results.
2. Never bypass security, licensing, DRM, or thermal protections.
3. Mark every feature Planned / Implemented / Tested / Experimental /
   Blocked — honestly.
4. Execute supported native x86-64 code directly; no emulation, no VMs,
   no streaming.
5. Report a blocked capability as blocked.

## License

GPL-2.0-only (see LICENSE). Chosen so design and code from
[boykopovar/AnyPS5](https://github.com/boykopovar/AnyPS5) (GPL-2.0) can be
reused with attribution where it genuinely helps.
