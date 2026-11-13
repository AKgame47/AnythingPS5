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

## IF-GRAPH-02 RDNA decoder (v1) — Implemented (subset), Tested

- `rdna_family_of(word)` classifies any first word into its RDNA encoding
  family; unrecognized patterns return `Unknown`, never a guess.
- `rdna_decode(code)` walks a word stream into `RdnaInstruction` records
  (pc, family, opcode id, mnemonic, operands, immediates, branch target,
  word count). Truncation and unknown families are honest errors.
- Mnemonics exist only for a named subset, partial in both scalar and
  vector families. Other opcodes decode
  structurally with `supported=false` and no invented name.
- 64-bit families (VOP3/memory/image) consume two words so program
  walking stays aligned even where payload fields are not decoded yet.
- Encoding taxonomy, field layouts, operand numbering and opcode numbers
  are adapted from AnyPS5's RdnaDecoder (boykopovar, GPL-2.0); the
  implementation is original. Test corpus is synthetic instruction words,
  like AnyPS5's own decoder tests - no commercial shader content.
- Planned: SDWA/DPP modifiers, MIMG/MUBUF payload fields, VOP3 named
  table, control-flow structurization, general decode-to-SPIR-V translation.
  The restricted constant-compute path is IF-GRAPH-03 below.


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


## IF-GRAPH-03 Restricted compute translation (v12)

Status: Experimental, Implemented (strict subset), Tested.

- `shader/translate.hpp`: `translate_compute_bytes(bytes, guest_base, buffer_words=1, local_size_x=1, uniform_words=0, guest_base_high=0, loop_count_uniform=16)` parses
  little-endian words without alignment assumptions; partial words fail.
  `translate_compute_words(words, guest_base, buffer_words=1, local_size_x=1, uniform_words=0, guest_base_high=0, loop_count_uniform=16)` is the word-level entry point.
- Result: `ok`, diagnostic `error`, `spirv`, `stored_value_known`,
  `stored_value`, `required_buffer_words`, `guarded_memory`, `local_size_x`, `conditional_selection`, `required_uniform_words`, `host_float32`, `full64_pointers`, `structured_loop`, `loop_count_uniform`, `loop_memory`. Every failure returns empty SPIR-V. `stored_value` is
  meaningful only when `stored_value_known` is true; runtime results are
  never invented by the compiler.
- Explicit test ABI: one workgroup with 1..64 invocations, one fixed-size uint array SSBO at set 0/binding 0.
  Caller supplies the 64-bit guest base and 1..65536-word extent.
  The base maps to word zero; all accesses must stay in this byte range.
  It is not a host pointer, a PS5 ABI, or general GPU address translation.
- Accepted instructions: `v_mov_b32` (literal/inline integer/defined VGPR),
  `flat_load_dword`, `v_add_nc_u32`, `v_sub_nc_u32`, `v_and_b32`, `v_or_b32`,
  `v_xor_b32`, one `flat_store_dword`, final zero-immediate `s_endpgm`.
- VGPR definitions map to immutable SSA node indices. Copies retain old
  definitions after source overwrite; destination/source aliasing is safe.
  Runtime SSBO loads become OpLoad and arithmetic becomes real SPIR-V
  integer ALU, with uint32 wraparound. No host precomputation of loaded data.
- For constant address VGPR pairs, add the unsigned
  11-bit FLAT byte offset, then check addition overflow, lower/upper bounds
  and four-byte alignment before generating an array index. Guest base must
  be aligned; zero/oversized extent and base+range overflow are rejected.
  The fast low-word guard is used when high word is constant and
  matches binding base, binding does not cross a 32-bit boundary, and FLAT
  immediate is zero. Other runtime pairs use full64 guards below (single
  invocation only). Undefined words still fail at translation.
  ALU results can supply low addresses, with uint32 modular semantics:
  no carry into high word is inferred for v_add_nc/v_sub_nc. Scalar
  addc/subb chains below explicitly propagate it.
  FLAT scalar address must be 0x7f, all segment/cache/LDS/reserved
  fields zero (including offset bit 11); load VDATA/store VDST must be zero.
- `graphics::SpirvModule::compute_u32(nodes, buffer_words=1, guest_base_low=0, local_size_x=1, uniform_words=0, guest_base_high=0, loop_count_uniform=16)` emits the straight-line uint
  IR. Invalid forward references or reads from Store nodes return empty.
  Load.a and Store.b are checked word indices; malformed/out-of-range
  static accesses produce no module. GuardedLoad.a is an earlier runtime
  low-address node; GuardedStore.a=data, .b=address. Invalid SSA refs fail.
  SPIR-V uses a fixed-size uint array with
  ArrayStride=4, inside the existing BufferBlock struct. Instruction order
  is preserved; constant declarations precede functions.
