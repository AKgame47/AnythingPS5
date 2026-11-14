// PNR plugin system - plugin manager core (slice P1), GPL-2.0.
// Installs a plugin from a local directory, keeps per-plugin state (enabled, granted permissions),
// checks versions and dependencies, and removes plugins. It does NOT run plugin code: isolated
// execution, signing, GitHub analysis and integration with the library and store are later slices.
// Plugins are untrusted. Installing grants nothing. Enabling needs the user to grant EVERY permission
// the manifest declares, by name. An update that asks for a permission the user never granted is
// installed disabled. Implemented, Tested on synthetic plugins.
#pragma once
#include <string>
#include <vector>

#include "core/runtime.hpp"
#include "plugin/manifest.hpp"
#include "plugin/signing.hpp"

namespace pnr::plugin {

struct InstalledPlugin {
  Manifest manifest;
  bool enabled = false;
  std::vector<std::string> granted;
  std::string dir;  // host path of the installed copy
};

struct InstallResult {
  bool ok = false;
  bool updated = false;            // replaced an older installed version
  bool disabled_by_update = false; // update asked for permissions the user had not granted
  std::vector<std::string> errors; // plain words
  std::string signature = "unsigned";  // unsigned | valid (key name) | untrusted key
  std::vector<std::string> warnings;     // from static analysis, install still allowed
  Manifest manifest;
};

class PluginManager {
 public:
  // root holds <root>/plugins/<id>/ and <root>/plugins/state.json.
  explicit PluginManager(std::string root);
  const std::string& root() const { return root_; }
  // Pinned Ed25519 keys (<root>/plugins/trusted_keys.json). Pinning is the user's decision, by name and hex key.
  std::vector<PinnedKey> pinned_keys() const;
  Status pin_key(const std::string& name, const std::string& pubkey_hex);
  Status unpin_key(const std::string& name);
  // When true, only plugins with a valid signature from a pinned key install. Default false (unsigned plugins allowed, shown as such).
  bool require_signed = false;

  InstallResult install_from_dir(const std::string& src_dir);
  // grant must contain every permission in the manifest (names exactly as declared).
  Status enable(const std::string& id, const std::vector<std::string>& grant);
  Status disable(const std::string& id);
  Status remove(const std::string& id);
  std::vector<InstalledPlugin> list() const;
  bool get(const std::string& id, InstalledPlugin* out) const;
  // Dependency problems for enabling `m` right now, in plain words. Empty means fine.
  std::vector<std::string> dependency_problems(const Manifest& m) const;
  // Enabled plugins of a kind, in id order.
  std::vector<InstalledPlugin> enabled_of_kind(Kind k) const;

  static constexpr std::size_t kMaxFiles = 2000;
  static constexpr std::uint64_t kMaxBytes = 64ull << 20;

 private:
  std::string plugins_dir() const { return root_ + "/plugins"; }
  std::string state_path() const { return plugins_dir() + "/state.json"; }
  struct State { bool enabled = false; std::vector<std::string> granted; };
  std::vector<std::pair<std::string, State>> load_state() const;
  Status save_state(const std::vector<std::pair<std::string, State>>& s) const;
  std::string root_;
};

}  // namespace pnr::plugin
