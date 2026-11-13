# PNR Interface Contracts

Version 0.1 · Phase 0 deliverable · 2026-11-13
Contracts are versioned. Breaking changes bump the major version and keep
the old symbol available for one release where feasible.

## IF-CORE-01 RuntimeManager (v1) — Implemented, Tested

Owns the runtime. Construction is cheap; `initialize()` performs
VALIDATING → INITIALIZING → READY.

- `Status initialize()` — fails if any registered subsystem fails to
  initialize. On failure the runtime stays usable for error reporting.
- `Status mark_running() / suspend() / resume() / shutdown()` — lifecycle
  transitions; illegal transitions return an explicit error.
- Accessors: `events()`, `subsystems()`, `config()`, `capabilities()`,
  `errors()`, `lifecycle()`.

Threading: individual sub-objects are internally synchronized;
`RuntimeManager` methods are intended for the launcher main thread.

## IF-CORE-02 LifecycleController (v1) — Implemented, Tested

Legal transitions (enforced, anything else fails):

```
CREATED      -> VALIDATING | STOPPING
VALIDATING   -> INITIALIZING | STOPPING
INITIALIZING -> READY | STOPPING
READY        -> RUNNING | STOPPING
RUNNING      -> SUSPENDED | STOPPING
SUSPENDED    -> RUNNING | STOPPING
STOPPING     -> TERMINATED
TERMINATED   -> (final)
```

## IF-CORE-03 RuntimeEventBus (v1) — Implemented, Tested

- `subscribe(type, handler) -> id`; `unsubscribe(id)`; `publish(event)`.
- `"*"` subscribes to everything. Synchronous dispatch, publish order,
  handlers invoked outside the internal lock.
- Known types: `lifecycle.changed`, `session.started`, `session.exited`,
  `session.crashed`, `session.stop_requested`.

## IF-CORE-04 ConfigurationManager (v1) — Implemented, Tested

- `load_from_file(path)` — `key = value` lines, `#` comments, blank lines
  allowed. Missing file, malformed line, or empty key are explicit errors.
- `get / get_or / get_int / set`.

## IF-CORE-05 CapabilityRegistry (v1) — Implemented, Tested

- `declare(name, supported, note)`; `query(name) -> optional<Capability>`.
- Absence of a capability record means "not detected", and consumers must
  treat it as unsupported. Predeclared negatives: `graphics.vulkan` (in the
  dev sandbox), `loader.protected_ps5_format` (until authorized).

## IF-CORE-06 GameSession (v1) — Implemented, Tested

- Constructed with an injected `Launcher` (function: exe, args, out-signal
  -> exit code). The core never links the loader.
- `start()` requires READY or RUNNING; publishes session events; converts
  signals and non-zero exits into failures. Never reports success for a
  crashed game.

## IF-LOAD-01 ElfParser (v1) — Implemented, Tested

- `parse_elf(path) / parse_elf_bytes(bytes) -> ParseResult`.
- Validates: ELF magic, ELF64 class, little-endian, header sizes,
  program-header table bounds, per-segment file-range bounds.
- Rejects with specific errors; never guesses at unrecognized formats.
- `is_x86_64`, `is_executable_type` helpers. Only ET_EXEC/ET_DYN pass
  loader validation today.

## IF-LOAD-02 NativeLoader (v1) — Implemented (POSIX), Tested

- `validate(path) -> ParseResult` (STEP 1 of the launch workflow).
- `launch(path, args) -> LaunchResult`: validated → fork + execv in a
  child process → waitpid → `{started, finished, exit_code | signal,
  wall_seconds}`. Child inherits stdio (capture Planned).
- In-process segment mapping (`mmap` + relocations + TLS) for host-ABI
  binaries: Planned. Foreign-ABI loading: Phase 7 research, Blocked.

## IF-LOAD-03 FormatAdapter (v0) — Planned

Replaceable binary-format front end. Future adapters register a recognizer
+ parser pair. Protected/proprietary formats register as "recognized but
unsupported", producing an informative error — never a fake load.

## IF-COMPAT-01 MemoryService (v1) — Implemented, Tested

- `allocate(size, alignment=16)` — power-of-two alignment required;
  returns nullptr on failure (never a dummy pointer).
- `deallocate(ptr)`; `bytes_outstanding()`; `live_allocations()`.

## IF-COMPAT-02 TimingService (v1) — Implemented, Tested

- `now_seconds()` monotonic; `sleep_ms(ms)`.

## IF-COMPAT-03 FileSystemService (v1) — Implemented, Tested