- `run_translated_compute(backend, translation, io)` is the checked dispatch
  route. It verifies a successful translation and io length at least
  required_buffer_words before Vulkan. Null/short buffers fail unchanged.
  Raw VulkanBackend dispatch remains a low-level API with caller-owned
  buffer sizing; use the checked route for translated kernels.
- Rejects undefined registers, unsupported sources/instructions/modifiers,
  other addresses, additional stores, missing termination and trailing code.
  No wave/EXEC, asynchronous waitcnt, general barriers, resource descriptors,
  unbounded virtual addresses or real-game shader ABI. Full shader compilation Planned.
- FLAT layouts/opcodes and ALU opcode numbers follow AnyPS5's attributed
  GPL-2.0 decoder taxonomy. Tests contain only synthetic instructions.
- Tests use spirv-val for Vulkan 1.0 when `PNR_SPIRV_VAL` is set, check emitted
  OpLoad/ALU opcodes, then dispatch each of five modules against four changing
  runtime inputs without recompilation (including overflow/underflow).
  Readback must match uint32 arithmetic and leave a neighboring word intact.
  SSA copy/overwrite, source order, literal ALU and dependent ALU chains are
  tested too. Multi-word tests load two independent runtime inputs, add,
  store a separate output and check every other word remains unchanged.
  Last legal word at FLAT offset 2044 is executed; invalid ranges, overflow,
  misalignment, reserved offsets, null/short host buffers are rejected.
  Missing validator/loader prints a skip, not a tested claim.


### Runtime address guard policy (Experimental test ABI)

- `guarded_memory=true` signals runtime checks. This is a PNR test policy,
  not a claim about PS5 out-of-range access behavior.
- Before each dynamic access, emit unsigned lower/upper address comparisons
  and `(address & 3)==0`. Combine with OpLogicalAnd, then OpSelectionMerge
  and OpBranchConditional. Index subtraction, shift and OpAccessChain live
  inside the valid branch, as does OpLoad/OpStore. No speculative invalid
  pointer or memory operation is emitted outside that branch.
- An invalid runtime load yields zero via OpPhi at the merge. An invalid
  store does nothing. Host dispatch succeeds; this policy is not a host
  exception or GPU fault report. Constant invalid accesses still fail
  translation, with no module.
- Guarded stores always have stored_value_known=false because even constant
  data may be suppressed. A checked host buffer length is still required.
- Four guarded modules (load, store, load+ALU+store, ALU-derived address)
  validate for Vulkan 1.0 and execute on lavapipe across nine runtime
  addresses each. Cases include first/middle/last words, lower bound, one
  past the end, misalignment, zero and 0xffffffff. Whole-buffer comparisons
  verify zero/skip policy and that every unrelated word remains unchanged.
- Malformed SSA address/data references are rejected. Dynamic high words,
  high mismatch, crossing bindings and nonzero offsets now use the full64
  single-invocation guards documented below.


### Bounded invocation indexing (Experimental test ABI)

- `local_size_x` is baked into SPIR-V LocalSize, with Y=Z=1. Vulkan backend
  dispatches exactly one workgroup. Range 1..64 is a PNR restriction, not a
  claim about hardware limits or PS5 waves.
- When local_size_x>1, initial v0 is LocalInvocationIndex via an Input uint
  BuiltIn (decoration verified with spirv-as). Input variable is included
  in the SPIR-V 1.0 entry-point interface. Default size=1 keeps legacy
  undefined initial VGPR behavior rather than silently changing that ABI.
- SSA tracks non-wrapping affine low addresses (origin + stride*index) for
  moves and supported integer add/sub. Every invocation's address must be
  provably aligned/in-range. Runtime high words or loaded/bitwise-derived
  unproven parallel addresses are rejected, even though guarded single-mode
  addressing allows them. Guards remain emitted for accepted dynamic lows.
- Exactly one store instruction remains supported. Its address stride must
  be four bytes, so invocations write disjoint words. Every load range must
  be disjoint from the whole output range. This conservative proof rejects
  in-place updates and ambiguous aliasing without inventing barriers.
  Shared read-only constant inputs are permitted. No atomics, workgroup
  barriers, shared memory, EXEC masks, subgroup or wave semantics.
