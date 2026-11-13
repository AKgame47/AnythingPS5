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
| Compat services: memory, timing, jailed filesystem, threads | Implemented, Tested |
| Host OS abstraction (IF-OS): Linux backend (CPU/memory/power/process launch) | Implemented (Linux), Tested |
| `.pnrgame` package format: ustar + SHA-256 manifests, builder | Implemented, Tested |
| Controlled importer: checksum verification, ELF validation, traversal protection, library registration | Implemented, Tested |
| Game library metadata: SQLite store (games, installations, compatibility, launches) | Implemented, Tested |
| CLI driving the full 10-step launch workflow with honest per-step results | Implemented, Tested |
| Developer-controlled demo app running only on compat services | Implemented, Tested |
| Vulkan graphics backend | Blocked in the dev sandbox (no GPU); Planned (Phase 2) |
| Adaptive Performance Engine | Planned (Phase 4) |
| Windows backend | Implemented (hwinfo via Win32; ELF launch Blocked, cross-compile CI-verified) |
| Rust components | Planned (no Rust toolchain in current sandbox) |
| Any commercial PS5 software support | Blocked (proprietary formats, authorization, DRM — see docs) |

Latest measured test result on Linux x86-64 (g++ 11.4, CMake 3.22):
**80/80 unit tests pass, 3/3 CTest suites pass** (unit, end-to-end native
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

Note: this sandbox has the SQLite runtime but no dev headers, so the repo
carries a minimal API shim (`library/sqlite3_shim.h`). On a normal dev
machine define `PNR_USE_SYSTEM_SQLITE3` and use the system header.

`pnr run` prints the master workflow's ten steps with explicit
SUCCESS / FAILED / BLOCKED / SKIPPED markers — a BLOCKED graphics step is
reported, never hidden.

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
loader/      MODULE 02 - ELF parser (A), native loader (B), format adapters (C, planned)
compat/      MODULE 03 - memory, timing, filesystem, thread services
platform/    MODULE 07 - host OS abstraction, Linux + Windows backends (Windows launch blocked)
package/     .pnrgame format: ustar, SHA-256, importer
library/     MODULE 09 - SQLite game/installation/compatibility/launch store
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
