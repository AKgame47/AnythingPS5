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
| CLI driving the full 10-step launch workflow with honest per-step results | Implemented, Tested |
| Developer-controlled demo app running only on compat services | Implemented, Tested |
| Vulkan graphics backend | Blocked in the dev sandbox (no GPU); Planned (Phase 2) |
| Adaptive Performance Engine | Planned (Phase 4) |
| Windows backend | Planned (POSIX today; abstraction defined) |
| Rust components | Planned (no Rust toolchain in current sandbox) |
| Any commercial PS5 software support | Blocked (proprietary formats, authorization, DRM — see docs) |

Latest measured test result on Linux x86-64 (g++ 11.4, CMake 3.22):
**49/49 unit tests pass, 2/2 CTest suites pass** (unit + end-to-end native
launch). No benchmarks exist yet; none are claimed.

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
```

`pnr run` prints the master workflow's ten steps with explicit
SUCCESS / FAILED / BLOCKED / SKIPPED markers — a BLOCKED graphics step is
reported, never hidden.

## Windows build

Planned. The core is OS-independent; the loader and compat services carry
POSIX implementations today (`fork/exec`, `posix_memalign`) behind
interfaces whose Windows backends (Win32, MSVC + CMake) are Phase 1
follow-up work. Untested on Windows — that is stated, not assumed.

## Repository layout

```
apps/        pnr CLI now; desktop launcher + diagnostics later
core/        MODULE 01 - runtime, lifecycle, events, config, capabilities
loader/      MODULE 02 - ELF parser (A), native loader (B), format adapters (C, planned)
compat/      MODULE 03 - memory, timing, filesystem, thread services
examples/    pnr_test_app - developer-controlled native demo "game"
tests/       unit tests (no framework) + end-to-end CTest
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
