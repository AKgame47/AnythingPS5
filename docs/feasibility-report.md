# PNR Feasibility Report

Version 0.1 · Phase 0 deliverable · 2026-11-13
Rule applied throughout: separate proven facts from assumptions, and name
every blocked dependency. Nothing here claims commercial PS5 compatibility.

## 1. Proven in this repository today (measured, reproducible)

| Claim | Evidence |
|---|---|
| ELF64 binaries parse with bounds checks; non-ELF/corrupt files rejected with specific errors | `loader/elf_parser.cpp`; unit tests `elf: *` |
| A real compiled x86-64 test program executes natively on the host CPU through PNR's launcher workflow | `pnr run build/pnr_test_app` → child exit 0, wall time measured |
| The game runs in a separate process; crashes/non-zero exits surface as failures, never successes | unit tests `loader: *` |
| Compat services (memory/timing/filesystem/threads) work for a real program | `examples/test_app.cpp`; unit tests `mem: *`, `timing: *`, `fs: *`, `threads: *` |
| Lifecycle state machine enforces legal transitions only | unit tests `lifecycle: *` |
| Filesystem path routing (application-level jail) rejects `..` traversal | unit tests `fs: *` |
| Full 10-step launch workflow reports per-step SUCCESS/FAILED/BLOCKED/SKIPPED | CLI run log in Phase 1 report |

Test status (2026-11-13, second increment): 80/80 unit tests pass; 3/3
CTest suites pass (unit, end-to-end native launch, package workflow) on
Linux x86-64, g++ 11.4, CMake 3.22.

## 2. Feasible with public knowledge (no proprietary access needed)

- Native x86-64 execution of developer-controlled executables (done).
- ELF parsing, validation, segment inspection (done; in-process mmap-based
  loading with relocations is standard published technique — Planned).
- Compatibility services over POSIX and Win32 (POSIX slice done; Win32
  backend is published API work — Planned).
- Vulkan rendering, swapchain, pipeline cache, shader management for
  developer-controlled content (standard Khronos-documented work — Planned,
  currently Blocked by missing GPU in the dev sandbox).
- Dynamic resolution, frame pacing, telemetry-driven performance profiles
  (standard engine work once a renderer exists — Planned).
- `.pnrgame` developer package format, importer, integrity checks
  (self-defined format — Planned).
- Launcher UI, game library, SQLite metadata (standard desktop work —
  Planned).

## 3. Requires research, uncertain

- In-process native loading of arbitrary x86-64 shared libraries with full
  TLS, relocations, and exception interop between game and host. Standard
  in principle (glibc does it), but doing it for binaries not built for the
  host loader is real engineering with real edge cases.
- Running software built against a foreign libc/ABI (e.g. a console's
  userland ABI) requires either a shim libc or syscall translation at the
  user/kernel boundary. Comparable projects (compatibility layers for
  other consoles) show partial success after years of work. Timeline and
  ceiling are unknowns, not impossibilities.
- Temporal upscaling / frame generation quality on low-end hardware:
  feasible only where the renderer supplies motion vectors, depth and
  jitter; must be validated per-integration.

## 4. Blocked on proprietary knowledge or authorization

These cannot be implemented from public information, and PNR will not fake
them:

| Dependency | Why blocked | What would unblock it |
|---|---|---|
| PS5 executable format (protected, encrypted/ signed binaries) | Format undocumented officially; contents encrypted; handling requires authorization | Licensed documentation or Sony partnership; lawful published research |
| PS5 system libraries / ABI semantics (libkernel, libSce* modules) | Proprietary; behavior not officially documented | Licensed SDK access or validated clean-room documentation |
| PS5 shader format (GNM-family GPU ISA/semantics vs desktop Vulkan) | Proprietary shader pipeline; PS5 GPU has custom AMD IP blocks | Authorized docs; per-title validated translation |
| PlayStation Network account, entitlement, and game distribution | No public authorized API for third-party storefront/runtime use | Sony/partner agreements |
| DRM / license checks in commercial games | Must not be bypassed (legal + project rule) | Authorized, rights-holder-approved mechanisms |

Until unblocked, any commercial PS5 content is reported as
**Unsupported / Blocked by unavailable interfaces** — never as playable.