- Tests validate and dispatch sizes 2, 8 and 64. Each input element is
  incremented into its separate output element, across three runtime input
  sets per module. Whole-buffer checks include unchanged inputs/sentinel
  and uint32 wraparound. Byte API retains the same workgroup setting.
- Negative cases cover size 0/65, overlapping input/output, colliding
  constant stores, misaligned affine strides, unproven loaded addresses,
  affine overflow and last-invocation out-of-range. Low-level SSA emitter
  does not perform the translator's alias proof; use translator and checked
  dispatch for this ABI. This is not general PS5 shader launch support.


### Strict conditional selection (uniform workgroup decisions)

- Supports `v_readfirstlane_b32` (defined VGPR to ordinary SGPR) only with
  local_size_x=1; one invocation means no wave reduction is invented.
  Decoder now correctly marks this instruction's destination as scalar.
  `s_mov_b32` copies defined SGPR/inline integer/literal values. Ordinary
  scalar values may feed existing integer/vector moves/ALU sources.
- `s_cmp_eq_u32` creates an SSA comparison of defined scalar sources.
  `s_cbranch_scc0` may skip forward over a body of only `v_mov_b32`.
  Body executes on equality, skipped on inequality. One such region per
  kernel; target must be a decoded instruction boundary within the program.
- Body has no memory/scalar/ALU side effects. Moves may depend on earlier
  body moves. Every assigned VGPR must be defined on the skipped path too.
  At merge, each assigned VGPR becomes a structured SelectEqual node:
  OpIEqual, SelectionMerge, BranchConditional and two branches to a Phi.
  This lowers the restricted guest branch into conditional value selection,
  not general CFG structurization. Pure move aliases can be computed before
  selection because they cannot fault or have external effects.
- Conditional SSA preserves constants only when both alternatives have the
  same constant; affine origin/stride only when BOTH alternatives have the
  identical proven map. Otherwise it discards those facts. A selected address
  can still use single-invocation runtime guards; in multi-mode the usual
  affine/alignment/range/disjoint-address proof must succeed after merging.
  `conditional_selection=true` identifies this path.
- Rejects backward/zero/out-of-program/non-boundary targets, undefined SCC
  or SGPRs, special scalar destinations, SCC1/other compare kinds, loops,
  nested/repeated branches, conditional loads/stores/ALU/scalar effects,
  undefined skipped-path VGPRs and multi-mode readfirstlane. Multi-mode
  compares use only constants/uniform SGPRs, as described below.
- Validation tests cover four modules: runtime register selection, dependent
  multi-register moves, literal move with exact PC/word count, and selected
  address feeding a guarded load. Each validates for Vulkan 1.0 and runs
  both equality outcomes on lavapipe, with whole-buffer checks for the
  base module and focused output checks for the additional shapes.
- No general PS5 branch, VCC/EXEC, arbitrary scalar ALU flags, wave
  convergence, divergent multi-invocation control flow or commercial shaders.

### Workgroup-uniform conditional extension

- The same strict forward equality skip now accepts local_size_x=2..64.
  All SGPRs in multi-mode originate from declared runtime push constants,
  scalar copies or constants. Scalar-source instruction fields cannot encode
  VGPR operands. readfirstlane remains rejected, as does carry arithmetic in
  multi-mode. Thus SCC comparison/decision is uniform across the workgroup.
  This is scalar SCC semantics, not independent per-lane scalar branching.
- Move-only bodies may select values which differ by invocation: lane index,
  SSBO data, or previous pure vector results. Dependent/multiple moves merge
  independently with Phi nodes. No body loads/stores/barriers or side effects
  are speculated. Selected address alternatives must retain the identical
  proven affine map in multi-mode; otherwise translation fails conservatively.
  The output must still be stride-four and disjoint from every load range.
- Six modules (2, 8, 64 invocations, two varying-data bodies) validate for
  Vulkan 1.0 and execute on lavapipe. Each reuses the same compiled module
  with nine uniform pairs (zero, seven, 0xffffffff) covering both outcomes.
  Whole-buffer checks verify every selected lane result, inputs and sentinel.
  One body uses dependent moves from per-lane SSBO input, the other selects
  invocation index. Both merge identical affine pointer alternatives.
  Byte API output matches word API with local size and conditional metadata.
- Negative coverage includes undefined/SCC-special compare sources, missing
  compare, wave readfirstlane, backward jump/non-boundary literal target,
  nested region, scalar/body-memory effects, undefined skipped-path values,
  different/misaligned/data-derived selected pointers, overlapping input and
  output, colliding stores and undeclared scalar input.