- Jail root set at construction. Virtual paths start with `/` and resolve
  inside the root; `..` escaping the root, backslashes, and NULs are
  rejected. This is virtual path routing at the application level, not an
  OS-enforced security sandbox: a binary calling the host directly bypasses
  it. Trusted developer-built content only until Module 11 lands a real
  boundary.
- `resolve / write_file / read_file / exists / create_dirs`.

## IF-COMPAT-04 ThreadService (v1) — Implemented, Tested

- `spawn(fn) -> id`; `join_all()`; `live_count()`.

## IF-OS-01 .. IF-OS-11 Host abstraction (v0) — Planned

`IProcess, IThread, IMemory, IFilesystem, IClock, IWindow, IDisplay,
IInput, IAudio, IPower, ITelemetry` with Windows and Linux backends.
Currently the loader and compat modules carry POSIX paths directly; the
abstraction lands before the Windows build (P1 follow-up) so both OSes
share one core.

## IF-GRAPH-01 VulkanBackend + SpirvModule (v1) — Implemented (first slice), Tested

- `VulkanBackend::open()` dlopens the system Vulkan loader; a host without
  one returns an honest error (graphics is Blocked there, never faked).
- `create_instance()` enumerates real physical devices with
  name/vendor/api/type (software Vulkan devices are labeled, not hidden).
- `create_device()` picks a compute-capable queue family or fails honestly.
- `run_compute_words(spirv, io)` runs a compute shader with one SSBO
  (set 0 binding 0): buffer, host-coherent memory, shader module,
  descriptors, pipeline, dispatch, fence, readback. Every stage reports
  its own failure with the driver result code.
- `SpirvModule::compute_write_u32(value)` emits a SPIR-V 1.0 module at
  word level; validated externally (spirv-val when `PNR_SPIRV_VAL` is set)
  and by real execution. The emitted interface follows Vulkan 1.0 rules
  (BufferBlock SSBO; Uniform storage is not listed in OpEntryPoint).
- Builds without the Vulkan SDK via `graphics/vulkan_shim.h` (declarations
  taken from the official Khronos headers, Apache-2.0/MIT); define
  `PNR_USE_SYSTEM_VULKAN` to build against the SDK instead - the test
  suite passes identically both ways.
- Planned, not built: render passes, swapchains, vertex/fragment stages,
  texture/sampler objects.


Game Rendering Interface → Graphics Compatibility Adapter → Intermediate
Rendering Commands → Shader/Resource Translator → Vulkan Backend.
Contract drafting starts with the Vulkan prototype; nothing is specified
here that has not been validated against a real device.

## Cross-language boundary (v0) — Reserved

A narrow C ABI (`pnr_` prefix, opaque handles, explicit error codes) is
reserved for future Rust crates (telemetry, package management). No Rust
code ships yet; no Rust toolchain exists in the current dev sandbox.

## IF-OS-01 Host Platform (v1) — Implemented (Linux), Tested

`pnr::platform::IHostPlatform` (platform/os.hpp):

- `name()` → "linux" | "windows" (both backends Implemented; the Windows
  backend reports real Win32 hwinfo and honestly refuses ELF process
  launch as Blocked - see `platform/host_windows_stub.cpp`).
- `cpu_logical_cores()`, `memory()` → optional MemoryInfo (absent when the
  host cannot report it), `monotonic_seconds()`.
- `launch_process(path, args) → ProcessResult` — supervised child process:
  `{started, finished, exit_code | signal, wall_seconds}`.
- `on_battery()` → optional; absent on hosts without power_supply nodes.
  Unknown is reported, never guessed.
- Factory: `make_host_platform()`.

## IF-PKG-01 Package Format .pnrgame (v1) — Implemented, Tested

ustar archive (regular files, entry names ≤ 100 bytes, no `..` segments):

- `manifest.pnr` — `key = value` lines. Required: `id` (directory-safe),
  `title`, `version`, `executable`. Optional: `arch` (must be `x86-64`),
  `min_runtime`.
- `checksums.sha256` — `<hex>  <path>` per payload file.
- Payload: executable (must itself validate as x86-64 ELF) + assets.

`build_package(out, manifest, files)` (dev tooling) and
`import_package(path, library_root, db)`:

1. Read archive (header checksums verified).
2. Reject unsafe entry names (absolute, `..`, backslash, NUL).
3. Parse + validate manifest (all required fields, arch, safe id).
4. Verify EVERY payload SHA-256 before any disk write.
5. Validate the executable's ELF bytes from memory.
6. Extract to `<root>/games/<id>-<version>/`, executable marked +x.
7. Register game + installation(integrity=verified) +
   compatibility(Untested) in the db. Importing never asserts playability.