## 5. Compatibility assumptions (explicit)

1. Host CPU is x86-64. Non-x86-64 hosts (ARM laptops) are out of scope for
   the initial runtime.
2. Developer test software is built for the host OS ABI, so the Phase 1
   loader delegates process creation to the OS. Foreign-ABI loading is a
   later, separately-validated milestone.
3. 16 GB RAM and a Vulkan 1.2-capable GPU are the provisional floor for
   future graphics demos; unverified until Phase 2 hardware testing.
4. Low-end targets get honest messaging: if a workload cannot hit a frame
   target, PNR says so instead of promising (rule 5/6).

## 6. Legal & licensing boundaries (binding)

- No Sony firmware, SDK libraries, game files, or keys in this repo, ever.
- No DRM circumvention, no device impersonation, no fake PSN services.
- Owning a PS5 game does not by itself grant PC execution rights;
  ownership, distribution rights, and execution rights are tracked as
  separate records (see architecture doc, Game Manager).
- Test content is original and developer-controlled.

## 7. Performance feasibility notes

- CPU: native execution means no instruction translation cost for x86-64
  targets; the real costs will be ABI bridging, memory layout, and sync
  semantics — measurable only against real foreign binaries (Blocked).
- GPU: desktop Vulkan drivers vs console GPU behavior differ (shader
  model, memory model, presentation); translation overhead is unknown
  until measured in Phase 2+.
- Memory: PS5 titles assume 16 GB unified memory; low-end PCs with 8 GB +
  shared iGPU memory may simply not fit some workloads. PNR will measure
  and report, not promise.

## 8. Proof-of-concept test plan (per phase)

| Phase | Exit criterion (executable) | Status |
|---|---|---|
| 0 | This doc set: what is implementable with public interfaces vs blocked | Done |
| 1 | `ctest` green: unit + end-to-end native run of `pnr_test_app` via CLI | Done (80/80, 3/3) |
| 2 | Render + animate a 3D scene via Vulkan on Windows and Linux, validation layers clean | Partially unblocked: compute dispatch + SSBO readback tested on software Vulkan (Mesa lavapipe) in the sandbox; full render passes Planned |
| 3 | Demo game runs using only compat APIs (no direct host calls) for audio/input/storage/timing | Partially demonstrated (memory/fs/timing/threads; imported .pnrgame game installs + runs) |
| 4 | Measured frame-time improvement from dynamic resolution on ≥1 real GPU, baseline vs adapted, same scene | Planned |
| 5 | Correct power-profile + hybrid-GPU behavior on a real laptop | Planned (needs hardware) |
| 6 | Nontechnical tester installs, launches demo, changes settings, exits — no dev tools | Planned |
| 7 | Written compatibility report: what runs, what is blocked, with evidence | Planned |

## 9. Benchmark methodology (committed)

- Report average FPS, 1% low FPS, frame-time percentiles, CPU/GPU frame
  time, RAM/VRAM usage, shader-compile stalls, load times.
- Compare every optimization against an unoptimized baseline, same scene,
  same settings, recorded driver versions, thermal and power state.
- Minimum three hardware classes: low-end iGPU laptop, midrange dGPU
  machine, high-end desktop.
- Never fabricate results (rule 14). No benchmarks exist yet; none are
  claimed.

## 10. Related work (design reference only)

- [boykopovar/AnyPS5](https://github.com/boykopovar/AnyPS5) — a PS5 relinker:
  converts PS5 ELFs offline into native Linux/Windows/macOS executables and
  binds imports by NID against reimplemented PRX system libraries, with a
  shader recompiler producing SPIR-V. License: GPL-2.0-only. PNR adopted GPL-2.0-only
  (owner decision, 2026-11-13), so AnyPS5 code may be reused with
  attribution where it genuinely helps. It remains primarily a design
  reference for:
  the relinker-vs-runtime-loader architecture trade-off (Module 02 format
  adapters), NID-based import binding, the PRX system-library taxonomy a
  compatibility layer eventually needs, and the shader-to-SPIR-V pipeline
  shape (Module 04). Its legal posture matches ours: no firmware, keys, or
  DRM circumvention shipped.
