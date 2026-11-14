// PNR plugin system - manifest (slice P1), GPL-2.0.
// A plugin is a directory with "pnr-plugin.json". Plugins are UNTRUSTED: the manifest only declares what a
// plugin wants. Nothing in it grants anything. Implemented, Tested on synthetic manifests.
//
// {
//   "id": "example.catalog", "name": "Example", "version": "1.0.0",
//   "description": "...", "author": "...", "license": "GPL-2.0",
//   "pnr_api": ">=1.0.0 <2.0.0",
//   "kinds": ["catalog"],
//   "permissions": ["net:example.org", "fs_write_plugin_data"],
//   "platforms": ["linux", "windows"],
//   "dependencies": {"other.plugin": "^1.0.0"},
//   "entry": {"type": "process", "path": "bin/plugin"}      // or {"type": "data"}
// }
#pragma once
#include <map>
#include <string>
#include <vector>

#include "plugin/semver.hpp"

namespace pnr::plugin {

inline constexpr Version kPluginApiVersion{1, 0, 0};
inline constexpr const char* kManifestFile = "pnr-plugin.json";

enum class Kind {
  GameLibrary, StoreSource, Catalog, DownloadProvider, PackageExtractor, Installer, NativeRuntime, Compatibility, Performance, Metadata
};
const char* kind_name(Kind k);
bool kind_from_name(const std::string& s, Kind* out);

// Permission names. "net:<host>" needs a host. Anything else must be in this list, so a typo is an error.
bool valid_permission(const std::string& p);

struct Manifest {
  std::string id, name, description, author, license;
  Version version;
  Constraint pnr_api;
  std::vector<Kind> kinds;
  std::vector<std::string> permissions;     // declared, not granted
  std::vector<std::string> platforms;       // "linux", "windows"
  std::map<std::string, std::string> dependencies;  // id -> constraint text
  std::string entry_type = "data";          // "data" or "process"
  std::string entry_path;                   // relative path inside the plugin, for "process"
};

struct ParseResult {
  bool ok = false;
  Manifest manifest;
  std::vector<std::string> errors;          // every problem found, in plain words
};

ParseResult parse_manifest(const std::string& json_text);
// "linux" on this build. Windows builds report "windows" but plugin execution there is Blocked.
const char* host_platform();
// Empty if the plugin can be installed here; otherwise plain-words reasons.
std::vector<std::string> check_compatible(const Manifest& m);
bool valid_plugin_id(const std::string& id);
// Path inside a plugin: relative, no "..", no leading '/', no backslash, no empty segments.
bool valid_relative_path(const std::string& p);

}  // namespace pnr::plugin
