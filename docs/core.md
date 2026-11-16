# Translation core (slices C1-C5: module loading, NID binding, libkernel ABI, libc, PM4 decode, register meaning, GPU address map, first draw, audio, pad, save data)

Destination: running real PS5 games on a PC by translating the game's calls to the host, not by
emulating a console. This page covers the first slice of that core. It is the current state on the
road, not the end of it.

## Status
| Item | State |
|---|---|
| `NID#lib#mod` symbol names parsed and decoded (`loader/sony_symbols`) | Implemented, Tested |
| Import coverage report for ELF files with or without section headers (`pnr imports <elf>`) | Implemented, Tested |
| In-process module loader: maps PT_LOAD, applies RELATIVE, 64, GLOB_DAT and JUMP_SLOT relocations, binds imports by NID to PRX host functions (`loader/module_loader`) | Implemented, Tested on synthetic modules only |
| Run a loaded module's entry in a forked child (`pnr module-run <elf>`) | Implemented, Tested on synthetic modules only |
| Refuse a SELF container with a clear message | Implemented, Tested |
| Real PlayStation executables | Untested: no real file has been loaded |
| TLS and TCB: PT_TLS image copied per thread, x86-64 variant II layout, FS base set for guest code and switched back to the host base around every host call (`loader/guest_runtime`) | Implemented, Tested on synthetic modules only |
| Entry parameters: `entry(args, exit_handler)`, args block with argc, argv, no envp | Implemented, Tested (PNR's own convention for the args block, not verified against a console) |
| Sony protection and mmap flag translation, `mmap` (anonymous only), clock id translation and `sceKernelClockGettime` | Implemented, Tested. Only SHARED, PRIVATE, FIXED and ANON are FreeBSD-standard values. NO_OVERWRITE, NOCORE, NO_COALESCE, GPU bits and clock ids come from public homebrew knowledge, not verified on a console. Unknown bits are refused |
| Guest threads and mutexes: `scePthreadCreate`, `scePthreadJoin`, `scePthreadSelf`, `scePthreadMutex{Init,Lock,Unlock,Destroy}` | Implemented, Tested (32 threads at once; no condition variables, rwlocks, TLS keys or thread attributes yet) |
| Init/fini arrays, dependent modules | Planned |
| libSceLibcInternal subset: memmove memcmp memchr strcpy strncpy strcat strncmp strchr strrchr strstr calloc realloc snprintf sprintf puts putchar atoi strtol abs labs toupper tolower isdigit isalpha isspace srand rand exit, plus the earlier printf malloc free memcpy memset strlen strcmp (`prx/libc_abi`) | Implemented, Tested (unit and from machine code) |
| C-style formatter: flags `-` and `0`, width, length h hh l ll z j t, conversions d i u x X o c s p | Implemented, Tested. Floating point formats are NOT supported and print literally |
| libSceSysmodule `LoadModule`, `IsLoaded`, `UnloadModule` | Experimental stubs: ids are recorded, nothing is loaded, id values unverified |
| Libc imports beyond this subset, floating point arguments, qsort and other callbacks into guest code | Planned |
| PM4 command buffer decode and register-state tracking: packet types 0, 2, 3, SET_*_REG windows, draw and dispatch capture with the register state at each draw, indirect buffers through an address resolver, `pnr gnm-decode <file>` (`graphics/pm4`) | Implemented, Tested on SYNTHETIC buffers only. Opcode numbers, header layout and register windows verified against the public Linux kernel header soc15d.h. Draw packet body layouts are from public driver usage and NOT verified on a PlayStation |
| Register meaning and draw bindings (`graphics/amd_regs.hpp`, `graphics/gfx_state`): shader addresses, user data, viewport, color target 0, primitive type | Implemented, Tested on SYNTHETIC buffers. Per public AMD headers, unverified on PlayStation |
| GPU virtual address map (`graphics/gpu_va`): range table to host memory, no stitching across regions, PM4 indirect-buffer resolver | Implemented, Tested on synthetic data. No page tables or coherency model |
| Draw from tracked state to one Vulkan draw (`graphics/gfx_draw`): non-indexed triangle list, PNR test ABI, pixels read back and copied to the colour target address | Experimental, Tested on a SYNTHETIC command buffer with hand-assembled RDNA vertex kernels. Pixel shaders are NOT translated (fixed colour). No real game shader or command buffer has run. Needs a Vulkan device (tests skip without one; measured on Mesa llvmpipe, a software device) |
| libSceAudioOut `Init`, `Open`, `Output`, `Close` (`prx/hw_abi`) | Experimental, Tested from the registry by NID. Frames are captured and handed to an optional host sink. With no sink nothing is played. No mixing, latency or timing model. Argument order and format codes come from public homebrew documentation, unverified on PlayStation |
| libScePad `Init`, `Open`, `ReadState`, `Close` with a host input mapping (evdev style names to pad bits and 0..255 axes) | Experimental, Tested. Writes only buttons, sticks and triggers (offsets 0..9) of the guest's buffer. Button bit values and the offsets are from public homebrew documentation, unverified on PlayStation. One pad, no touchpad, motion, haptics or lightbar |
| Save data model (`services/save_data`): named save directories in the jailed per-user, per-game area, name validation, quota, list and remove | Implemented, Tested. The PlayStation SaveData API is NOT bound (structure-heavy, layouts unverified): Planned. The jail is path routing, not a security sandbox |
| Indexed draws, vertex fetch from real shader resource descriptors, real pixel shaders, depth, blending | Planned |
| Windows (the loader uses mmap and fork) | Blocked |

## What it proves
A hand-assembled x86-64 module, generated by `pnr_make_ps_elf` (not a game, no Sony code), names its
imports as NIDs. PNR maps it, applies its relocations, binds `printf` and `strlen` by NID, runs the
machine code natively in a child process and gets the right result. An import whose NID is in no table
makes the load fail with a message naming it, and nothing runs.

## Limits, stated plainly
- PNR does not decrypt, unsign or unprotect anything. A SELF file is refused. The input is a plain
  ELF that the user already has.
- Imports are called with up to 6 integer arguments and at most 64 imports are bound per process.
  No floating point arguments and no varargs beyond the six registers.
- Imports are only mapped in the launcher process. Execution happens in a forked child.
- The set of implemented system functions is small. `pnr imports` shows exactly what a given ELF still needs.

## Slice C2 notes
- The synthetic kernel module (`pnr_make_ps_elf OUT.elf --kernel`) is hand-assembled machine code. It checks
  argc, the TCB self pointer, initialised and zeroed TLS data, a TLS write, `mmap` with Sony flags, a clock
  read, a mutex cycle, and a thread that runs with its own TCB. It exits 0 only if all 14 checks pass.
  `--kernel-no-tls` drops PT_TLS and must exit 3.
- Host calls need the host FS base (glibc keeps its own state there). Each import thunk switches FS to the host
  base, calls, and switches back. The guest TCB copies the host stack-protector canary so both sides agree.
- `sceKernelMmap`-style 7-argument calls and floating point arguments are not supported by the thunks.
- Linux only. Windows stays Blocked.

## Slice C3 notes
- `pnr_make_ps_elf OUT.elf --libc` builds a machine-code module that calls snprintf, strcmp, strtol, calloc,
  memmove, strlen and printf through NID-bound imports and prints `C3 libc ok 42`. It exits 0 only if all checks pass.
- `%d` without `l` now has C semantics (32-bit), so a negative int passed in a register prints correctly.
- These functions trust the guest's pointers like a real libc. A bad pointer crashes the guest child, not the launcher.
- `sprintf` is capped at 4096 bytes. `rand` is PNR's own generator, not Sony's.

## Slice C4a notes (PM4 decode)
- PM4 is the AMD packet format. This slice only decodes packets and tracks registers; it submits nothing to Vulkan.
- Unknown opcodes are counted and printed as UNHANDLED. Register numbers are not given meaning unless verified
  against a public source. A write that runs past its register window is stopped and reported.
- Indirect buffers hold GPU addresses. Without a resolver they are counted and reported as not followed.
- `pnr gnm-synth OUT` writes a SYNTHETIC test buffer. No real game command buffer has been decoded.

## Slice C4b notes (register meaning)
- `graphics/amd_regs.hpp` names a small set of registers: VS/PS shader addresses and resource words, user data,
  viewport scale/offset, color target 0, primitive type. Absolute number = `mm` offset from gc_10_3_0_offset.h
  plus segment base (SH 0x1260, context and uconfig 0xA000, from sienna_cichlid_ip_offset.h).
- Sources: torvalds/linux `drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_3_0_offset.h`,
  `.../include/sienna_cichlid_ip_offset.h`, and shadPS4 `regs_vertex.h` for primitive type numbers only.
- Label for all of it: per public AMD headers, unverified on PlayStation. Nothing here was checked against a real console.
- `extract_bindings` reads the tracked registers at a draw and lists what is missing in plain words.
- `pnr gnm-decode` now prints register names, per-draw bindings and the written registers.

## Slices C4c-C4e notes (address map, draw, triangle)
- `GpuAddressSpace` maps GPU address ranges to host memory. A read must fit inside ONE mapping; nothing is stitched.
  `pm4_resolver()` lets `pm4::interpret` follow indirect buffers through it.
- `execute_draw` is the first GPU work. It is a PNR TEST ABI, not the PlayStation one: the vertex-stage memory holds a
  "PVS1" blob (four restricted RDNA kernels), VS user data 0..3 point at a matrix and a vertex buffer, and the pixel
  stage must be set and mapped but is not translated (a fixed pass-through colour is used and the result says so).
- A real GCN/RDNA vertex program is refused with a plain message. Viewport registers are only checked against the target size.
- The test builds a PM4 buffer, runs it through the decoder, address map, translator and Vulkan, and checks real pixels:
  red-pixel count within 4% of the triangle area, clear colour in all four corners, and the target memory byte-equal to the image.
- Source of every register meaning is still public AMD headers, unverified on PlayStation.
- `pnr gnm-triangle <out.png>` runs that test scene end to end and writes a 256x256 PNG (stored deflate, uncompressed).
  The image is the Vulkan draw read back from the colour target memory. Needs a Vulkan device; exits 3 with SKIP without one.

## Slice C5 notes (audio, pad, save data)
- These are host models behind a few symbols, not the PlayStation services. Signatures use integer and pointer
  arguments only. Nothing here was checked on a console.
- Audio: `sceAudioOutOpen(user, type, index, len, freq, format)` accepts 44100 or 48000 Hz, length 1..4096 frames,
  formats S16 and float, mono and stereo, up to 8 ports. Float samples convert to S16 and clip at full scale.
  `sceAudioOutOutput` returns at once. A real-time clock and a host audio device are Planned.
- Pad: host events are applied with `pad_apply_host_event`. Sticks map to 0..255 with 128 at centre.
  Wiring a live evdev device to it is Planned.
- Save data: names are 1..32 characters of letters, digits, dot, underscore and dash, not starting with a dot.
  A write that would pass the directory quota is refused and leaves nothing behind.

## Slice S1 notes (condition variables and semaphores)
- New libkernel symbols: `scePthreadCondInit/Wait/Timedwait/Signal/Broadcast/Destroy` and
  `sceKernelCreateSema/WaitSema/SignalSema/DeleteSema`. Status: Implemented, Tested.
- Handles are registry ids, never raw pointers: a forged, stale or destroyed handle returns an error and the host never
  dereferences guest memory for it. (The older mutex handle is still a host pointer; moving it onto the registry is Planned.)
- Semaphores: signalling past the maximum is refused and leaves the count unchanged; waiting for more than the maximum
  is an argument error; destroying wakes blocked waiters with an error. Timeouts are microseconds on the monotonic clock.
- Tests drive the same entry points the PRX symbols call from real host threads (4 threads woken by broadcast, a blocked
  semaphore waiter, destroy-while-waiting) and check all 10 symbols are registered. They are NOT yet exercised from guest
  machine code through the NID path, and argument layouts follow public pthread/semaphore conventions, unverified against a console.
- Not done: event queues, rwlocks, thread-local keys, thread priorities and affinity, cancellation.

## Slice S2 notes (fused multiply-add vertex path)
- The RDNA decoder now names one VOP3A opcode: `v_fma_f32` (331, from AMD's public "RDNA 2" Instruction Set
  Architecture, Table 83) and decodes the third source operand. Neighbouring opcodes stay unnamed and unsupported.
- The translator accepts a restricted `v_fma_f32`: vector destination, register/constant sources, no ABS/OPSEL/CLMP
  clamp or OMOD/NEG modifiers (any set modifier bit fails translation). Status: Implemented, Tested.
- It emits GLSL.std.450 `Fma` (fused, single rounding) instead of mul+add, which matches RDNA `v_fma_f32` semantics
  more closely; the mul+add path keeps its NoContraction pair. Still host Vulkan FP policy, not PS5 rounding modes.
- A fused variant of the compute-as-vertex component kernel transforms the same cube through translated RDNA machine
  code. Measured on lavapipe over 8 frames x 24 vertices x 4 components: bit-identical to the CPU `std::fma`
  reference (max diff 0), within 2.4e-7 of the mul+add path, and 0 of 9216 pixels differing per frame from the
  hand-authored vertex shader.
- Tests: decode naming, modifier rejection (ABS/CLMP/NEG/OMOD), translation flags, spirv-val acceptance of the fused
  module, on-GPU execution against both CPU references, and the pixel comparison above.
- Not done: other VOP3 opcodes, source modifiers, SDWA/DPP, 64-bit `v_fma_f64`, output modifiers.

## Slice S3 notes (window presentation)
- `graphics/window.hpp/.cpp`: a `WindowPresenter` that opens an SDL2 window, enables SDL's Vulkan instance
  extensions, creates a real `VkSurfaceKHR`, picks a graphics+compute+present queue family, enables
  `VK_KHR_swapchain` and presents frames through a real swapchain (acquire, layout transitions, copy, present,
  semaphores). Status: Implemented, Tested under Xvfb.
- Frames come from the existing offscreen renderers via a host readback + staging upload each frame. Rendering
  directly into the swapchain image is Planned; nothing about the roundtrip is hidden.
- BGRA-only surfaces get a real byte swizzle during upload (llvmpipe here presents B8G8R8A8, so that path is the
  tested one). Fixed-size window: a swapchain resize fails honestly instead of stretching.
- The backend resolves the KHR surface/swapchain entry points optionally: a headless host without them stays a
  working compute/offscreen backend and any present attempt names the missing function.
- `pnr-window-demo` (optional `-DPNR_BUILD_WINDOW=ON` build, needs SDL2 dev files; headless CI does not build it,
  same policy as `pnr-ui`) spins the cube with clip positions from the slice S2 translated-RDNA fused multiply-add
  path and presents every frame. `pnr_window_smoke` runs it under Xvfb: 12 presented frames plus a saved PPM.
  Measured locally: 12/12 (smoke) and 380/380 presented frames, zero present errors.
- Not done: swapchain resize, frames in flight, direct-to-swapchain rendering, pad input, real display or hardware
  GPU verification.

## Slice S4 notes (pad input)
- `input/sdl_pad.hpp/.cpp`: a LIVE pad source for the services/input model. The earlier backends only enumerate
  devices (evdev) or replay scripted events (virtual); `SdlPadSource` opens the first SDL joystick, polls real
  pad state each frame and emits the same `InputEvent` control names the evdev/C5 pad model uses
  (`BTN_SOUTH`..., `ABS_X`...), so live pad events can feed `pnr::prx::pad_apply_host_event` unchanged.
  Status: Implemented, Tested (virtual pad); a physical USB/Bluetooth pad is Untested (no hardware here).
- The axis/button-index to name mapping is PNR's own positional convention; SDL's controller database is not
  consulted, so an unusual physical pad can order buttons differently. Documented in `input/sdl_pad.cpp`.
- `SdlVirtualPad` drives SDL's virtual joystick (SDL 2.0.14+): hosts without hardware - Xvfb, containers -
  exercise the REAL SDL joystick path end to end. No mock: the device is attached through SDL, opened by
  `SdlPadSource` like any joystick, and driven through SDL's own setters.
- `services/input.hpp/.cpp` gains `normalize_axis` (clamp, symmetric deadzone, linear rescale to the rim),
  unit-tested in the default build (`tests/pad_tests.cpp`, 25 checks).
- `pnr-window-demo` reads the pad: left stick X scales the spin, holding the south button pauses it.
  `--virtual-pad` runs a scripted pattern (stick right/left/neutral, button down/up) and the demo verifies the
  events arrived (`pad-check: ok ...`), so `pnr_pad_smoke` (Xvfb) tests input and presentation together.
  Without a pad the demo behaves exactly as in slice S3.
- Built only with `-DPNR_BUILD_WINDOW=ON` (needs SDL2); headless CI does not build it, same policy as
  `pnr-window-demo` and `pnr-ui`.
- Not done: physical-pad verification, controller-database mapping, multiple pads, hotplug during play,
  hat-to-dpad naming beyond hat 0 (extra hats report as zeroed axes), feeding the C5 guest pad model from the
  live source inside a `pnr run` game loop.

## Slice S5 notes (wider RDNA float ALU coverage)
- The translator's float subset grows by eight instructions, opcode numbers from AMD's public "RDNA 2" ISA
  (VOP1/VOP2 tables): `v_subrev_f32` (VOP2 5), `v_min_f32` (15), `v_max_f32` (16), `v_mac_f32` (31),
  `v_rcp_f32` (VOP1 42), `v_sqrt_f32` (51), `v_cvt_f32_i32` (5, truncation toward zero) and
  `v_cvt_i32_f32` (8). Status: Experimental, Tested.
- `v_mac_f32` is emitted as multiply-then-add (two roundings), matching RDNA semantics; the fused
  single-rounding form stays with `v_fma_f32` (slice S2). A `v_mac_f32` with an undefined destination
  accumulator is rejected.
- `v_rcp_f32` is emitted as a correctly-rounded `1.0f/x` (OpFDiv). RDNA's own rcp is approximate, so this
  deliberately is NOT bit-identical to hardware - documented, not hidden.
- Min/max NaN handling follows the host GLSL.std.450 FMin/FMax, not RDNA's operand-pick rule; the conversion
  ops introduce a signed 32-bit SPIR-V type only in modules that use them (all other modules stay
  byte-identical).
- `tests/rdna_alu_tests.cpp`: synthetic 24-lane kernels on the slice S2 vertex layout, executed on the real
  Vulkan backend with host readback against a CPU float reference. Measured: sub/subrev/min/max/mac exact
  (0 ulp), rcp exact division (0 ulp), sqrt (0 ulp), both conversions exact; spirv-val accepts a mixed
  min/rcp/cvt module. Three earlier "outside subset" rejection checks were flipped to acceptance with the
  documented caveats.
- Not done: SDWA/DPP/source modifiers remain rejected, denorm/rounding modes stay host policy, wave/EXEC
  semantics unchanged, hardware GPU untested, min/max signed-zero and NaN picks not matched to RDNA.

## Slice S6 notes (translated perspective divide + viewport transform)
- New kernels in `graphics/rdna_vertex.hpp`: `rdna_post_ndc_kernel` (inv_w = `v_rcp_f32`, ndc_c =
  clip_c * inv_w) and `rdna_post_win_kernel` (win_c = `v_fma_f32`(ndc_c, scale, offset); the depth
  component clamps to [zmin, zmax] with `v_max_f32`/`v_min_f32`). The translated subset allows one
  store per kernel, so the post-transform is one kernel per output component - six dispatches over
  a shared 5-region layout (clip x/y/z/w in, one component out). Status: Experimental, Tested.
- Uniform ABI (8 words): xscale, xoffset, yscale, yoffset, zscale, zoffset, zmin, zmax. The rcp is
  the slice S5 correctly-rounded `1.0f/w`, NOT RDNA's approximate rcp; w = 0 propagates IEEE
  inf/NaN with no guard. Host Vulkan FP policy throughout; min/max keep their documented host NaN
  semantics.
- New "PVS2" vertex blob (magic 0x32535650, 7 kernels: 4 clip + 3 ndc): `execute_draw` runs the
  divide in translated RDNA code and feeds the rasterizer ndc with w = 1. Window/depth outputs are
  computed by `rdna_post_transform_clip` for verification; on-screen viewport scale/offset stays
  host fixed-function (VkViewport), and viewport depth bounds are not tracked (0..1 used).
- `tests/rdna_post_xform_tests.cpp`: 24-lane execution vs a CPU division/fma reference measured
  0 components differing of 144 (bitwise, 0 ulp), w = 0 gives +inf, and the PVS2 draw measured
  0 pixels differing of 4096 against the PVS1 (GPU-side divide) path. spirv-val accepts the
  window/depth module.
- `pnr-window-demo` now runs clip AND the perspective divide through translated RDNA every frame.
- Not done: indexed draws, other primitive types, viewport depth bounds, w guards/clipping,
  RDNA-accurate approximate rcp, SDWA/DPP, hardware GPU verification.

## Slice S7 notes (indexed draws)
- `execute_draw` accepts DRAW_INDEX_INDX: the index buffer is fetched from guest memory through the
  GPU address space (16-bit or 32-bit indices, bounds-checked against the mapping), and the vertex
  transform runs over the referenced vertices (max index + 1; slice S9 lifts the lane limit
  with PVS3 batching).
  Indices pass through unchanged, so winding and shared vertices behave as on hardware. The result
  notes the guest-memory index fetch. Status: Experimental, Tested.
- Refusals are honest: unmapped index buffer, an index count that is not a positive multiple of 3,
  and an index past the vertex buffer (caught at the vertex fetch) all fail with plain reasons.
  Without an INDEX_TYPE packet the PM4 interpreter's default (32-bit) applies; that default is
  documented, not hidden.
- Tests (gfx_draw_tests.cpp): a 4-vertex square drawn indexed (32-bit and 16-bit index buffers,
  byte-identical pixels), pixel-identical to the same square drawn with DRAW_INDEX_AUTO over 6
  expanded vertices, pixel-identical through the PVS2 (translated divide) path, red coverage within
  about 4% of the square area, and the three refusals above.
- Not done: primitive restart, instancing beyond 1, indirect draws, vertex counts past 65536
  (draw cap), real GNM fetch shaders. (Strips/fans landed in slice S10; GNM has no 8-bit index
  type, so none is emulated.)

## Slice S8 notes (translated pixel stage)
- `graphics/rdna_pixel.hpp`: a "PPS1" pixel blob (magic 0x31535050, 4 kernels - one store per
  kernel) computing a CONSTANT draw colour from pixel user data 0..7: base rgba, intensity,
  ambient, clamp bounds; colour_c = clamp(base_c * (intensity + ambient), lo, hi) in translated
  RDNA (v_add/v_mul/v_min/v_max). Status: Experimental, Tested. PNR TEST ABI: no interpolation,
  no export targets, no parameter cache.
- `execute_draw` translates and runs the blob when the pixel-stage address holds one (missing
  pixel user data 0..7 is refused); the computed colour replaces the per-vertex colours for that
  draw. Any other pixel-stage content keeps the fixed pass-through colour, and the result note
  says which path ran.
- Tests (gfx_draw_tests.cpp): the translated colour is bitwise equal to a CPU float reference
  over 3 parameter sets (including a clamp case), a PPS1 draw is byte-identical (0 of 16384
  bytes) to the same constant colour drawn per-vertex, the centre pixel is the translated colour,
  and missing user data is refused.
- Not done: varyings/interpolation from translated code, texture sampling, multiple render
  targets, per-pixel execution (real PS waves), discard/depth export, hardware GPU verification.

## Slice S9 notes (batched draws past the lane limit)
- The per-dispatch lane cap moves from 24 to 64, the translator's hard limit (verified:
  64-lane vertex kernels translate, validate and run bitwise-correct on lavapipe).
