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
| 02b | PRX system libraries | `prx/` | Implemented (libkernel: usleep, sleep, alloc, free, create_thread, map_memory, munmap, mtypeprotect, query_memory_protection over POSIX mmap; jailed file calls (open/read/write/close/lseek/fstat) over FileSystemService; libSceLibcInternal: printf bounded subset, malloc, free, memcpy, memset, strlen, strcmp; bound by NID; taxonomy per AnyPS5 core/libs/prx, implementations original), Tested |
| 02c | Import resolution | `loader/import_resolver.*` | Implemented (DT_NEEDED + undefined .dynsym parsed bounds-checked; every import classified host / prx-implemented / prx-declared / unknown and reported at launch), Tested |
| 02d | PS-ABI shim + PS-style demo | Implemented / Tested | `libpnr_psabi.so` exports real sce functions (heap, sleep, thread, mmap family, jailed file calls over `PsFileTable`); `examples/ps_demo.cpp` is a PS-style binary whose imports all classify prx-implemented and dispatch through the shim at launch (`LD_LIBRARY_PATH` + `PNR_JAIL_ROOT`) |
| 03 | Compatibility Layer | `compat/` | Implemented (memory, timing, filesystem, threads), Tested |
| 04 | Graphics Engine (Vulkan) | `graphics/` | Implemented (first slice): dlopen Vulkan backend (instance/device/compute queue/SSBO/pipeline/dispatch/readback) + word-level SPIR-V emission (compute write-back shape), Tested on software Vulkan (Mesa lavapipe); `shader/rdna.cpp` RDNA decoder (families + named subset, AnyPS5-attributed taxonomy) Implemented, Tested; `shader/translate.cpp` restricted straight-line integer-compute RDNA-to-SPIR-V path (SSA loads + add/sub/bitwise operations) Experimental, Tested on synthetic bytes + lavapipe; explicit bounded guest-range-to-SSBO test ABI, static offsets + guarded runtime low-word accesses + checked host buffer length; 1..64-invocation workgroup test ABI with affine/disjoint-address proof, no general wave/address semantics; workgroup-uniform SCC0 forward conditional selection (move-only body + Phi, lane-varying data, identical affine merge proof) Experimental, Tested; explicit runtime uniform SGPR push-constant ABI (0..16 words) Experimental, Tested; host-policy binary32 VOP2 add/sub/mul Experimental, Tested (not PS5-exact FP controls); single-invocation full64 pointer pair arithmetic/guarding Experimental, Tested; bounded constant/runtime-uniform-count accumulator loops (real LoopMerge/Phi, invalid count rejected before dispatch plus GPU entry guard; no body memory) Experimental, Tested; two-axis loop-memory proof utility + dedicated guarded dword-copy loop lowering Experimental, Tested (exact collision/alias proof, constant/runtime-uniform count, no dependencies/full64 loop pointers); render passes + full stages Planned |
| 05 | Adaptive Performance Engine | `performance/` | Planned |
| 06 | Laptop Optimization Manager | `performance/laptop` | Planned |
| 05 | Adaptive Performance Engine | `ape/` | Implemented (deterministic rules: CPU/RAM class heuristic, frame stats, closed-loop keep/revert controller), Tested |
| 06 | Laptop Optimization Manager | `laptop/` | Implemented (battery/balanced/max-performance caps; unknown power state never assumed AC), Tested |
| 08 | Game Services | `services/` | Implemented: virtual filesystem (jailed areas, per-game/user saves), input (evdev enumeration, mapping profiles, virtual device), audio (interface + null backend), Tested; host audio backends Planned |
| 07 | Host OS Abstraction | `platform/` | Implemented (Linux backend: CPU/memory/power/process, Tested; Windows backend: Win32 hwinfo, ELF launch Blocked) |
| 08 | Game Services (audio/input/storage) | `services/` | Planned (save-data storage exists inside `compat`) |
| 09 | Game Manager (library) | `library/` | SQLite data layer Implemented, Tested; UI Planned | Run-by-id (`pnr run <id>`) resolves installed games with per-game jails, launch history, and honest compatibility advancement (Untested -> Launches Only); UI Planned.
| 10 | Desktop Launcher UI | `apps/launcher` | Planned (CLI exists: `apps/pnr_cli.cpp`) |
| 11 | Security Manager | `security/` | Partial (process isolation, application-level file path routing, package traversal/checksum rejection implemented). The current "jail" is virtual path routing, NOT an OS-enforced security sandbox: direct host syscalls bypass it. OS-enforced sandboxing (seccomp/namespaces/job objects) with adversarial tests is Planned here |
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
- FileSystemService owns a jail root; all virtual paths resolve inside it. This is application-level path routing, not a security boundary: code that calls the host directly bypasses it.

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

## Offscreen render slice (Module 04)

`VulkanBackend::render_offscreen_rgba` draws a triangle list (positions generated by the vertex shader from the vertex index, no vertex buffers) into an RGBA8 image, copies it to host memory and returns the pixels. Shaders are hand-authored SPIR-V in `graphics/demo/` (validated with spirv-val, vulkan1.0). There is no swapchain, no window system and no display: rendering is headless only. Status: Implemented, Tested on software Vulkan. Not done yet: vertex buffers, depth, uniforms for graphics, translating RDNA pixel/vertex shaders, animation. The animated 3D scene remains the Phase 2 exit criterion.

