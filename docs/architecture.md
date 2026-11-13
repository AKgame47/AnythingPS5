# PNR Architecture Specification

Version 0.1 · Phase 0 deliverable · 2026-11-13
Status labels used everywhere: **Planned / Implemented / Tested / Experimental / Blocked**.

## 1. What PNR is (and is not)

PNR (PlayStation Native Runtime) is a research project exploring native
x86-64 execution of compatible software on Windows and Linux, with a
compatibility layer mapping console-style services to host services, a
Vulkan graphics backend, and an adaptive performance engine.

PNR is **not** an emulator, not a VM, not streaming, and — as of this
revision — **not** able to run any commercial PS5 game. No proprietary
Sony interface is implemented or claimed. See `docs/feasibility-report.md`
for what is proven, assumed, and blocked.

## 2. Execution pipeline (target)

```
Launch PNR
 -> Detect CPU / GPU / RAM / storage / drivers / OS
 -> Determine native execution + graphics capabilities
 -> Display game library
 -> Validate selected game (format, architecture, ownership, compatibility)
 -> Resolve runtime interfaces
 -> Initialize native executable environment
 -> Initialize graphics / memory / audio / input / filesystem
 -> Activate Adaptive Performance Engine
 -> Start game process (separate from launcher)
 -> Monitor CPU / GPU / memory / frame times
 -> Adjust supported settings during execution
 -> Save data + config; shutdown with logs and optional perf report
```

Phase 0/1 reality: the pipeline above runs end-to-end with a
developer-controlled test application and no graphics.

## 3. Module map (11 modules)

| # | Module | Repo path | Status |
|---|--------|-----------|--------|
| 01 | Core Runtime | `core/` | Implemented, Tested |
| 02 | Native Executable Loader | `loader/` | Implemented (ELF64 validate + supervised child-process launch), Tested |
| 02a | PS NID support | `loader/nid.*` | Implemented (SHA-1 + NID computation + seed resolver; algorithm adapted from AnyPS5, GPL-2.0, attribution in file headers), Tested |
| 02b | PRX system libraries | `prx/` | Implemented (libkernel: usleep, sleep, alloc, free, create_thread; libSceLibcInternal: printf bounded subset, malloc, free, memcpy, memset, strlen, strcmp; bound by NID; taxonomy per AnyPS5 core/libs/prx, implementations original), Tested |
| 02c | Import resolution | `loader/import_resolver.*` | Implemented (DT_NEEDED + undefined .dynsym parsed bounds-checked; every import classified host / prx-implemented / prx-declared / unknown and reported at launch), Tested |
| 03 | Compatibility Layer | `compat/` | Implemented (memory, timing, filesystem, threads), Tested |
| 04 | Graphics Engine (Vulkan) | `graphics/` | Blocked in sandbox (no GPU/Vulkan); Planned |
| 05 | Adaptive Performance Engine | `performance/` | Planned |
| 06 | Laptop Optimization Manager | `performance/laptop` | Planned |
| 05 | Adaptive Performance Engine | `ape/` | Implemented (deterministic rules: CPU/RAM class heuristic, frame stats, closed-loop keep/revert controller), Tested |
| 06 | Laptop Optimization Manager | `laptop/` | Implemented (battery/balanced/max-performance caps; unknown power state never assumed AC), Tested |
| 08 | Game Services | `services/` | Implemented: virtual filesystem (jailed areas, per-game/user saves), input (evdev enumeration, mapping profiles, virtual device), audio (interface + null backend), Tested; host audio backends Planned |
| 07 | Host OS Abstraction | `platform/` | Implemented (Linux backend: CPU/memory/power/process, Tested; Windows backend: Win32 hwinfo, ELF launch Blocked) |
| 08 | Game Services (audio/input/storage) | `services/` | Planned (save-data storage exists inside `compat`) |
| 09 | Game Manager (library) | `library/` | SQLite data layer Implemented, Tested; UI Planned |
| 10 | Desktop Launcher UI | `apps/launcher` | Planned (CLI exists: `apps/pnr_cli.cpp`) |
| 11 | Security Manager | `security/` | Partial (process isolation, fs jail, package traversal/checksum rejection implemented; sandboxing Planned) |
| - | Installer (.pnrgame) | `package/` | Implemented (tar+SHA-256, importer, registration), Tested |