Failure names the exact stage. Tampered payloads fail at CHECKSUM
MISMATCH. Non-packages fail at archive validation.

## IF-LIB-01 Game Database (v1) — Implemented, Tested

Installations carry the entry point (`executable`) so `pnr run <id>`
resolves a game without a path; older databases gain the column by an
automatic migration. Launches of library games are recorded
(exit code + wall seconds). A clean exit advances compatibility
`Untested -> Launches Only`; that state asserts only that the game
started and finished, never playability. Ad-hoc `pnr run <path>`
touches nothing in the library.

`pnr::library::GameDb` over SQLite (WAL):

- Tables: `games(game_id,title,version,platform)`,
  `installations(game_id,path,version,integrity)`,
  `compatibility(game_id,state,notes)`,
  `launches(game_id,exit_code,wall_seconds,at)`.
- `open(path)` creates parent dirs + schema; `add_game / get_game /
  list_games / set_installation / get_installation / set_compatibility /
  get_compatibility / record_launch / launch_count`.
- Compatibility states: Verified Playable | Playable With Issues |
  Launches Only | Unsupported | Untested | Blocked by Missing Dependency.
  New imports are always Untested.
- Entitlement records are a separate future table; this store never
  asserts ownership, distribution or execution rights.

## IF-APE-01 Adaptive Performance Engine (v1) — Implemented, Tested

- `HardwareFacts{cpu_logical_cores, ram_total_bytes?, ram_available_bytes?, on_battery?}`: optional fields are nullopt when the host cannot report them; never fabricated.
- `classify(HardwareFacts) -> PerfClass`: CPU/RAM heuristic (D: >=12 cores and >=32 GiB; C: >=8 and >=16 GiB; B: >=4 and >=8 GiB; else A; no cores -> Unknown). GPU is excluded until Module 04 probing exists; compatibility is never decided from class alone.
- `FrameWindow::recompute()`: avg, p95, variance over the recorded frame times.
- `AdaptiveController::decide(Telemetry, QualitySettings&) -> Adjustment`: at most one bounded change per decision (background_tasks halved on CPU saturation; resolution scale -10% on memory pressure or missed frame target; fps_cap floor 30 as last resort); `evaluate()` after the stabilization window reverts changes that did not improve frame time. Deterministic rules only — ML tuning is deferred per the master prompt.

## IF-LOM-01 Laptop Optimization Manager (v1) — Implemented, Tested

- `PowerMode`: battery_saver / balanced / max_performance with fixed `Caps{fps_cap, min_resolution_scale_pct, max_background_tasks}` (30/50%/1, 60/75%/2, 120/100%/4).
- `default_mode(HardwareFacts)`: on battery -> saver; plugged in OR unknown -> balanced (unknown power state is never assumed to be AC).
- `apply_caps(Caps, QualitySettings&)`: applies caps as bounds to APE quality settings.

## IF-SVC-01 Virtual Filesystem (v1) — Implemented, Tested

- `VirtualFileSystem(root)`: one jailed sub-directory per area (installed content, user config, save data, shader cache, runtime cache, compat metadata, logs).
- `saves(user_id, game_id)`: per-game, per-user save area; ids are validated (non-empty, no path separators, no `..`, no leading `.`) and invalid ids throw `std::invalid_argument`. Save areas are isolated: one user cannot read another user's saves through the API.
- All path resolution goes through the Module 03 jailed `FileSystemService`; traversal outside an area root is rejected.

## IF-SVC-02 Audio (v1) — Interface Implemented, host backends Planned

- `IAudioBackend::enumerate() -> vector<AudioDeviceInfo>`: reports what the host actually exposes; empty when no device API is available.
- `IAudioBackend::open_stream(device_id, out) -> Status`: fails with an explicit error when no backend exists (never a silent dummy).
- Only the null backend ships today (no audio device API in the dev sandbox). No PS5 audio algorithm is claimed.

## IF-SVC-03 Input (v1) — Implemented, Tested

- `IInputBackend::enumerate() -> vector<InputDeviceInfo>`; the Linux backend scans `/dev/input` read-only.
- `InputMapper::load(profile_text)`: ini-style `action = control` bindings; malformed lines fail with the line number. `translate(raw)` maps raw control events to actions.
- `VirtualInputDevice`: scripted device for tests and demos. Advanced controller features are exposed only when a device reports them.

## IF-NID-01 PS NID Computation (v1) — Implemented, Tested

