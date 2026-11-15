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
  transform runs over the referenced vertices (max index + 1, still the 24-lane kernel limit).
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
- Not done: 8-bit indices, primitive restart, strips/fans, instancing beyond 1, indirect draws,
  vertex counts past 24, real GNM fetch shaders.

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
