# Plugin system (slice P1: manifests, versions, manager core)

State: **plugin manager core and sandboxed execution with a permission broker are Implemented and Tested on synthetic plugins (Linux).**
Static analysis, Ed25519 signatures and broker calls for library, store and download queueing are Implemented and Tested too. GitHub URL / package install is EXPERIMENTAL (offline-tested). The SDK reference is docs/plugin-sdk.md.
Windows: plugin execution is Blocked, same as running games (the core builds, nothing runs there).

## What a plugin is
A directory with `pnr-plugin.json`. Plugins are **untrusted**. The manifest only declares what a plugin wants.
Nothing in it grants anything.

```json
{
  "id": "example.hello-catalog", "name": "Hello Catalog (example)", "version": "1.0.0",
  "author": "PNR examples", "license": "GPL-2.0",
  "pnr_api": ">=1.0.0 <2.0.0",
  "kinds": ["catalog"],
  "permissions": ["fs_write_plugin_data"],
  "platforms": ["linux", "windows"],
  "dependencies": {"other.plugin": "^1.0.0"},
  "entry": {"type": "data"}
}
```

- Kinds: game_library, store_source, catalog, download_provider, package_extractor, installer, native_runtime,
  compatibility, performance, metadata.
- Versions are strict MAJOR.MINOR.PATCH. Constraints: `=` `>` `>=` `<` `<=` `^` `~` and `*`, all must hold.
- Permissions are a closed list, plus `net:<host>` for one named host. No wildcard hosts. A typo is an error.
  List: fs_write_plugin_data, library_read, library_write, store_add_source, download_request, install_package,
  runtime_hook (native_runtime plugins only), compat_report_write, perf_report_write.
- Entry type is `data` (no code) or `process` (a host program inside the plugin, relative path, no `..`).
  Console binaries (`elf`, `self`, `prx`, `ps5`, `console`) are refused with a plain reason. PNR never runs them as plugins.
- Every manifest problem is reported at once, not just the first.

## Manager rules
- `plugin add <dir>` validates the manifest, platform and plugin API version, scans the tree (regular files only:
  no symbolic links, no hidden files, at most 2000 files, 64 MiB, 8 levels), checks the entry file, rejects a
  dependency cycle, copies to a temporary directory and swaps it in. A failed install never damages an installed plugin.
- A new install is **disabled and holds no permissions**.
- `plugin enable <id> --grant a,b` needs the grant to contain every permission the manifest declares, by name.
  It also needs every dependency installed, in range and enabled.
- An update must be a newer version. If it asks for a permission you never granted it installs **disabled with the grants cleared**.
- A plugin cannot be disabled or removed while another plugin that depends on it is enabled (disable) or installed (remove).
- State is kept in `<home>/plugins/state.json` (home is `./pnr_data`, or `PNR_PLUGIN_HOME`).

## Limits, stated plainly
- A `process` entry runs only through `plugin run <id>`, only when enabled, only inside the PNR sandbox (user, net, pid, ipc and uts namespaces,
  seccomp denylist, pivot_root filesystem). That isolation is PARTIAL: a kernel bug or an allowed syscall misused is not covered, and there are
  no CPU or memory limits yet. If the sandbox is unusable the plugin is not started (no silent fallback).
- Broker (JSON lines on stdout/stdin): `log` always; `data_put` and `data_get` need the granted `fs_write_plugin_data` (own data dir only, names
  `[A-Za-z0-9._-]`, 64 KiB values); `result` ends the session. Anything else is refused and counted. Limits: wall-clock timeout, 1 MiB output, 500 requests.
- Library, store and download calls go through the broker only with the matching grant (P4, see docs/plugin-sdk.md). The plugin has no network (empty network namespace).
- Windows: plugin execution is Blocked.
- Automatic updates are Planned. Package-file and GitHub URL install and repository analysis exist (see below; GitHub is Experimental).
- No code from any repository is used. The ps5-library repository is a jailbreak and piracy ecosystem payload and is
  not used. If it is ever analysed, its dump catalogs and download providers are refused.

## Analysis, signatures, pinned keys (P3a)
- `plugin add` first analyses the tree without running anything. Refused: Sony SELF containers, FreeBSD-ABI ELF (console payload style),
  `.self/.sprx/.pup`, links to file-locker or torrent hosts, magnet links, dump catalogs with download links, jailbreak/exploit
  payload loaders, and any `net:` permission for a file-locker or torrent host. Native x86-64 ELF/PE files only produce a warning.
  These checks are heuristics. A clean result is not a safety guarantee; the sandbox and your grants are the real controls.
- Signatures: `pnr-plugin.sig` holds an Ed25519 key and signature over `path + sha512` of every other file. Any added, changed or
  removed file breaks it. `plugin sign <dir>` reads the seed from the file named by `PNR_PLUGIN_SEED_FILE` (never from the command line).
- `plugin trust add <name> <pubkey-hex> | list | remove <name>` pins keys (`plugins/trusted_keys.json`). A valid signature from an
  unpinned key proves nothing and is shown as "untrusted". `--require-signed` installs only plugins signed by a pinned key.
- Still Planned: automatic updates.

## Install from a package file or a GitHub URL (P3b)
- `plugin add <file.tar|file.tar.gz|file.tgz>`: unpacked in memory (own inflate + ustar/pax reader, no zlib). Limits: 2000 files, 64 MiB unpacked.
  Refused: symlinks, hard links, devices, `..`, absolute paths, backslashes, duplicate paths, bad checksums. One top-level folder is stripped.
  Hidden files (`.git`, `.github`, `.gitignore`) are dropped, never installed. Then the normal analysis and signature checks run.
- `plugin add https://github.com/<owner>/<repo>[/tree/<ref>]` maps to `https://codeload.github.com/<owner>/<repo>/tar.gz/<ref>` (ref `HEAD`
  when omitted, unverified against GitHub), through a guard that allows only that host over https, with a 32 MiB cap. It needs `--live-ok`.
  **EXPERIMENTAL:** tested offline with a fake HTTP client; the live fetch has not been run.
- A repository is untrusted like any plugin. A repo that looks like a download provider or exploit payload is refused by the analysis.