- `make_pnr_vs3_blob(bool with_post)` builds the "PVS3" vertex blob (magic 0x33535650): the
  same layout as PVS1/PVS2, but the kernels are built ONCE for the fixed 64-lane workgroup and
  carry no vertex count. `execute_draw` runs ceil(n/64) dispatches, zero-padding the tail lanes
  of the last batch (their stores land in scratch and are discarded). Kernel count 4 = clip
  only; 7 = clip + the translated post-transform (PVS2 divide semantics). PVS1/PVS2 blobs keep
  their per-count single-dispatch path (now up to 64 vertices). Status: Experimental, Tested.
- Draw caps (PNR TEST ABI sanity limits, not hardware): 65536 transformed vertices and 4M
  indices per draw; larger counts are refused with plain reasons.
- Tests (gfx_draw_tests.cpp, rdna_vertex_tests.cpp): a 96-vertex auto draw in 2 batches (a green
  triangle in the LAST batch covers the expected area, first/last-batch exclusive pixels,
  centre pixel, exact target copy); 66-vertex batched indexed draws with and without the
  translated divide are pixel-identical; a 64-lane helper transform matches the CPU float
  reference (<1e-5); PVS1 blob with 66 vertices, over-cap and non-multiple-of-3 counts refused.
- Not done: multiple workgroups in one dispatch (the translator ABI is one workgroup, so
  batching is host-side), >64-lane waves, instancing, indirect draws, real GNM fetch shaders,
  hardware GPU verification. (Strips/fans landed in slice S10.)

