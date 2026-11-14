# Platforms: what runs where (slice E honesty pass)

PNR executes **native x86-64 Linux code, directly**. No emulation, no VM, no
streaming, no translation layer. Anything outside that is listed here with
its real status. Statuses: Implemented / Tested / Experimental / Planned /
Blocked.

## Host operating systems

| Host | Status |
|---|---|
| Linux x86-64 | Implemented and Tested. This is the only platform where PNR runs anything. |
| Windows x86-64, build | The core cross-compiles with llvm-mingw (reported in an earlier slice; not re-run in this audit). CI (Linux only) does not build it. Never run on real Windows hardware. |
| Windows x86-64, hardware info (`pnr hwinfo`) | Implemented through Win32, untested on real Windows. |
| Windows x86-64, running a game (`pnr run`, `pnr-launcher launch`) | **Blocked.** PNR loads ELF files, Windows cannot execute them. The Windows backend refuses and says so. |
| Desktop UI (`pnr-ui`, docs/ui.md) | Linux only: Implemented for the Library screen (UI-1), Tested under Xvfb, untested on a real display/GPU. Windows: Blocked. |
| Translation core C1-C4e (`pnr module-run`, `pnr imports`, docs/core.md) | Linux only: Implemented, Tested on synthetic modules (module loading, TLS/TCB, libkernel ABI subset, guest threads, libc subset, PM4 decode, register meaning, GPU address map, one synthetic triangle draw). No real PlayStation executable has been loaded (Untested). Windows: Blocked (mmap and fork). |
| Windows sandbox | Planned, notes only (docs/sandbox.md). Nothing built. |
| Windows PSN token file hardening | Not implemented (docs/psn.md). |
| Windows live PSN sync, store downloads, `ask`, `manifest suggest` | Not built or tested. These are Linux only. |
| CI (GitHub Actions) | Implemented for Linux x86-64 only: Release `-Werror` build, unit suite, CTest, plus an ASan+UBSan job. Windows is not built or run there. No packaging, no release uploads, no GPU runners. |
| macOS | Not supported. No backend exists. |
| ARM or other non-x86-64 hosts | Not supported. PNR does not emulate x86-64. |

## Guest programs (what PNR can run)

| Guest | Status |
|---|---|
| Native x86-64 Linux ELF packaged as `.pnrgame` (PNR's own format) | Implemented, Tested on Linux. |
| Windows PC games or `.exe` files | **Not supported, not planned.** No Wine, Proton, DXVK or VKD3D style layer exists or is planned. `manifest suggest` ignores `.exe` files. |
| Commercial PS5 games or Sony packages | **Blocked.** Proprietary formats, authorization and DRM. PNR never decrypts, bypasses or opens them. |
| Android, macOS or other guest binaries | Not supported. |

A future Windows runtime path (PE/Win32) is Planned and not started. If it
ever lands it would run guests built for PNR itself, not Windows PC games.

## Rules this page keeps

- A claim about Windows support must say Blocked, Planned, Not built or Not
  supported, unless it is about the hardware-info call.
- No release is published. Downloadable releases are parked until the owner
  asks. A release would be Linux x86-64 only.
- `tests/docs_claims.cmake` (CTest `pnr_docs_claims`) fails the build if the
  docs regain an overclaim of this kind. It is a keyword check, not proof the
  docs are right.