- Existing one-invocation selected-pointer guard composition is unchanged.
  No new GPU capability, subgroup/wave operation or divergent guest CFG is
  implemented by this extension.


## Shader audit fixtures

`pnr_shader_checks` runs compact synthetic fixtures and deterministic mutation
diagnostics; CTest name `pnr_shader_fixtures`. See `shader-audit.md` for exact
coverage, sanitizer scope and reproduction. Shared scalar literal selectors
consume one word, with PC/truncation regression checks.


### Explicit scalar/uniform input ABI

- Translation entry points take uniform_words=0..16. Initial ordinary SGPRs
  s0..sN-1 are runtime push-constant inputs, uniform across invocations.
  Other initial SGPRs stay undefined. Values are not compilation parameters;
  a single module accepts changing inputs at dispatch. This is PNR test ABI,
  not a Sony user-data/descriptor layout or generic uniform-buffer support.
- IR UniformInput.a selects a checked member of a PushConstant Block with
  uint members at offsets 0,4,... . SPIR-V 1.0 push variables are not listed
  as Input/Output entry-point interface variables. Storage/decoration values
  checked using spirv-as, full modules validated for Vulkan 1.0.
- `run_translated_compute(backend, translation, io, uniforms={})` requires
  exactly required_uniform_words values before Vulkan. Missing/extra values,
  or malformed counts >16 fail without modifying io. Raw backend optional
  uniform vector also caps at 16, but matching shader layout is caller-owned.
- Backend pipeline layout declares that compute push range and command
  recording calls vkCmdPushConstants before dispatch. No new descriptor set
  is used. VkPushConstantRange shim was checked against official headers;
  complete Release -Werror tests pass with both shim and official headers.
- Existing scalar moves and compare use these runtime SSA inputs. Vector
  integer ALU can read a declared SGPR as its scalar source. SGPR copies keep
  old values after later overwrites. Uniform-derived addresses remain runtime
  data, subject to guards in single mode and affine restrictions in multi mode.
- Tests execute changing uniforms on unchanged modules: copied s0 values,
  s15 with overflow arithmetic, both uniform-compare outcomes, and broadcast
  increment for an eight-invocation separate-input/output kernel. Four modules
  validate and execute on lavapipe. Negative tests cover count/index limits,
  undeclared SGPRs, special scalar destinations and missing/extra dispatch data.
- Still no scalar arithmetic flags, scalar memory/resource descriptors,
  wave reduction, multi-invocation scalar branch or commercial shader ABI.


### Bounded host-policy binary32 ALU

- Supports unmodified VOP2 `v_add_f32` (3), `v_sub_f32` (4), `v_mul_f32`
  (8), opcode taxonomy checked against AnyPS5's attributed vector decoder.
  VGPR/SGPR/literal/inline-integer operands contain raw 32-bit register bits.
  They are bitcast, not numerically converted from integer to float.
- Emits OpTypeFloat 32, OpBitcast, OpFAdd/OpFSub/OpFMul, OpBitcast to uint
  storage. Every float result is decorated NoContraction. No RelaxedPrecision
  or FPFastMathMode decorations are emitted. No FMA operation is generated.
  Result loses constant/affine facts; stored_value_known stays false.
- `host_float32=true` identifies an experimental host-Vulkan FP policy.
  PNR does not query/enable shader_float_controls or emit RoundingModeRTE,
  RoundingModeRTZ, DenormPreserve/FlushToZero, SignedZeroInfNanPreserve modes.
  Therefore exact PS5 float behavior is NOT implemented. No rounding-mode
  selection, exception flags, traps, NaN payload/sign/signaling preservation,
  denormal mode or signed-zero portability is promised. NoContraction alone
  is not a substitute for those controls. Source references:
  https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_shader_float_controls.html
  https://github.khronos.org/SPIRV-Registry/extensions/KHR/SPV_KHR_float_controls.html
- NaN and rounding contract: host default arithmetic applies. NaN payload,
  canonicalization, signaling-to-quiet behavior and result sign are unspecified
  by PNR. Even the preservation extension does not promise NaN bit patterns.
  Tests check NaN classification on the tested lavapipe only, not payloads.
  Rounding tests observe ties-to-even for selected normal add cases on
  lavapipe; they do not establish a cross-driver rounding guarantee.
