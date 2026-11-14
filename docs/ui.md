# Desktop launcher UI (`pnr-ui`)

Native Linux window app (C++, SDL2 + Dear ImGui). Optional build: `-DPNR_BUILD_UI=ON`
(needs `libsdl2-dev`; Dear ImGui v1.93.0, MIT, is fetched pinned by SHA256, not vendored).

Destination: the desktop app is the front door for the project goal of running real PS5
games on a PC by translation. Today it launches developer-built x86-64 packages only; that is
the current state on the road, not a limit of the goal. No decryption, no DRM bypass.

## Status
| Item | State |
|---|---|
| Library screen: real SQLite game list, search, status badges | Implemented, tested under Xvfb |
| Launch button: same provenance re-check as `pnr-launcher launch`, optional partial sandbox | Implemented, tested (backend tests + real window) |
| Add games: read-only folder scan, then Import | Implemented, tested |
| Game page: Overview, Compatibility (set by a person), History, Manage (verify, back up saves, two-step remove) | Implemented, backend calls tested; window checked under Xvfb |
| Store screen: user-added sources, refresh, package list, Get (download) and Install (scan, then import) | Implemented, backend tested with a fake network; live download only checked against an unreachable host (fails honestly) |
| Settings | Planned (UI-4) |
| Hardware, Diagnostics, Reports screens | Planned |
| Controller navigation | Planned |
| Real display / GPU hardware | Untested (software renderer under Xvfb only) |
| Windows | Blocked (PNR cannot run ELF games on Windows) |

## Run
    cmake -S . -B build -DPNR_BUILD_UI=ON && cmake --build build --target pnr-ui
    cd build && ./pnr-ui --db /path/to/pnr_data/library.db

Test hooks: `--frames N`, `--shot N:file.bmp`, `--select ID`, `--auto-launch ID`,
`--auto-sandbox`, `--scan-dir DIR`, `--size WxH`, `--page ID`, `--tab N`, `--store` (0 Overview, 1 Compatibility, 2 History, 3 Manage).

## Game page (UI-2)
Open it with the Details button, a double-click on a card, or Game page in the left rail.
Every action is the same `LauncherCore` call the `pnr-launcher` CLI makes: `compat set`, `history`,
`verify`, `export`, `remove`. Remove is two steps: step 1 is a dry run that lists the install and
saves folders, step 2 deletes only after you confirm. A status you set is marked human-set and no
launch or re-import overwrites it. Mutating actions are refused while a launch is running.
Limits: verify checks file presence and the ELF header only; save export refuses to overwrite;
the page is untested on a real display or GPU.

## Known limits
- Import and scan run on the UI thread, so a very large package can freeze the window.
- Launch output capture redirects process-wide fds 0/1/2 while a game runs.
- The sandbox is partial isolation, not a security boundary.
- No CI job builds the UI yet; `pnr_ui_smoke` runs only where `PNR_BUILD_UI=ON` and `xvfb-run` exist.

## Store screen (UI-3)
Every action is the same `store` code the CLI runs (`source add/remove`, `refresh`, `get`, `queue run`,
`install`). There is no default source: the screen is empty until you add one, and adding needs an
https index URL plus a rights statement you write. PNR cannot verify that statement. Network work
(refresh, download) runs on a worker thread. Only `pnr-package` entries can be fetched; other kinds show
as Unsupported. Install runs the same scan and importer as `store install`; a flagged scan refuses
and the UI has no override. Removing a source asks for a second click, and installed games are untouched.
Limits: no download progress bar yet (the job line shows the CLI result when it ends); one job at a time;
a pinned-key source shows "(key pinned)" but pinning is still CLI-only (`store source pin`);
a real download over the network was not exercised against a live host here.
