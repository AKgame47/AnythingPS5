# PNR plugin SDK

State: **Implemented and Tested on synthetic plugins (Linux).** Windows plugin execution is Blocked. The sandbox is PARTIAL isolation,
not a guarantee (see `sandbox/sandbox.hpp`). Plugin API version: `1.0.0`.

## A plugin is a folder
```
my-plugin/
  pnr-plugin.json      manifest (required)
  bin/run.sh           entry, only for "process" plugins (any executable that works inside the sandbox)
  pnr-plugin.sig       optional Ed25519 signature
```
Manifest fields: `id`, `name`, `version` (MAJOR.MINOR.PATCH), `description`, `author`, `license`, `pnr_api` (constraint such as
`>=1.0.0 <2.0.0`), `kinds`, `permissions`, `platforms`, `dependencies` (`{"other.id": "^1.0.0"}`), `entry`.
Kinds: `game_library`, `store_source`, `catalog`, `download_provider`, `package_extractor`, `installer`, `native_runtime`,
`compatibility`, `performance`, `metadata`. Version constraints: `= > >= < <= ^ ~ *`.

## Permissions (closed list, declared in the manifest, granted by the user by name)
| Permission | What the broker allows |
| --- | --- |
| `fs_write_plugin_data` | `data_put` / `data_get` in the plugin's own data folder (64 KiB values) |
| `library_read` | `library_list`: ids and titles of the user's games |
| `store_add_source` | `store_add_source`: add a store source (https URL, rights statement). Marked "declared by plugin". Nothing is fetched |
| `download_request` | `download_request`: QUEUE a package from an existing source. Never downloads or installs; unsupported kinds are refused |
| `net:<host>` | declared for the manifest. Plugins have NO network in this release (empty network namespace). File-locker and torrent hosts are refused |

## Talking to the host
One JSON object per line on stdout; the host answers one JSON line on stdin. Always read the answer.
```
{"call":"log","msg":"text"}                                  -> {"ok":true}
{"call":"library_list"}                                      -> {"ok":true,"value":"[{\"id\":..,\"title\":..}]"}
{"call":"data_put","name":"k","value":"v"}                   -> {"ok":true}
{"call":"data_get","name":"k"}                               -> {"ok":true,"value":"v"}
{"call":"store_add_source","id":"s","url":"https://..","rights":".."}
{"call":"download_request","source":"s","pkg":"p"}
{"call":"result","value":"text"}                             ends the session
```
A refused call returns `{"ok":false,"error":"..."}` (missing grant, unknown call, host refusal). Limits: 10 s wall clock, 1 MiB output,
500 requests. Environment: `PNR_PLUGIN_ID`, `PNR_PLUGIN_DATA` (empty unless `fs_write_plugin_data` was granted).

## Working together
Plugins do not call each other. They share data through the host: one plugin can add a source or queue a package, another reads the
library. `dependencies` makes the manager refuse to enable a plugin whose dependencies are missing, out of range or disabled.
Plugins of the same kind are listed together (`enabled_of_kind`).

## Lifecycle
The entry file must be executable (`chmod +x bin/run.sh`); a web upload or zip can drop that bit.
`plugin add <dir|package|github-url>` (analysis, signature, installs disabled) -> `plugin enable <id> --grant ...` -> `plugin run <id>`
-> `plugin disable|remove`. Updates install only if newer, and an update that asks for a new permission stays disabled.

## What PNR refuses, always
Console binaries (SELF, console-ABI ELF), download providers for copyrighted games, dump catalogs, jailbreak/exploit payloads, and
console entry types. If a repository is of that kind, there is no adapter for it. A clean PC adapter is a plugin that works on
files the user owns, such as metadata, covers, or a library importer.

## Not built yet
Automatic updates, a settings screen for plugins (UI-4 is paused), plugin network access through the broker, CPU/memory limits,
Windows execution.