- `nid::sha1(data) -> array<uint8_t,20>`: SHA-1 digest, validated against a reference vector.
- `nid::compute_nid(symbol_name, library_name) -> string`: the 11-character PS-style NID; throws `std::invalid_argument` on an empty symbol name. Algorithm adapted from [AnyPS5](https://github.com/boykopovar/AnyPS5) (GPL-2.0); AnythingPS5 is GPL-2.0.
- `nid::resolve_nid(nid) -> string`: seed lookup table over common libkernel/libSceLibcInternal symbols; returns empty for unknown NIDs. The table grows with the PRX system-library layer (taxonomy per AnyPS5's core/libs/prx structure; we document the structure, we do not copy its 6000+ definition files wholesale).

## IF-PRX-01 PRX System Libraries (v1) — Implemented, Tested

- `PrxRegistry`: taxonomy of PS-style system-library symbols. Library/symbol organization follows AnyPS5 (GPL-2.0) core/libs/prx; host implementations are original PNR code.
- `bind(nid, &fn, &error) -> SCE_OK` for implemented symbols; `SCE_KERNEL_ERROR_EINVAL` for declared-only symbols (known but not implemented); `SCE_KERNEL_ERROR_UNKNOWN` for NIDs outside the taxonomy. Never silently substitutes.
- Implemented (libkernel): `sceKernelUsleep`, `sceKernelSleep`, `sceKernelAlloc`, `sceKernelFree`, `sceKernelCreateThread` (detached host thread; handle/join API is Planned).
- Implemented (libSceLibcInternal): `printf` (BOUNDED formatter: %d %i %u %x %s %c %% only, max 8 variadic args - the honest limit of a NID trampoline; unsupported specifiers pass through literally), `malloc`, `free`, `memcpy`, `memset`, `strlen`, `strcmp`.
- Implemented (libkernel memory): `sceKernelMapMemory` (anonymous private mmap only), `sceKernelMunmap` (full/head/tail unmap of own mappings; middle-split EINVAL), `sceKernelMTypeprotect` (mprotect on own mappings), `sceKernelQueryMemoryProtection` (own mappings only - foreign ranges return EINVAL, never guessed).
- Declared-only (honest, no host implementation): `sceKernelReserveVirtualRange`, `sceKernelAvailableFlexibleMemorySize`.

## IF-IMP-01 Import Resolution (v1) — Implemented, Tested

- `resolve_imports(path, registry, &out) -> Status`: bounds-checked parse of the ELF dynamic section (DT_NEEDED) and undefined .dynsym symbols; malformed files fail with a specific error, never a crash.
- Each undefined symbol is classified: `host` (native library satisfies it), `prx-implemented` (taxonomy + working implementation), `prx-declared` (in taxonomy, not implemented - reported honestly), or `unknown` (PS-style sce* name outside the taxonomy).
- `pnr run` prints the classification summary and every non-host import before launch; nothing is silently substituted.

## IF-PRX-02 Libkernel File Calls (v1) — Implemented, Tested

- `PsFileTable(fs)`: a jailed descriptor table over a `compat::FileSystemService`. Every path is resolved through the jail before any host syscall; traversal (`..`, escaping segments) returns EINVAL, absolute virtual paths stay inside the jail.
- `open(path, flags) -> fd>=3` (RDONLY/WRONLY/RDWR + CREAT/TRUNC/APPEND subset; unknown flags EINVAL), `read`/`write` -> byte counts, `close`, `lseek` (SET/CUR/END), `fstat` -> `{size, is_directory}`. Closed or foreign fds are EINVAL, never host-fd guesses.
- Registry integration: file calls are declared-only until `PrxRegistry::install_file_calls(&table)` wires them; `bind()` then resolves them as implemented. This keeps "no filesystem" an honest state.

## IF-ABI-01 PS-ABI shim and PS-style guests (v1) — Implemented, Tested

- `libpnr_psabi.so` exports `sceKernelAlloc/Free`, `sceKernelUsleep/Sleep`,
  `sceKernelCreateThread`, `sceKernelMapMemory/Munmap/MTypeprotect`, and the
  `sceKernelOpen/Read/Write/Close/Lseek/Fstat` file family.
- `pnr run` launches guests with the shim preloaded and
  `PNR_JAIL_ROOT=<game data dir>/jail` in the environment; the shim resolves
  its jail root from that variable (default `/tmp/pnr_psabi_default_jail`).
- Shim file calls dispatch through `compat::FileSystemService` + `PsFileTable`,
  so the guest's sce file calls land inside its jail exactly like an emulated
  file table. The guest's own direct host calls are NOT contained - see the
  FileSystemService contract.
- The CLI installs the same jail's file table into its classification registry
  before resolving imports, so the printed classification matches the real
  dispatch environment.