## Slice S10 notes (triangle strips and fans)
- `execute_draw` accepts VGT_PRIMITIVE_TYPE TriangleStrip (6) and TriangleFan (5) alongside
  TriangleList (4), for auto and indexed draws. The strip/fan source sequence expands to a
  plain triangle list on the host before rasterization: strip triangle i takes source vertices
  (i, i+1, i+2) with the first two swapped on odd i to keep the winding consistent; fan
  triangle i takes (0, i+1, i+2). The rasterizer does not cull today, so the winding swap is a
  correctness pre-step for when culling exists. Status: Experimental, Tested.
- Count rules follow the hardware: the multiple-of-3 rule applies to triangle lists only;
  strips/fans need at least 3 vertices/indices. The expanded index stream stays under the
  existing 4M-index draw cap. The result note states the expansion.
- 8-bit indices are NOT implemented on purpose: the GNM INDEX_TYPE field has only 16-bit and
  32-bit encodings, so there is nothing real to translate.
- Tests (gfx_draw_tests.cpp): a 4-vertex strip quad pixel-identical (0 of 16384 bytes) to the
  expanded triangle list, auto and indexed strips pixel-identical, a 5-vertex fan
  pixel-identical to its expanded list with the missing wedge at the clear colour, a 2-vertex
  strip and point/line primitives refused.
