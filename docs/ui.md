# Desktop launcher UI (`pnr-ui`)

Native Linux window app (C++, SDL2 + Dear ImGui). Optional build: `-DPNR_BUILD_UI=ON`
(needs `libsdl2-dev`; Dear ImGui v1.93.0, MIT, is fetched pinned by SHA256, not vendored).

## Status
| Item | State |
|---|---|
| Library screen: real SQLite game list, search, status badges | Implemented, tested under Xvfb |
| Launch button: same provenance re-check as `pnr-launcher launch`, optional partial sandbox | Implemented, tested (backend tests + real window) |
| Add games: read-only folder scan, then Import | Implemented, tested |
| Game details page | Planned (UI-2) |
| Store screen | Planned (UI-3) |
| Settings | Planned (UI-4) |
| Hardware, Diagnostics, Reports screens | Planned |
| Controller navigation | Planned |
| Real display / GPU hardware | Untested (software renderer under Xvfb only) |
| Windows | Blocked (PNR cannot run ELF games on Windows) |

## Run
    cmake -S . -B build -DPNR_BUILD_UI=ON && cmake --build build --target pnr-ui
    cd build && ./pnr-ui --db /path/to/pnr_data/library.db

Test hooks: `--frames N`, `--shot N:file.bmp`, `--select ID`, `--auto-launch ID`,
`--auto-sandbox`, `--scan-dir DIR`, `--size WxH`.

## Known limits
- Import and scan run on the UI thread, so a very large package can freeze the window.
- Launch output capture redirects process-wide fds 0/1/2 while a game runs.
- The sandbox is partial isolation, not a security boundary.
- No CI job builds the UI yet; `pnr_ui_smoke` runs only where `PNR_BUILD_UI=ON` and `xvfb-run` exist.
