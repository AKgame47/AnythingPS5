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