- Not done: primitive restart, culling/winding enforcement, line/point primitives, instancing,
  indirect draws, hardware GPU verification.

## Slice S11 notes (translated per-vertex varying: Gouraud shade)
- `rdna_shade_kernel` (graphics/rdna_vertex.hpp): a restricted RDNA kernel per colour channel
  over the full-vertex layout (x y z nx ny nz col_c, out): dot = fma(nz, l.z, fma(ny, l.y,
  nx * l.x)), out_c = col_c * clamp(dot + ambient, lo, hi). Uniform ABI: light xyz, ambient,
  clamp lo/hi (6 words). One store per kernel. Status: Experimental, Tested.
- `make_pnr_vs4_blob()` builds the "PVS4" vertex blob (magic 0x34535650): 4 clip kernels + 3
  shade kernels, all built once for the fixed 64-lane workgroup and batched like PVS3. A PVS4
  draw's vertex buffer holds 9 floats per vertex (x y z r g b nx ny nz); VS user data 4..9
  carry the light/ambient/clamp parameters (missing data is refused). The shaded rgb replaces
  the raw vertex colours and the rasterizer interpolates it - the first varying produced by
  translated code on the draw path. A PPS1 pixel blob still overrides the whole draw colour.
- Tests (gfx_draw_tests.cpp): a triangle with per-vertex normals nz = 0, 0.5, 1 shades to greys
  0, 0.5, 1 and interpolates (centre luminance 162 vs the 159 ideal, gradient endpoints near
  0.28 and 0.98); a constant-normal draw is byte-identical (0 of 16384 bytes) to a PVS1 draw
  with CPU pre-scaled colours; a 66-vertex draw batches into 2 dispatches; missing shade
  parameters refused.
- Not done: interpolated INPUTS to translated pixel code (real PS varyings per pixel), more
  than one varying channel, specular/multiple lights, normal transforms, textures, hardware
  GPU verification.

## Slice S12 notes (depth testing: DB_DEPTH_CONTROL)
- `DB_DEPTH_CONTROL` (ctx 0x200) added to graphics/amd_regs.hpp. Register address and field
  layout (bit 0 stencil enable, bit 1 depth enable, bit 2 depth write enable, bits 4..6
  compare func) from public shadPS4 src/video_core/amdgpu/{regs.cpp,regs_depth.h}
  (values only, no code). A func field of 0 means NEVER - depth enable alone is not LESS.
- `DrawBindings` (graphics/gfx_state.{hpp,cpp}) tracks the raw register when written.
- `execute_draw` (graphics/gfx_draw.cpp): when the register is set and bit 1 is on, the draw
  renders with the host depth attachment (clear 1.0, D32_SFLOAT). Stencil
  enable (bit 0) is refused with "stencil testing is not supported". Slice S26: all eight
  compare functions are supported - the AMD CompareFunc encoding 0..7 (never/less/equal/
  lequal/greater/notequal/gequal/always) matches the VkCompareOp values exactly (values
  only, public shadPS4 regs_depth.h CompareFunc), so bits 4..6 pass through raw.
  The depth surface is created and destroyed per draw: no cross-draw depth, no
  depth readback, DB_DEPTH_CLEAR untracked, depth write enable (bit 2) not separated (the
  host always tests and writes within the draw). The notes state the limits.
- Tests (gfx_draw_tests.cpp): two overlapping quads (near red z = 0.2, far green z = 0.8) in
  one draw - with depth on red wins in BOTH draw orders (byte-identical pixels, 0 differing
  bytes); the same red-first order with depth off lets the last-drawn green quad win
  (control); stencil draws refused. Slice S26 adds the compare-function matrix: a green
  reference quad at z = 0.5 (ALWAYS + write) then a red quad under each function into one
  shared frame - 14 pass/fail cases (nearer/farther/equal per function) plus a notes check.

## Slice S18 notes (face culling - PA_SU_SC_MODE_CNTL)
- The PolygonControl register (bit layout from public shadPS4 regs_primitive.h - values
  only, no code) is decoded at draw time: bit 0 cull_front, bit 1 cull_back, bit 2
  front_face (0=CCW, 1=CW). The cull bits map onto VK_CULL_MODE_* (front+back is
  supported and culls everything); the face bit maps onto VK_FRONT_FACE_*.
- Facing is faithful to GNM even though this pipeline hands the rasterizer NDC and the
  host's fixed-function viewport maps it (the guest viewport values are validated but
  not applied - a documented limit of the draw path): GNM determines facing in its
  y-flipped window space and Vulkan's facing convention negates the framebuffer-area
  formula, so the two sign flips cancel. The tests pin both windings against both cull
  bits and both face values.
- Refused with named reasons (never silently approximated): point/line polygon modes
  (enable_polygon_mode with a non-fill front or back mode), polygon offset enables,
  window offset, perspective-correct-interpolation disable, multi-primitive index
  buffers. The provoking-vertex bit is ignored - flat varyings do not exist in this
  subset, so it cannot change the output - and the draw notes say so.
- Strips and fans expand to lists on the host (S10) with winding corrected, so a
  cull-back draw keeps every expanded triangle; the tests render a strip quad and a
  three-wedge fan under cull-back and check each wedge.
- MeshInput carries cull_mode/front_face (VK values, NONE/CCW defaults) so the older
  render entry points are unchanged; the draw notes report the active cull state.

## Slice S17 notes (texture sampling - T#/S#)
- A "PPS2" pixel blob ({magic, 0}) marks the textured pixel stage. Pixel user data 0..1
  addresses the T# (8 dwords), 2..3 the S# (4 dwords); both descriptors live in guest
  memory and are decoded and validated on every draw (gfx_tsharp.hpp; layouts from public
  shadPS4 resource.h / pixel_format.h / tiling.h - values only, no code).
- Accepted subset, everything else refused with a named reason: Color2D images,
  8_8_8_8 unorm, linear tiling (DisplayLinearAligned / DisplayLinearGeneral), one mip
  level, no arrays, identity component select, extent 1..256 (this slice's staging cap);
  wrap/mirror/clamp/mirror-once addressing, point or bilinear filters, no depth compare,
  no min/max filter mode, no mip filtering.
- The draw repacks the guest pixels (T# pitch honoured) into a tightly packed RGBA8
  image, builds a Vulkan sampler from the S# state (GNM ClampMode/Filter values mapped
  to VK_SAMPLER_ADDRESS_MODE_* / VK_FILTER_*), and renders with the clip-space textured
  pipeline (cliptex_vert + tex_frag, 9-float records: clip4 + colour3 + uv2). The uv
  arrives as a plain second varying, like the fixed colour pass-through.
- Sampling itself runs in the host rasterizer's fragment stage - it is NOT translated
  RDNA in this slice (unlike the S8 PPS1 constant-colour kernels); the draw notes say so.
- PPS2 combines with PVS1..PVS3 vertex blobs only; PVS4 (normals) and PVS5 (V# fetch)
  combinations are refused as not implemented.
- Tests (13 in gfx_draw_tests + 15 descriptor tests in gfx_tsharp_tests): 4x4 checker
  nearest draw byte-exact in 16-pixel blocks (either row orientation), target memory
  exact; wrap addressing repeats the pattern every 32 pixels with 8-pixel texel blocks;
  bilinear midpoint is the four-texel average (127/128) and a pixel landing on a texel
  centre samples that texel exactly; refusals: missing T#/S# pointers, tiled image,
  border clamp, PPS2+PVS4.

## Slice S19 notes (mipmapped textures)
- T# mip chains: base_level must be 0 and last_level may reach the extent's level count
  (each level's width/height shrinks by half, floored at 1). The accepted guest layout
  is level 0 at the T# pitch, then every smaller level tightly pitched (pitch = level
  width) one after another; validate_tsharp_sample bounds the whole chain inside the
  guest address space. Chains starting above level 0 and levels beyond the extent are
  refused with named reasons.
- S# gains min_lod / max_lod (12-bit 4.8 fixed), mip_point_preclamp and lod_bias
  fields (layout from public shadPS4 resource.h, values only). mip_filter none / point
  / linear map to the host sampler's mipmap mode (none and point both NEAREST, with
  none clamping maxLod to level 0); mip filter value 3, preclamp and any lod bias are
  refused. T# and S# min_lod combine by max (the tighter clamp wins); S# max_lod 0
  means no clamp, otherwise it clamps and is itself capped at last_level. max_lod
  below min_lod is refused. mip filtering on a single-level texture is refused.
- The host side (render.cpp) creates the image with mipLevels = chain length, uploads
  every level in one staging pass (one VkBufferImageCopy per level, tight packing), and
  sets the sampler's mipmapMode / minLod / maxLod from the mapped state. TexInput
  carries the chain as packed levels with the same tight layout.
- Sampling still runs in the host rasterizer's fragment stage (not translated RDNA),
  same honesty note as S17.
- Tests (10 in gfx_draw_tests + 10 new descriptor checks in gfx_tsharp_tests): solid
  colour levels L0 red / L1 green / L2 blue - point picks levels 1 and 2 exactly at
  footprints 2 and 4, magnification keeps level 0, linear is exact at an integer lod
  and blends levels 1 and 2 inside a tolerance band at lod ~1.585, S# max_lod / min_lod
  and T# min_lod clamp the sampled level, mip filter none keeps level 0 on a chain,
  and the refusal cases above all name their reason.

## Slice S20 notes (compressed textures - BC1/BC3)
- T# data formats 35 (BC1) and 37 (BC3) with unorm numerics join the sampled subset
  (values from public shadPS4 pixel_format.h, values only). The block bit layout is the
  public S3TC/DXT specification. The subset requires linear tiling, a tight pitch
  (pitch == width) and extents that are multiples of 4; everything else is refused
  with a named reason. Mip chains combine with S19: level l covers ceil(w/4) x
  ceil(h/4) 4x4 blocks, packed like S19's levels.
- gfx_bc.hpp decodes blocks to RGBA8 with exact integer arithmetic: BC1's two
  RGB565 endpoints expand by bit replication, the 4-colour ramp interpolates the
  expanded channels ((2c0+c1)/3, (c0+2c1)/3), the 3-colour mode adds transparent
  black; BC3's alpha block interpolates 6 or 4 values between the endpoints (plus
  0 and 255 in 6-alpha mode) and its colour half always uses the 4-colour ramp.