- Tests use fixed expected binary32 bit patterns (not host C++ float as an
  oracle): exact finite operations, tie-even inputs, signed-zero observations,
  plus quiet/signaling NaN classification. Infinity, subnormal and overflow
  probes print observed bits without claiming portable bit behavior. Three
  arithmetic modules pass spirv-val for Vulkan 1.0 and execute on lavapipe.
  Literal and scalar push-constant sources execute with correct raw bits.
- Rejects undefined operands, inline float selectors (not yet implemented),
  SDWA/DPP, VOP3/modifiers, reverse subtract/min/max/FMA and malformed SSA.
  Float ALU remains forbidden in conditional move-only branch bodies.
  Multi-invocation memory affine/race checks remain unchanged; float-derived
  addresses lack a valid affine proof. No commercial shader compatibility.


### Full64 runtime pointer pairs (single invocation)

- Scalar SOP2 s_add_u32/addc_u32/sub_u32/subb_u32 (0/4/1/5) implement
  unsigned arithmetic with carry/borrow-out. addc/subb require a defined
  arithmetic SCC; ordinary add/sub start with carry-in zero. Compare SCC
  cannot be reused as arithmetic carry in this restricted implementation;
  arithmetic SCC cannot drive the compare-only branch subset. Neither
  stale SCC nor unknown flag modes are guessed. Scalar carry arithmetic
  is restricted to one invocation and ordinary SGPR destinations.
- Per-word results are modulo 2^32. Addition carry is overflow of either
  a+b or (a+b)+carry-in. Borrow is (a<b) OR ((a-b)<borrow-in).
  Pair add/sub therefore implements modulo-2^64 arithmetic. Pointer wrap
  in the guest arithmetic itself follows those instructions; subsequent
  FLAT-offset overflow is separately checked and suppresses the access.
  Scalar semantics/opcodes grounded in AMD RDNA ISA section 12.1:
  https://gpuopen.com/download/RDNA_Shader_ISA_25September2020.pdf
- Runtime low/high VGPR pairs may come from SSBO loads, uniform SGPR moves
  or scalar pair arithmetic. Unsigned FLAT offset (0..2047) is added to low,
  carry added to high; high overflow invalidates access. Full unsigned
  lower/upper 64-bit comparisons are lexicographic (high then low), plus
  four-byte alignment. The bounded binding itself must not overflow uint64.
- Pointer/index construction and load/store remain inside the valid branch.
  Since binding size is at most 256KiB, guarded low-word modular subtraction
  gives the exact byte delta even when the binding crosses 0xffffffff.
  No shaderInt64 capability or device Int64 feature is required: only uint32
  comparisons/arithmetic and structured SPIR-V. Invalid loads=0/stores=skip.
- IR GuardedLoad64 has a=low node, b=high node, d=literal offset.
  GuardedStore64 adds c=data node. CarryAdd/BorrowSub a,b,c are operand and
  incoming flag nodes. Invalid refs and offset>2047 fail emission.
  full64_pointers identifies the wide guard route. Low-level IR expects
  carry inputs 0/1 as constructed by the translator, not arbitrary data.
- Workgroup affine addressing restrictions are unchanged. Wide dynamic
  pointers are not accepted in multi-invocation mode; no virtual mapping,
  multiple resources, atomics, VCC/vector carry, arbitrary scalar flags,
  cross-workgroup synchronization or commercial shader ABI is implied.
- Tests validate five full64 modules and execute add/carry, sub/borrow,
  cross-low-word binding, runtime mismatched high, low/high SSBO-loaded
  pointers, misalignment, below/above range, wraparound and FLAT-offset
  high overflow on lavapipe. Whole-buffer comparisons verify zero/skip and
  unrelated words. Negative tests cover undefined carry/borrow/sources,
  special destinations, compare/carry SCC mismatch, workgroup mode and
  malformed IR. Existing guard tests now accept formerly rejected full64
  shapes but test actual runtime policy in this corpus.


### Strict counted accumulator loops

- A separate experimental shape recognizes exactly five consecutive decoded
  instructions: s_mov_b32 sCounter, N; v_add_nc_u32 vAccum, delta, vAccum;
  s_sub_u32 sCounter, sCounter, 1; s_cmp_eq_u32 sCounter, 0;
  s_cbranch_scc0 back-to-add. N must be a literal or inline constant 1..64.
  Backedge must name the decoded add's exact byte PC (including literals).
  Zero is rejected: this is a do-while guest pattern, so zero would underflow
  instead of representing a zero-trip loop. Counts >64 are rejected, never
  clamped or silently unrolled. Runtime uniform counts are now supported by
  the explicit execution contract below.
