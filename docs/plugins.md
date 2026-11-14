# Plugin system (slice P1: manifests, versions, manager core)

State: **plugin manager core is Implemented and Tested on synthetic plugins. Plugins cannot run yet.**
Isolated execution, signing, GitHub analysis, and the hooks into the library and store are later slices.
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
- Nothing runs. A `process` entry is recorded and shown as "NOT run yet".
- No signatures yet (the store's Ed25519 verification will be reused). Unsigned plugins are the only kind that exist today.
- Install from a GitHub URL, local package files, automatic updates and repository analysis are Planned.
- No code from any repository is used. The ps5-library repository is a jailbreak and piracy ecosystem payload and is
  not used. If it is ever analysed, its dump catalogs and download providers are refused.