- The draw repack (gfx_draw.cpp) reads the guest's block rows and decodes each level
  into the S19 tight RGBA8 chain, so mips, filters and addressing apply unchanged.
- The hand-authored tex_frag shader now passes the sampled alpha through
  (previously forced to 1), so BC3 alpha and BC1 punch-through reach the target;
  tint has no alpha channel.
- Tests (14 in gfx_bc_tests + 8 in gfx_draw_tests): every decode rule byte-exact
  against hand-computed blocks (both BC1 modes, both BC3 alpha modes incl. nonzero
  endpoints, BC3's 4-colour colour half, edge-block dropping), then end-to-end:
  BC1 solid and checker draws byte-exact, BC3 alpha gradient byte-exact in the
  target, BC1/BC3 mip chains select the right level, loose pitch and non-multiple-
  of-4 extents refused.

## Slice S21 notes (BC2/BC4/BC5)
- T# data formats 36 (BC2), 38 (BC4) and 39 (BC5) join the subset under the same
  rules as S20 (linear tiling, tight pitch, multiple-of-4 extents, S19 mip chains).
- BC2: explicit 4-bit alphas scaled by 17, colour half always on the 4-colour ramp.
  BC4: one channel (the BC3 alpha block layout) sampled as (r, 0, 0, 1). BC5: two
  BC4 blocks sampled as (r, g, 0, 1). All exact integer arithmetic.
- Tests (5 in gfx_bc_tests + 3 in gfx_draw_tests): nibble scaling, both BC4 modes
  with nonzero endpoints, independent BC5 channels, and end-to-end draws byte-exact
  (BC2 alpha ramp in the target, BC4 red-only, BC5 two-channel).

## Slice S22 notes (PS5 thin texture tiling)
- The T# tiling index switches to the PS5-native SW_MODE vocabulary, resolved like
  AnyPS5's resolveTileMode: 0 linear; 1/2 = 256B S/D; 5/6 = 4KB S/D; 9/10 = 64KB
  S/D; 17/18 = 64KB S/D "T"; 21/22 = 4KB S/D XOR; 24..27 = 64KB Z/S/D/R XOR. The
  GCN (PS4) index encodings 8/31 that S17 used for linear are refused with a named
  reason, as are unknown modes; pitch rules stay linear-only.
- graphics/gfx_swizzle.hpp is a port of AnyPS5's thin-tiling path (GPL, owner grant,
  attribution header): the 80-entry swizzle equation table (AMD addrlib GFX10
  pattern data), tile-block geometry per mode and element size, the mip-chain layout
  with tail packing (tail block first, then levels smallest to largest through the
  tail tables), element offsets, and a CPU detiler. XOR modes take a pipe/bank XOR
  from the guest base's low bits (4KB modes: base & 0xFFF; 64KB modes: base & 0xFFFF)
  and the base is normalized down before the read.
- The draw repack (gfx_draw.cpp) reads the whole guest surface once, detiles each
  level into a linear staging buffer at the hardware layout pitch, then runs the
  common repack: RGBA8 rows copy at the level pitch, BC levels decode through the
  S20/S21 path. Sampler filters, mips and addressing apply unchanged.
- Tests (30 in gfx_swizzle_tests + 6 new/changed in gfx_draw_tests + tiling value
  updates in gfx_tsharp_tests): mode vocabulary, block extents, all nine AnyPS5
  addrlib-validated known-answer vector sets (256B D, 4KB D, 64KB S/D/T, 4KB S/D
  XOR, 64KB Z XOR 8/16-bit) byte-exact at z=0, a byte-exact 64KB D chain detile,
  pipe/bank XOR rotation; end-to-end SW_64KB_S and SW_64KB_S_X checkerboards and a
  SW_64KB_D BC1 solid draw byte-exact, tail-packed chains pick levels 1 and 2
  exactly, GCN 8/31 and unknown modes refused with named reasons.
- Limits: thin (2D, single-slice) modes only - no thick/3D modes, arrays or depth
  sampling; sampling is still not translated RDNA; hardware GPU untested.