- Accumulator must be defined before the loop. Delta may be an initialized
  VGPR, ordinary SGPR, literal or inline integer, but must not be accumulator
  itself or the changing scalar counter. Delta values may differ by lane;
  count and control are uniform. Only this single uint32 add is allowed in
  the body. No loop-body memory, scalar effects beyond the counter, nested
  or repeated loops, combined conditional selection, alternate compare,
  decrement or branch modes, early exits or general CFG are supported.
- Real SPIR-V OpLoopMerge has distinct header/body/continue/merge blocks.
  Header Phi nodes carry iteration and accumulator values; body emits IAdd;
  continue increments the loop iteration. Loop result is the final Phi
  accumulator. Arithmetic is modulo 2^32. This is not compile-time result
  evaluation or host execution of guest iterations. Requires no new device
  capability. structured_loop=true marks successful loop translation.
- At exit the guest scalar counter is exactly zero, comparison SCC is true
  and arithmetic carry SCC is invalidated, as after the guest comparison.
  Other registers keep immutable SSA values. No address-affine facts are
  invented for the accumulator result. In multi-mode, using it as a pointer
  fails the usual proof; single-mode may still use emitted bounds guards.
  Memory outside the loop retains all bounded-SSBO/alignment/alias rules.
- Low-level LoopAdd IR has a=earlier initial node, b=earlier delta node,
  c=literal count 1..64. Undefined/forward/store-result references and invalid
  counts fail emission. The translator accepts one loop only; the low-level
  emitter is not itself a guest CFG verifier or an alias-proof interface.
- 21 modules validate for Vulkan 1.0 and run on lavapipe: sixteen combinations
  of local sizes 1/2/8/64 and counts 1/2/16/64,
  plus inline/literal operand forms, runtime uniform delta, exit counter and
  post-loop guarded-load composition. Whole-buffer tests compare every lane,
  inputs and sentinel with wraparound seeds. Byte API retains exact SPIR-V
  and loop metadata. The tests assert actual OpLoopMerge emission.
- Negative cases cover zero/huge/runtime counts, self/changing/undefined
  delta, undefined accumulator, non-in-place or unsupported body, incorrect
  decrement/counter/compare/backedge, special registers/modifiers, repeated
  loops, loop+conditional region, stale carry, malformed IR references/count,
  unproven loop-derived pointer and overlapping input/output.
- Scalar decrement/equality/SCC0 meanings follow the AMD RDNA ISA:
  https://gpuopen.com/download/RDNA_Shader_ISA_25September2020.pdf
  No wave/EXEC/divergent loop semantics or commercial-shader ABI is implied.


### Runtime uniform loop-count contract

- Count may originate from a declared initial SGPR push-constant input, or
  unchanged scalar copies of that input. Reg uniform_input provenance is
  separate from value/constant/affine facts. Overwriting the original SGPR
  after copying does not change the count SSA/input index. No computed,
  SSBO-derived, readfirstlane-derived or lane-varying runtime count is
  accepted. Ordinary scalar-counter destination may alias its source input.
- loop_count_uniform is the initial input index 0..15, or sentinel 16 for no
  runtime count. checked run_translated_compute verifies count metadata,
  exact uniform length, then rejects count zero or >64 with error 105
  (shader: runtime loop count must be 1..64; dispatch rejected), BEFORE
  calling Vulkan or changing io. Invalid metadata is error 104. This is an
  explicit execution failure, not a capped iteration count or guest success.
- The shader independently reads that input at entry, checks unsigned 1..64
  and takes an early return on failure BEFORE any guest nodes/SSBO access.
  The valid path executes real LoopMerge/Phi with runtime bound. This guard
  prevents unbounded execution even if a caller uses raw backend dispatch.
  Raw backend bypasses the checked Status contract: invalid count produces
  no SSBO effects but may return success from Vulkan. Use checked dispatch
  for an explicit count error. No silent clamping occurs in either path.
- LoopAddUniform has initial/delta SSA refs in a/b and count UniformInput
  node in c. Emitter requires the declared guard index to match that direct
  node, not arbitrary arithmetic/forward/store result. The input node may
  be shared by scalar aliases. Entry guard works for all invocations, and
  its branch composes with later load guards and loop header Phi predecessor.