## 4. Component diagram (current build)

```
+------------------+        injects Launcher         +---------------------+
|  apps/pnr_cli    |-------------------------------->|  core (MODULE 01)   |
|  10-step driver  |                                 |  RuntimeManager     |
+-------+----------+                                 |  GameSession        |
        | uses                                       |  EventBus / Config  |
        v                                            |  Capabilities /     |
+-------+----------+                                 |  ErrorReporter /    |
| loader (M02)     |                                 |  Lifecycle          |
|  ElfParser (A)   |                                 +----------+----------+
|  NativeLoader(B) |  fork/exec + waitpid                     | owns
|  FormatAdapter(C)|  (Planned)                               v
+-------+----------+                                 +----------+----------+
        | loads & supervises                         | compat (MODULE 03) |
        v                                            |  MemoryService      |
+------------------+                                 |  TimingService      |
| examples/        |  demo game calls compat only    |  FileSystemService  |
| pnr_test_app     |-------------------------------->|  ThreadService      |
+------------------+                                 +---------------------+
```

Dependency rule enforced by build order: `core` depends on nothing;
`loader` and `compat` depend only on `core`; apps/examples depend on all
three. The core never includes loader or compat headers; `GameSession`
receives its launcher as an injected `std::function`, so the core stays
OS- and loader-agnostic.

## 5. Data flow for one game launch (current build)

1. `pnr run <exe>` → NativeLoader.validate(): ELF64 parse, x86-64 machine
   check, executable-type check, bounds-checked program headers.
2. CLI records host facts (logical cores, /proc/meminfo) into the
   CapabilityRegistry. Vulkan is declared **unsupported** in this
   environment — recorded, not guessed.
3. RuntimeManager.initialize(): CREATED → VALIDATING → INITIALIZING →
   READY, events published on each transition.
4. Compat services start; save-data jail created at `./pnr_data/saves`.
5. Graphics step reports **BLOCKED** explicitly; the demo app is
   non-graphical, so the launch continues.
6. GameSession.start() → NativeLoader.launch(): fork + execv of the
   validated binary in a child process; waitpid; wall time; exit code or
   terminating signal captured.
7. Monitor/optimize steps report what was actually measured (wall time,
   exit status). Frame-time telemetry requires the graphics phase and is
   not simulated.
8. Shutdown: STOPPING → TERMINATED, subsystems released in reverse order,
   error count reported. Exit code of the CLI is the child's exit code.

## 6. Concurrency model

- Launcher process: single main thread for lifecycle; compat ThreadService
  spawns worker threads with join_all() semantics.
- Game code runs in a **separate process** (crash isolation; security rule:
  imported code never runs in the launcher process).
- RuntimeEventBus is thread-safe; handlers are invoked outside the bus
  lock so handlers may publish or unsubscribe.

## 7. Error handling

- Every fallible call returns `pnr::Status` (ok + Error{code, subsystem,
  message}). No silent dummy successes.
- A crashed or non-zero-exit game never produces a "success" report;
  `GameSession` converts signals and non-zero exits into explicit failures.
- Loader rejects malformed binaries at validation with a specific reason.

## 8. Resource ownership

- SubsystemRegistry owns subsystems via shared_ptr; shutdown order is the
  reverse of registration.
- MemoryService tracks every live allocation (pointer → size) and reports
  outstanding bytes; the demo proves return to zero.
- FileSystemService owns a jail root; all virtual paths resolve inside it.

## 9. Roadmap dependency graph (phases)

```
P0 Feasibility (this doc set)
  -> P1 Native runtime prototype [core, loader, compat, CLI, tests]  <== HERE
      -> P2 Vulkan prototype [graphics]
          -> P3 Compatibility services expansion [audio, input]
              -> P4 Adaptive Performance Engine [telemetry-driven]
                  -> P5 Laptop optimization
                      -> P6 Consumer launcher
                          -> P7 Authorized compatibility research
```

P2 does not depend on P3; in practice graphics + input interleave. No
phase advances on unverified claims: each has an executable exit
criterion in `docs/feasibility-report.md` §8.