`render_frames_rgba` adds an interleaved vertex buffer (float2 position, float3 colour at locations 0 and 1) and per-frame vertex-stage push constants (1..16 words, up to 64 frames). The demo `graphics/demo/spin.*.spvasm` rotates the triangle by a pushed cos/sin pair. Frames are submitted and read back one at a time; there is no frame pacing or presentation. Still missing for the Phase 2 exit criterion: 3D (depth, matrices, indexed mesh) and a real scene.

## 3D slice

`render_mesh_frames_rgba` draws an indexed mesh (float3 position, float3 colour, uint32 indices) with an optional D32_SFLOAT depth buffer (compare LESS) and a per-frame push-constant MVP (16 words, column-major). `graphics/mat4.hpp` holds the matrix helpers and the unit cube; `graphics/demo/cube.vert.spvasm` multiplies position by the pushed matrix. Tests compare every pixel of an 8-frame rotation with a CPU ray-vs-box caster and check depth ON versus OFF. Limits: offscreen only, no culling, lighting, textures or vertex-shader translation from RDNA; the slice proves the draw path, not a game renderer.

## Game-driven rendering

`libpnr_psabi.so` exports a small PNR extension, `pnrGfxInit`, `pnrGfxDrawCube`, `pnrGfxPresentToFile`, `pnrGfxFramesRendered`, `pnrGfxLastError`. They are PNR names, not Sony APIs, and the import resolver classifies any `pnr*` import as `pnr-extension`. A game calls them from its own frame loop; the shim runs the 3D draw path in the game process and "presents" by writing a PPM through the game's jail. `pnr run` step 5 reports the offscreen renderer (or BLOCKED without a Vulkan device) and step 8 counts the frame files. No swapchain, display, audio or input here, and `pnrGfxDrawCube` is the hand-authored cube pipeline. `pnrGfxDrawCubeRdna` and `pnrGfxRdnaFramesRendered` route the vertex stage through the translated-RDNA compute-as-vertex bridge below (PNR test ABI, not a real vertex stage); `ps_cube_demo` presents those frames and byte-compares them with the hand-authored reference.

## Compute-as-vertex bridge (Experimental)

`graphics/rdna_vertex.hpp` builds restricted RDNA words, translates them
with `translate_compute_words` (24 lanes, 16 push-constant scalars as
s0..s15) and runs one dispatch per clip component. The readback feeds
`render_clip_mesh_rgba` with a pass-through vertex shader. This is a PNR
test ABI: no vertex-stage inputs, exports or parameter cache, no Sony
driver state. The single-store translator limit is why it takes four
dispatches. Float results follow host Vulkan policy.

`pnrGfxSetMesh` / `pnrGfxDrawMesh` / `pnrGfxMeshDraws` let a game supply its own vertices (x,y,z,r,g,b) and triangle indices. The shim validates and copies them (1..24 vertices because the RDNA kernel uses one lane per vertex, 3..384 indices, finite floats, in-range indices) and keeps the previous mesh when an upload fails. `ps_mesh_demo` exercises this under `pnr run`. Not a Sony API.

`pnrGfxSetLitMesh` / `pnrGfxSetLight` / `pnrGfxDrawLitMesh` / `pnrGfxLitDraws` add per-vertex normals and one directional light (Lambert, ambient 0.25, Gouraud). Mode 0 computes the dot in a hand-authored vertex shader; mode 1 uses the translated-RDNA transform kernels plus `rdna_dot3_kernel` (s0..s2 = object-space light, unfused binary32 mul/add, no clamp since max() is outside the translated subset). A hand-authored fragment shader applies `0.25 + 0.75*max(d,0)`. Rounding between the two vertex paths agrees within a small tolerance, not by contract. `ps_lit_demo` exercises it under `pnr run`. No specular, textures, shadows or multiple lights.

`pnrGfxSetTexture` / `pnrGfxSetTexturedMesh` / `pnrGfxDrawTexturedMesh` / `pnrGfxTexturedDraws` add one sampled RGBA8 texture. `render_core` creates a sampled image, view, sampler and staging buffer, uploads with two barriers and a buffer-to-image copy, and binds a combined image sampler at set 0 binding 0 (fragment stage). Hand-authored vertex and fragment shaders (`graphics/demo/tex.*.spvasm`). No mipmaps, no anisotropy, no RDNA path; filtering is the host driver's.

`pnrGfxSetLitTexturedMesh` / `pnrGfxDrawLitTexturedMesh` / `pnrGfxLitTexturedDraws` combine the lit and texture paths: vertex attributes 0 position, 1 tint, 2 uv, 3 normal (`MeshInput.extra_floats == 5`), push constants MVP + object-space light, hand-authored `littex.vert` / `littex.frag`. The light comes from `pnrGfxSetLight`, the texture from `pnrGfxSetTexture`. No RDNA path for this combination.