- Seven additional modules validate for Vulkan 1.0 and execute on lavapipe:
  direct/copy-and-overwrite counts for 1/8/64 invocations plus s15 input.
  Each main module reuses compiled SPIR-V for 1/2/16/64 trip counts and
  zero/one/0xffffffff deltas. Whole buffers match exact wraparound outputs,
  unchanged inputs and sentinel. Invalid counts 0/65/0x80000000/0xffffffff
  are rejected before Vulkan on an unopened backend and a ready backend;
  raw dispatch also proves the GPU guard leaves every word unchanged.
  Byte API retains count metadata and exact module. Negative checks cover
  undeclared/computed/special/reduction counts, missing/extra inputs, bad
  metadata, guard-index mismatch, absent entry guard and malformed count IR.
- No zero-trip for-loop interpretation, new guest error mechanism, wave
  semantics, generalized scalar expression evaluator or dynamic unbounded
  loops are implied. Constant-count tests and restrictions remain in force.


### Two-axis loop-memory verifier (preparation only)

`shader/loop_memory.hpp` exposes prove_loop_memory(accesses, buffer_words,
lanes, iterations). This is a standalone proof utility, NOT a newly accepted
shader instruction shape. The strict copy-loop lowering below now calls this verifier;
other loop-memory patterns remain rejected.

- Access map is a uint32 load/store at byte offset origin + lane*lane_stride
  + iteration*iteration_stride relative to the aligned SSBO guest base.
  Strides and origin are nonnegative uint64. Buffer words 1..65536; both
  axes 1..64; access list 1..32. Pass iterations=64 for runtime 1..64 counts:
  every permitted execution is then a subset of the proven points.
- Verifies stride products and sums before arithmetic, then max address
  against last aligned uint32 offset. Origin and advancing strides must be
  four-byte aligned. An inactive axis (extent one) may have any unused
  stride; its product is zero. No signed strides/modulo wrap assumed.
- Exact enumeration and sorted address groups reject ANY store collision or
  read/write alias across lane, iteration and separate access maps. Read/read
  overlap is allowed. This deliberately disallows same-lane sequential
  read-modify-write or loop-carried dependencies: no ordering/barrier
  exception is inferred. Diagonal collisions across both axes are detected
  even when both nonzero strides appear individually disjoint.
- Success returns ok=true and checked_points=accesses*lanes*iterations.
  Failure returns ok=false, a nonempty diagnostic, checked_points=0. No
  partial proof or partly safe dispatch is returned. At most 131072 points
  are enumerated; no runtime backend or guest state is touched.
- Tests cover overflow of stride products and origin/axis sums, alignment,
  last-iteration bounds, aliases across both axes/accesses, exact tiled
  buffers, transposed maps, one-word-short buffers, repeated reads, inactive
  axes, maximum SSBO coverage and access budget. An independent address-event
  map oracle checks 12000 deterministic small cases (both success/failure,
  aligned and misaligned). Existing SPIR-V/lavapipe loop corpus remains
  passing; it is NOT proof of loop-body memory execution.
- The copy-loop lowering below binds accepted address expressions to these
  maps, retains guards inside the body and keeps checked-count entry policy.
  This proof is not a security sandbox.


### Dedicated guarded copy-loop lowering

- Recognizes exactly eight decoded instructions: s_mov_b32 counter,count;
  flat_load_dword data,srcPair+offset; flat_store_dword dstPair+offset,data;
  v_add_nc_u32 srcLow,srcStep,srcLow; v_add_nc_u32 dstLow,dstStep,dstLow;
  s_sub_u32 counter,counter,1; s_cmp_eq_u32 counter,0;
  s_cbranch_scc0 back-to-load. Scalar count 1..64 may be literal/inline or
  declared runtime uniform/copy under the existing error105/entry-guard
  contract. Exact decoded instruction boundaries include literal words.
- Source/destination low words need proven non-wrapping affine lane maps;
  high words must be constant and equal the binding base high. Binding must
  not cross a low-word boundary. Steps are literal/inline nonnegative bytes;
  both final pointers after N increments must fit uint32, even though only
  iterations 0..N-1 access memory. Runtime counts prove all 64 iterations.
  FLAT offsets are separately included in proof and emitted guards.
- Calls prove_loop_memory for the read/store maps at origin-base+offset,
  lane stride and iteration step. Every store is distinct across lanes and
  iterations; every read is disjoint from every store. Input broadcasts are
  allowed, output broadcasts/collisions are not. Tiled and transposed maps
  work; no hardcoded lane-stride-four rule is substituted for exact proof.
  Existing prior-read/output envelopes are also checked conservatively.
- One guest store instruction total, one loop, no combined selection region.
  Pointer pairs cannot overlap low/high words; loaded data cannot overwrite
  any address word and must be the stored register. Strict existing FLAT
  modifier/segment/saddr/reserved-field checks remain. No additional body
  ALU, read-modify-write, in-place update, memory dependency, signed stride,
  full64 loop pointer, barriers, atomics, nested/repeated loop or general CFG.