## Slice S31 notes (array/cube descriptor semantics: allocation vs view)
- **Corrects S27/S28.** The T# DEPTH field (q2 bits 0-12) is not just for
  3D: for Color2DArray it holds the ALLOCATION layer count - 1, and
  base_array..last_array select a view window clamped to that allocation:
  view = min(last_array + 1, depth + 1) - base_array. For Cube it counts
  full cubes (one cube = depth field 0 = 6 layers). Sources, independent of
  each other: shadPS4's resource.h (NumLayers: "Depth is the number of
  layers for Array images", "Depth is the number of full cubes for Cube
  images", and the clamp in NumArraySlices) and Mesa's ac_descriptors.c
  (AMD's own driver stack: the GCN-era builder sets DEPTH = layers-1 for
  arrays, and its gfx9+ comment documents DEPTH as "the last accessible
  layer"). The GCN-era layout is corroboration, not proof, of the console
  layout: no PlayStation verification exists, so the implemented subset is
  grounded in shadPS4's console model with Mesa as the independent second
  reading, and residual PS5-versus-GCN uncertainty is retained here rather
  than generalized away.
- PNR implements: arrays with base_array 0 and any view window (clamped),
  cubes with exactly one cube. Still refused with named reasons: non-zero
  base_array (mid-allocation views), cube arrays, tiled arrays/cubes.
- **Allocation bounds and the sampled view are kept distinct, per review:**
  validate_tsharp_sample bounds the full ALLOCATION (depth layers per mip
  level for arrays, depth*6 faces for cubes); the repack copies only the
  view window's slices out of each level and strides the guest address by
  the full allocation width per level. The byte-exact tests prove the
  distinction: a 4-slice view inside an 8-slice allocation draws the right
  slice colours, and mip level 1 is read after all 8 level-0 slices, not 4.
- Earlier descriptors PNR accepted remain accepted and behave identically
  when they follow the typical GNM pattern (depth field = last_array,
  base_array = 0): all S27/S28 tests pass unchanged in meaning. What
  changes: a mismatched descriptor is now read the way the console model
  reads it instead of silently treating last_array as the slice count.
- File layout: gfx_draw_tests.cpp outgrew the practical size limit of the
  editing tooling, so the texture draw tests (S17/S19/S20-S25/S27/S28 and
  the S18 textured-cull checks) moved to tests/gfx_tex_draw_tests.cpp
  (suite pnr_gfx_tex_draw_tests). No test was dropped; counts grew by the
  new S31 checks only.

## Slice S30 notes (3D / volume textures)
- **Implemented, Tested (software-output verified; texture-3d sweep path
  also validation-layer clean).** Color3D T# (type 10): the q2 bits 0-12
  field is the volume depth-1 (values from shadPS4's resource.h; usage only,
  no code taken). A 3D texture has no array slices and cannot be a cubemap;
  linear tiling only, uncompressed only, depth <= 256; anything else is
  refused with a named reason. Mip chains shrink width, height AND depth per
  level (max(1, d>>mip), same rule shadPS4's tile.h applies), packed
  level-major with each level's z-planes together.
- Host side: VK_IMAGE_TYPE_3D image + VK_IMAGE_VIEW_TYPE_3D view (enum
  values 2 and 2, audited by the S28 compile-time enum check), extent depth
  from the T#, per-level copy regions with the level's own depth.
- New PPS2 v3 test blob (magic, 3) marks a draw as 3D-sampling: vertex
  records grow to 9 floats (xyz, rgb, uv, w); a v0/v1/v2 blob with a 3D T#
  or a v3 blob with a 2D/array/cube T# is refused as a type mismatch. The
  fragment shader samples `texture3D(uvw)` (hand-written SPIR-V,
  spirv-val clean) and multiplies by the varying colour; sampling itself is
  still the host rasterizer's, not translated RDNA.
- Byte-exact tests: 8x8x4 volume with red/green/blue/white z-planes, point
  sampling at each plane centre draws that plane's colour exactly; mip level
  1 of a depth-4 volume is 4x4x2 and samples correctly through T# min_lod.
  Refusals covered: blob/T# type mismatch both directions, tiled 3D, depth
  beyond the cap, array fields set on a 3D T#, BC-compressed 3D.
- The validation sweep (S29) gained a texture-3d path: 7 paths, zero layer
  errors under VK_LAYER_KHRONOS_validation on lavapipe.
- Open grounding question: shadPS4's NumLayers() comment implies the same
  q2 depth field doubles as layers-1 for arrays; PNR's S27/S28 arrays use
  base/last_array instead and their behaviour is unchanged. Unresolved
  against real GNM docs; flagged, not blocking (3D and array types are
  disjoint in PNR's validator).

## Slice S29 notes (Vulkan validation-layer coverage)
Evidence levels are now stated separately, per policy: **software-output
verified** = byte-exact renders on lavapipe (tolerates wrong-but-plausible
configuration); **validation-layer clean** = the same paths also ran under
VK_LAYER_KHRONOS_validation with zero layer errors. Only the paths listed
below hold the second level; everything else in PNR (swapchain/present,
depth/stencil, blending, multi-frame surfaces, non-default sampler modes,
the full unit suite's negative tests) remains software-output verified only.

- `tests/validation_sweep.cpp` (`pnr_validation_sweep`): one instance/device
  with the layer explicitly requested, then each shipped texture/draw path
  once: untextured PVS1 `execute_draw` (base pipeline + translated-RDNA
  compute vertex stage + colour-target copyback), plain 2D texture,
  mip-chain 2D texture, sRGB 2D texture, 2D array (2D_ARRAY view), cubemap
  (CUBE_COMPATIBLE image + CUBE view). It SKIPs honestly when the layer or a
  Vulkan device is absent: the loader only warns about a missing env-forced
  layer, so the sweep checks vkEnumerateInstanceLayerProperties itself
  before claiming coverage.
- First real run caught a live bug the rasterizer never cared about:
  render.cpp's colour-target allocation never set
  VkMemoryAllocateInfo.sType (VUID-VkMemoryAllocateInfo-sType-sType). Fixed;
  the sweep now runs with zero layer errors.
- ctest `pnr_vulkan_validation` runs the sweep and fails on any "Validation
  Error" in its output (23/23 suites in the default build, 25/25 with the
  window build). CI gained a `linux-validation` job that installs
  vulkan-validationlayers and treats a SKIP as a failure, so the layer run
  is guaranteed real there.
- Local run without system packages: extract the jammy
  vulkan-validationlayers deb anywhere and point the loader at it:
  `VK_LAYER_PATH=<dir>/usr/share/vulkan/explicit_layer.d
  LD_LIBRARY_PATH=<dir>/usr/lib/x86_64-linux-gnu ./pnr_validation_sweep`.
- Compile-time companion: tests/vulkan_enum_check_tests.cpp (S28) audits
  constant VALUES; this slice audits API CALLS at runtime. Neither covers a
  real hardware GPU.

## Slice S28 notes (cubemaps)
- Cube T# images (type 11, value from public shadPS4 resource.h ImageType)
  are accepted: a single cubemap = exactly 6 faces, square extent, linear
  tiling, base face 0; tiled cubes, cube arrays, non-square extents and a
  non-zero base face are refused with named reasons. The bounds check covers
  all 6 faces of every mip level.
- Faces are laid out exactly like S27 array slices (level-major, 6 per
  level; order +X -X +Y -Y +Z -Z, the Vulkan layer order), so the repack
  and upload path are the S27 one with nslices = 6.
- A PPS2 blob version word of 2 ({magic, 2}) marks a cubemap draw: the T#
  must be type 11 and the 9-float vertex record carries the sampling
  direction (xyz rgb dir3). Blob/T# type mismatches are refused.
- Host side: TexInput.cube, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT on the
  image, a CUBE view; new texcube.frag (samplerCube) with the S27
  cliptex3.vert. Also fixes the S27 view-type enum: VK_IMAGE_VIEW_TYPE_2D_ARRAY
  is 5 and CUBE is 3 (S27 shipped 2 and 8, which lavapipe tolerated; the
  S27 array tests still pass byte-exact with the corrected values).
- Tests (gfx_tsharp_tests + gfx_draw_tests): validation (6-face accept,
  bounds across faces, face-count/non-square/tiled/base-face refusals);
  end to end byte-exact draws - all six faces by direction, mip level 1 of
  a cube, three blob/type mismatch refusals and a tiled-cube refusal.
- Limits: single cube only (no cube arrays), linear tiling, square faces,
  no cube RDNA sampling translation (host rasterizer), hardware GPU untested.

### Vulkan enum audit (S28 follow-up)
The S27 view-type bug proved that byte-exact lavapipe images do not prove
API-valid configuration: the software rasterizer accepts wrong-but-plausible
enum values and still renders. So every VK_* constant the shim re-declares
(134 names) was compared by script against the Khronos Vulkan-Headers
vulkan_core.h (VK_HEADER_VERSION 204, Vulkan 1.3.204, Apache-2.0, values
only). The audit found two more wrong values beyond the S27 pair:
VK_PRESENT_MODE_FIFO_KHR was 1 (MAILBOX; real FIFO = 2) and
VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT was 8 (MUTABLE_FORMAT; real = 16). All
four shipped values were silently tolerated by lavapipe; the first three are
invalid usage a validation layer would flag, and a cube view without
CUBE_COMPATIBLE is undefined on strict drivers. All four are fixed.
Regression protection: tests/vulkan_enum_ref.h (generated, verbatim Khronos
values, PNR-local names) plus 133 static_asserts in
tests/vulkan_enum_check_tests.cpp - the build fails the moment a shim
constant drifts. Regenerate after adding shim constants:

    python3 - <<'EOF'
    import re
    ref = {}
    for line in open('<vulkan_core.h path>'):
        m = re.match(r'\s+(VK_[A-Z0-9_]+)\s*=\s*([0-9xXa-fA-F]+)[UuLl]*\s*,', line)
        if m: ref[m.group(1)] = int(m.group(2), 0)
    # then rewrite tests/vulkan_enum_ref.h and the static_assert block in
    # tests/vulkan_enum_check_tests.cpp from the shim's VK_* names (values
    # must match ref; VK_COLOR_COMPONENT_RGBA is shim-invented, skip it)
    EOF

Remaining gap (tracked, not yet closed): no Vulkan validation layer runs in
tests today - none is installed locally or in CI (ci.yml installs only
libvulkan1 + mesa-vulkan-drivers), so tolerated-invalid configuration still
isn't caught at runtime. A follow-up slice should add a CI job that installs
vulkan-validationlayers and runs the suite with
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation, treating layer errors as
failures, before more texture features land on the shim.

## Slice S27 notes (2D texture arrays)
- Color2DArray T# images (type 13, value from public shadPS4 resource.h
  ImageType) are accepted: linear tiling only, base_array 0, up to 256
  slices; tiled arrays, a non-zero base slice and arrays on a Color2D
  descriptor are refused with named reasons. The bounds check covers every
  slice of every level.
- Guest layout is level-major (each mip level holds all slices contiguously,
  levels follow in order; level 0 rows at the T# pitch, deeper levels tight),
  matching shadPS4 ComputeImageSize (values only). The repack decodes slice
  by slice into a tightly packed RGBA8 staging image with the same layout.
- A PPS2 blob version word of 1 ({magic, 1}) marks an array draw: the T#
  must be type 13 and the vertex record carries a slice index after the uv
  pair (9 floats: xyz rgb uv layer). Blob/T# type mismatches are refused.
- Host side: TexInput.layers, a 2D_ARRAY image view (arrayLayers = slices,
  one upload region per level covering all layers), and a new entry
  render_textured_array_clip_mesh_rgba with 10-float records and a vec3
  uv+layer attribute; new demo shaders cliptex3.vert + texarr.frag
  (sampler2DArray). Sampling itself is still host-rasterizer, not translated
  RDNA; the draw notes say so.
- Tests (gfx_tsharp_tests + gfx_draw_tests): decode/validation (array accept,
  tiled/base_array/>256-slice/Color2D-with-slices refusals, bounds across
  slices); end to end byte-exact draws - plain and BC1 two-slice arrays pick
  slice 0/1 exactly, mip level 1 of a 2-slice array exact, v0-blob+array and
  v1-blob+2D mismatches refused, tiled array refused.
- Limits: linear tiling only (tiled arrays refused), no cube/3D/MSAA, slice
  index is per-vertex (no layer clamp/emulation beyond the host sampler),
  hardware GPU untested.

## Slice S26 notes (depth compare functions)
- All eight DB_DEPTH_CONTROL compare functions (bits 4..6) supported: never/less/equal/
  lequal/greater/notequal/gequal/always. The AMD CompareFunc encoding 0..7 is identical to
  the VkCompareOp values (values only, public shadPS4 regs_depth.h CompareFunc; the sampler
  DepthCompare enum in shadPS4 resource.h agrees), so the field passes to the host pipeline
  raw. Replaces the S12 "only LESS" refusal; stencil stays refused.
- Plumbing: MeshInput.depth_compare (VK_COMPARE_OP_LESS default keeps pre-S26 callers
  unchanged) through render_clip_mesh_rgba / render_textured_clip_mesh_rgba into
  VkPipelineDepthStencilStateCreateInfo.depthCompareOp. Draw notes name the function.
- Tests (gfx_draw_tests.cpp): 14 shared-frame cases - a green z = 0.5 reference (ALWAYS +
  write) then a red test quad per function at nearer/farther/equal depth; centre pixel is
  red iff the function passes. Plus a notes check. Stencil refusal kept.
- Limits: unchanged from S12/S13 - no depth readback, DB_DEPTH_CLEAR untracked, per-draw
  surface without a DB binding, D32 float only, stencil refused, hardware GPU untested.

## Slice S25 notes (BC6H decode)
- BC6H (T# data format 40, value from public shadPS4 pixel_format.h) completes
  the compressed-texture subset: all 14 modes, unsigned (number format Uint,
  4) and signed (Sint, 5); sRGB and other numerics are refused with named
  reasons (graphics/gfx_tsharp.hpp).
- The decoder (graphics/gfx_bc6h.hpp) is table-driven: the per-mode bit-field
  layout, mode info and mode-to-info map (graphics/gfx_bc6h_tables.inc) were
  transcribed by script from microsoft/DirectXTex DirectXTex/BC6HBC7.cpp (MIT)
  and never retyped. Transform-inverse, unquantize, interpolation and the
  31/64 / 31/32 finish follow the public BC6H specification; the 2-subset
  partition/fixup and weight tables are shared with BC7.
- Before any test was written, the decoder was differential-fuzzed against
  bcdec's bc6h_half on 2,000,000 random blocks per sign with zero mismatches
  (the same standard as S24).
- Output lands in the RGBA8 staging chain: decoded half floats convert to
  float and clamp to [0, 1] - HDR values above 1.0 clamp, which matches what
  an 8-bit unorm frame would do to any sampled HDR value (slice S23 note).
  Alpha is always 255; BC6H has no alpha channel.
- Tests (7 in gfx_bc6h_tests, 4 in gfx_tsharp_tests, 3 in gfx_draw_tests):
  reserved mode (black, full alpha), hand-computed mode 1/11/14 structural
  cases (unquantize extremes, delta-transform wrap, signed negative clamp,
  partition mapping, full-precision 16-bit endpoints), 32 captured
  known-answer vectors covering every mode in both signs (seed 0xBC6DEC), the
  tsharp accept/refuse matrix, and end-to-end draws - mode-11 solid red
  byte-exact, a mip chain selecting level 1, and a signed draw clamping
  negative endpoints to black.
- Limits: HDR above 1.0 clamps in the 8-bit frame; sampling is still not
  translated RDNA; hardware GPU untested.

## Slice S24 notes (BC7 decode)
- BC7 (T# data format 41, value from public shadPS4 pixel_format.h) joins the
  S20-S21 compressed-texture subset: all 8 modes decoded to RGBA8 with exact
  integer interpolation (graphics/gfx_bc7.hpp). 1-3 subsets, rotation, the
  mode-4 index selector, shared and per-endpoint P-bits, and anchor-bit
  removal are all implemented; a reserved block (no mode bit set) decodes to
  zeros, matching the public specification.
- The partition, fixup and weight tables (graphics/gfx_bc7_tables.inc) were
  transcribed by script from public MIT-licensed sources (microsoft/DirectXTex
  BC6HBC7.cpp, iOrange/bcdec) - never retyped - and the decoder was
  differential-fuzzed against bcdec on 2,000,000 random blocks with zero
  mismatches before any test was written (this caught a table-index bug
  immediately).
- BC7 flows through the common repack: T# validation accepts df 41 with unorm
  or sRGB numerics (graphics/gfx_tsharp.hpp), the draw path decodes it like
  BC1-BC5 (graphics/gfx_draw.cpp), and sRGB BC7 converts in the host sampler
  through the S23 SRGB staging image (graphics/gfx_formats.hpp).
- Tests (21 in gfx_bc7_tests, 4 in gfx_tsharp_tests, 3 in gfx_draw_tests):
  the reserved block, hand-computed mode 0/4/6 structural cases (endpoint and
  P-bit expansion, partition lookup, weight interpolation, rotation, index
  selector), 16 captured known-answer vectors (one per mode, forced and
  random, seed 0xBC7EC7), the tsharp accept/refuse matrix, and end-to-end
  draws - a mode-6 solid byte-exact, a mip chain selecting level 1, and an
  sRGB solid byte-exact through lavapipe's hardware conversion (sRGB bytes
  201/101/51 land on 149/33/8).
- Limits: 16-byte blocks require the usual tight pitch
  and multiple-of-4 extents; sampling is still not translated RDNA; hardware
  GPU untested.

## Slice S23 notes (extended plain texture formats + sRGB)
- The sampled T# subset grows past 8_8_8_8 unorm (graphics/gfx_formats.hpp;
  data/number format values from public shadPS4 pixel_format.h, values only):
  df 1 (8), 2 (16), 3 (8_8), 4 (32), 5 (16_16), 11 (32_32), 12 (16_16_16_16),
  14 (32_32_32_32) and 16 (5_6_5). Unorm covers every channel width; IEEE
  float (half and single) covers the 16/32-bit formats; 8_8_8_8 and BC1-BC3
  gain sRGB numerics. Snorm/scaled/integer numerics, sRGB on other formats
  and packed formats like 2_10_10_10 stay refused with named reasons.
- Conversion to the RGBA8 staging chain is exact: 16-bit unorm rounds by
  (v*255 + 32767)/65535 in integers, half decodes bit-exactly (subnormals,
  infinities, NaNs - NaN clamps to 0), float32 clamps and rounds to nearest,
  5_6_5 reuses the BC endpoint bit replication. Missing channels pad to
  (0,0,1), the BC4/BC5 convention. Formats are not silently approximated:
  the staged bytes equal the mathematically correct 8-bit result, and the
  unorm staging round-trips them exactly.
- sRGB is never converted on the CPU: TexInput gains an srgb flag and the
  staging image/view become VK_FORMAT_R8G8B8A8_SRGB, so the host sampler
  applies the specification's piecewise conversion. BC1-BC3 sRGB decode to
  sRGB-encoded RGBA8 and take the same path.
- Plain formats flow through the S22 tiled path unchanged: the element size
  parameterizes the swizzle block geometry (a tiled R16G16 checker detiles
  and draws byte-exact).
- Tests (38 in gfx_format_tests + 21 in gfx_tsharp_tests + 8 in
  gfx_draw_tests): the full accept/refuse matrix, scalar conversions against
  hand-computed values (unorm16 midpoints, half bit patterns, float clamps),
  row conversions with channel padding, and end-to-end draws - R8G8 checker,
  R16G16B16A16 unorm, half float, float32, 5_6_5 solids, a tiled R16G16
  checker, and sRGB 8_8_8_8 / BC1 solids where lavapipe's hardware sRGB
  conversion lands exactly on the specification values (10, 128, 255 for
  guest bytes 55, 188, 255).
- Limits: integer/sampled-as-float targets still land in an 8-bit unorm
  frame; snorm would clamp there and is refused instead; no BC6H, arrays,
  3D or cubes; sampling is still not translated RDNA; hardware GPU untested.

## Slice S16 notes (8_8_8_8 unorm vertex colours)
- The S15 fetch kernels gain a conversion path for data format 10 (8_8_8_8) with unorm
  numerics: all four components share the record's first dword, so the component dword
  offset is 0 and the byte is extracted after the load - v_and with the byte mask, cvt to
  float, fmul by 1/(255*256^c). Bytes 0..2 stay below 2^24, so the subset's signed
  cvt_i32->f32 is exact; the two roundings (scale constant, multiply) stay ~2 ulp, far
  below the 0.5/255 UNORM8 quantization step, so the rendered bytes are exact.
- Validation (gfx_vsharp.hpp): 8_8_8_8 is fetchable only for colour attributes with unorm
  numerics; 8_8_8_8 positions, snorm/integer byte formats and other packed formats are
  refused with plain reasons. The guest read for the last vertex keeps to one dword for
  8_8_8_8 (three components in one dword), so the bounds check stays exact.
- The alpha byte is not fetched: byte 3's masked value can exceed 2^31 and needs an
  unsigned cvt (v_cvt_f32_u32), which the subset does not have yet - Planned with the
  wider format work. Fragment alpha stays 1.0 (slice S14 note).
- Tests (gfx_draw_tests.cpp): a constant-colour quad as packed records (xyz floats + one
  colour dword, stride 16) renders byte-identical to the float reference with colours
  byte/255, and the centre is byte-exact for a mixed colour (255,128,64); 2_10_10_10 and
  8_8_8_8-with-float-numerics bindings are refused.

## Slice S15 notes (vertex fetch: V# buffer descriptors)
- V# decode (graphics/gfx_vsharp.hpp): the 128-bit buffer resource (base 44 bits, stride 14,
  num_records; dst_sel x..w, num_format, data_format, element_size, index_stride,
  add_tid_enable, type) from public shadPS4 src/video_core/amdgpu/resource.h (Buffer); the
  DataFormat/NumberFormat values (Format32=4, Format32_32=11, Format32_32_32=13,
  Format32_32_32_32=14; Float=7) from its pixel_format.h. Values only, no code.
- Fetch kernels (graphics/rdna_fetch.hpp): restricted RDNA code, one component per dispatch -
  lane*stride addressing (doublings + adds, the subset has no integer multiply), a flat dword
  load from the raw record region and a store into a gathered float region. Like GNM fetch
  shaders, the code is specialized per vertex layout (component and stride baked in).
- "PVS5" blob (make_pnr_vs5_blob): {magic, 13, pos_fmt, col_fmt, pos_stride_words,
  col_stride_words, 13 kernel lengths}; kernels 0..5 fetch (pos xyz, colour rgb), 6..12 are
  the PVS3 batched clip + post-NDC kernels. Vertex user data 2..3 addresses a two-entry V#
  table (position, colour) in guest memory; the matrix stays at slots 0..1.
- Draw path (gfx_draw.cpp): both descriptors are decoded and validated against the blob's
  compiled layout (stride, data format, num_records >= vertex count, identity dst_sel, no
  add_tid/swizzle/tiling, float number format). Only the consumed components are read from
  guest memory, so a strided colour V# inside a record never runs past the buffer end. The
  fetched floats feed the existing clip path unchanged; indexed draws fetch max index + 1
  vertices as before.
- Limits (stated in the draw notes): 32-bit float formats only - unorm/snorm/integer vertex
  data is Planned (needs v_lshr + v_cvt_f32_u32 in the translator subset); a Format32_32_32_32
  position's w is fetched but ignored (w_in = 1.0); component swizzles beyond identity are
  refused; no fetch of texture/other attribute semantics yet.
- Tests (gfx_draw_tests.cpp): interleaved/strided, packed (stride 0) and Format32_32_32_32
  vertex data all render byte-identical to the raw-pointer reference draw; a 66-vertex draw
  crosses two 64-lane fetch dispatches; refusals: add_tid, swizzled buffers, unorm number
  format, packed 8-bit formats, non-identity dst_sel, num_records short of the draw, and a V#
  layout that does not match the blob.

## Slice S14 notes (target-0 blending: CB_BLEND0_CONTROL)
- Register (graphics/amd_regs.hpp): CB_BLEND0_CONTROL ctx 0x1E0. Field layout (colour src
  factor bits 0..4, colour function bits 5..7, colour dst factor bits 8..12, alpha src factor
  bits 16..20, alpha function bits 21..23, alpha dst factor bits 24..28, separate-alpha bit 29,
  enable bit 30) from public shadPS4 src/video_core/amdgpu/{regs.cpp,regs_color.h} (values
  only, no code).
- Factor translation: GNM orders factors 4..9 alpha-first (4 src-alpha, 5 inv-src-alpha,
  6 dst-alpha, 7 inv-dst-alpha, 8 dst-colour, 9 inv-dst-colour); Vulkan orders the same rows
  colour-first (4 dst-colour, 5 inv-dst-colour, 6 src-alpha .. 9 inv-dst-alpha). gfx_draw
  translates values 4..9 through a ten-entry table; 0..3 (zero/one/src-colour/inv-src-colour)
  coincide. All five GNM blend functions map onto Vulkan blend ops (add, subtract, min, max,
  reverse-subtract).
- `BlendState` (graphics/vulkan.hpp) + MeshInput.blend + the colour-blend attachment state in
  render.cpp: blending is per-draw pipeline state, like slice-S12 depth. Without the register
  (or its enable bit) the draw replaces, exactly as before.
- Separate-alpha bit 29 is decoded: when set, the alpha factor/function fields drive the alpha
  channel; otherwise the colour fields apply to alpha. No pixel-level test yet (the current
  pipeline has fragment alpha 1.0, so only SRC/DST_ALPHA = pass/zero would be observable).
- Refused with plain reasons: src-alpha-saturate (10), constant colour/alpha (13, 14, 19, 20)
  and dual-source src1 factors (15..18) - no blend constants or dual-source outputs yet.
- Limits (stated in the draw notes): target 0 only (CB_BLEND1..7_CONTROL untracked); fragment
  alpha is 1.0 on the current pipeline (no vertex alpha, no pixel-shader alpha export); blend
  constants and logic ops not implemented.
- Tests (gfx_draw_tests.cpp): additive ONE+ONE over green gives yellow; MIN gives black;
  src*DST_COLOR multiplies the source by the target content (red over green -> black);
  SRC_ALPHA at fragment alpha 1.0 passes the source through (pins the alpha/colour row swap of
  the factor table); without the register the second draw replaces (control); constant-colour
  and dual-source factors refused.

## Slice S13 notes (frame-persistent render targets: bound DB depth buffer)
- Depth buffer binding registers (graphics/amd_regs.hpp): DB_Z_INFO ctx 0x010, DB_STENCIL_INFO
  0x011, DB_Z_READ_BASE 0x012, DB_Z_WRITE_BASE 0x014, DB_DEPTH_SIZE 0x016. Addresses and field
  layout (ZInfo format bits 0..1: 1 = Z16, 3 = Z32 float; tile surface enable bit 28; DepthSize
  pitch/height tile maxima, pitch = (max+1)<<3; bases << 8) from public shadPS4
  src/video_core/amdgpu/{regs.cpp,regs_depth.h} (values only, no code).
- `FrameSurface` (graphics/vulkan.hpp) + `VulkanBackend::create_frame_targets`/`destroy_frame`
  (graphics/render.cpp): a colour + D32 depth attachment pair kept across draws. First use (or a
  size change) creates and clears both (LOAD_OP_CLEAR); later draws reload them (LOAD_OP_LOAD),
  so consecutive draws accumulate into the bound colour target and test against earlier depth.
  Single-frame renders only.
- `DrawFrameCache` (graphics/gfx_draw.{hpp,cpp}): caller-owned frame state (colour base, z base,
  size, surface). `execute_draw(vk, mem, draw, frame)` renders into the cache; the 3-argument
  overload keeps an empty cache, so single draws behave exactly as before (slice S12 per-draw
  depth). The frame restarts (fresh clear) when the colour base, z base or size changes.
- Binding rules: with depth enable and a valid linear Z32 float DB binding, the draw uses
  the persistent depth surface; DB_DEPTH_CONTROL depth_write_enable (bit 2) is honoured (read-only
  depth tests without it). Without a valid binding the draw keeps the slice-S12 per-draw depth
  surface. Refused with plain reasons: Z16 and the reserved format, tiled depth (HTILE, bit 28),
  a stencil format, separate read/write bases, and a shared frame that gains depth after its
  first draw (bind everything up front, like a real game).
- Limits (stated in the draw notes): no depth readback; DB render-control clears and
  DB_DEPTH_CLEAR are not tracked, so the frame clears only on a binding/size change - frame
  boundaries are not modelled yet; the depth surface is linear D32 only.
- Tests (gfx_draw_tests.cpp): two separate draws into one frame - far green full quad then near
  red centre quad keeps green outside and red inside (accumulation + cross-draw depth); reversed
  order (near first, far second) keeps the centre red (the far draw is blocked by earlier depth)
  while still covering empty depth; the same draws without a shared cache repaint the centre
  (control, per-draw behaviour); depth-write-off leaves depth untouched; a new z_base restarts
  the frame; Z16, tiled, stencil and separate-base bindings refused.