- LoopCopy IR carries source/dest low SSA refs a/b, c=count literal or input
  node, d=0 constant /1 runtime mode, e/f=pointer steps, g/h=FLAT byte offsets.
  Header Phi values carry pointers, iteration and last loaded word. Body has
  separate structured load/store guards including alignment/range and
  offset-add overflow; AccessChain and load/store occur ONLY in valid paths.
  Invalid load=0/store=skip remains the low-level defensive policy, though
  accepted guest shapes prove those accesses valid for every permitted count.
- LoopCopy result is last loaded word; LoopCopySource/Dest project final
  pointers at loop exit. Guest data/pointer/counter registers preserve exact
  final values. Constant-count pointer maps keep final affine facts only when
  proven; runtime final maps lose affine facts rather than guessing count.
  Counter exits zero and stale carry is invalidated. loop_memory=true and
  structured_loop=true identify the path; guarded_memory=true is retained.
- Low-level emitter checks count mode/limits, refs, offsets, runtime guard
  match and non-crossing binding. It is NOT an alias-proof API: callers
  bypassing translator own alias semantics. Guards still prevent OOB pointer
  construction. Use translator plus checked dispatch for the full contract.
- 26 new modules validate for Vulkan 1.0 and execute on lavapipe: sixteen
  local-size/count combinations (1/2/8/64 lanes, 1/2/16/64 counts), three
  changing-runtime-count modules, offset and transposed maps, and five
  low-level final-pointer/data/invalid-load/store probes. Whole buffers match
  for every lane/iteration, wrap-pattern input, untouched input and sentinel.
  Runtime invalid count is rejected before dispatch and raw entry guard is
  verified no-op. Byte API preserves exact module and loop-memory metadata.
- Negatives cover last-iteration OOB, two-axis aliases/collisions, alignment,
  pointer wrap, FLAT modifiers/reserved fields, data/address alias, wrong
  increments/counter/compare/backedge, dynamic/unproven/mismatched-high pairs,
  zero/oversized/undeclared count, short full-domain runtime extent, boundary
  crossing, repeated/combined regions, extra outside store and malformed IR.

## render_offscreen_rgba

`Status render_offscreen_rgba(w, h, vert_spirv, frag_spirv, vertex_count, clear[4], &rgba)`. Size 1..4096 per axis; vertex_count a nonzero multiple of 3, at most 3000; shaders use entry point `main`; output is w*h*4 bytes, row-major, top row first, RGBA8 UNORM. Fails with an error naming the stage when no graphics queue exists or the driver rejects the SPIR-V. Headless only: no swapchain or presentation.

## render_frames_rgba

`Status render_frames_rgba(w, h, vert_spirv, frag_spirv, vertices, frame_push, clear[4], &frames)`. `vertices` is interleaved float2 position + float3 colour (5 floats per vertex, count a multiple of 3, max 3000 vertices); vertex shader inputs are location 0 (vec2) and 1 (vec3). `frame_push` has one entry per frame (1..64 frames), each 1..16 words, same size for all, pushed to the vertex stage at offset 0. `frames[i]` is w*h*4 RGBA8 bytes. Same size and error rules as render_offscreen_rgba. Headless only.

## render_mesh_frames_rgba

`Status render_mesh_frames_rgba(w, h, vert_spirv, frag_spirv, vertices, indices, depth, frame_push, clear[4], &frames)`. `vertices`: float3 position + float3 colour per vertex (multiple of 6 floats, max 65536 vertices), vertex locations 0 and 1 (vec3). `indices`: uint32 triangle list, multiple of 3, max 30000, every index must be < vertex count. `depth` enables a D32_SFLOAT depth buffer (cleared to 1.0, LESS); fails with a named error when the device lacks D32 depth attachments. Push constants and frame rules as render_frames_rgba. Headless only.

## pnrGfx* (PNR extension, in libpnr_psabi.so)

`int pnrGfxInit(w,h)` (1..1024; 0 ok, negative = Blocked with `pnrGfxLastError()`), `int pnrGfxDrawCube(float yaw, float pitch)` returns covered pixel count or a negative error, `int64 pnrGfxPresentToFile(const char* jail_path)` writes a binary PPM through the game's jail and returns bytes written, `uint32 pnrGfxFramesRendered()`. Process-local state, one renderer per game process. Not a Sony ABI.
