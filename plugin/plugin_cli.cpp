// Command logic for 'pnr-launcher plugin ...' (slice P1). GPL-2.0.
#include "plugin/plugin_cli.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "plugin/fetch.hpp"
#include "plugin/runner.hpp"

namespace pnr::plugin {
namespace {
std::vector<std::string> split_csv(const std::string& s) {
  std::vector<std::string> o;
  std::stringstream ss(s);
  std::string t;
  while (std::getline(ss, t, ',')) if (!t.empty()) o.push_back(t);
  return o;
}
std::string jesc(const std::string& s) {
  std::string o;
  for (unsigned char c : s) { if (c == '"' || c == '\\') { o += '\\'; o += char(c); } else if (c < 0x20) o += ' '; else o += char(c); }
  return o;
}
std::string join(const std::vector<std::string>& v, const char* sep) {
  std::string o;
  for (std::size_t i = 0; i < v.size(); ++i) o += (i ? sep : "") + v[i];
  return o;
}
std::string kinds_of(const Manifest& m) { std::vector<std::string> k; for (auto x : m.kinds) k.push_back(kind_name(x)); return join(k, ","); }
}  // namespace

int cmd_plugin(PluginManager& pm, const std::vector<std::string>& a, const std::string& grant, bool yes, bool json, std::string* out) {
  (void)yes;
  if (a.empty()) { *out = "usage: plugin add <dir|package.tar.gz|github-url> | list | info <id> | enable <id> --grant <perms> | disable <id> | remove <id> | run <id> | trust add <name> <pubkey-hex> | trust list | trust remove <name> | sign <dir>\n"; return 2; }
  const std::string& c = a[0];
  if (c == "add" && a.size() == 2) {
    InstallResult r;
    std::error_code fec;
    if (a[1].rfind("https://", 0) == 0) {
      GithubRef g; std::string why;
      if (!parse_github_url(a[1], &g, &why)) { *out = "plugin not installed:\n  - " + why + "\n"; return 1; }
      if (!pm.allow_network) { *out = "this would download " + github_archive_url(g) + " (EXPERIMENTAL, never run live in tests).\nre-run with --live-ok to allow the network fetch. The plugin is analysed like any other and installs disabled.\n"; return 1; }
      Status cs; auto http = psn::make_curl_client(psn::CurlOptions{}, &cs);
      if (!http) { *out = "plugin not installed:\n  - no HTTPS client available: " + cs.error.message + "\n"; return 1; }
      r = install_from_github(pm, *http, a[1]);
    } else if (std::filesystem::is_regular_file(a[1], fec)) r = install_from_archive_file(pm, a[1]);
    else r = pm.install_from_dir(a[1]);
    if (!r.ok) {
      *out = "plugin not installed:\n";
      for (auto& e : r.errors) *out += "  - " + e + "\n";
      return 1;
    }
    *out = std::string(r.updated ? "updated " : "installed ") + r.manifest.id + " " + r.manifest.version.str() + " (disabled)\n";
    if (r.disabled_by_update) *out += "this update asks for permissions you had not granted, so it stays disabled until you grant them\n";
    if (!r.manifest.permissions.empty()) *out += "it asks for: " + join(r.manifest.permissions, " ") + "\nto enable: plugin enable " + r.manifest.id + " --grant " + join(r.manifest.permissions, ",") + "\n";
    else *out += "to enable: plugin enable " + r.manifest.id + "\n";
    *out += "signature: " + r.signature + "\n"; for (auto& w : r.warnings) *out += "warning: " + w + "\n";
    *out += "plugins are untrusted. Nothing runs until you enable it and run it; it then runs only in the partial sandbox.\n";
    return 0;
  }
  if (c == "list" && a.size() == 1) {
    auto l = pm.list();
    if (json) {
      *out = "{\"plugins\":[";
      for (std::size_t i = 0; i < l.size(); ++i)
        *out += std::string(i ? "," : "") + "{\"id\":\"" + jesc(l[i].manifest.id) + "\",\"version\":\"" + l[i].manifest.version.str() + "\",\"enabled\":" + (l[i].enabled ? "true" : "false") + ",\"kinds\":\"" + kinds_of(l[i].manifest) + "\"}";
      *out += "]}\n";
    } else if (l.empty()) *out = "no plugins installed\n";
    else for (auto& p : l) *out += p.manifest.id + " " + p.manifest.version.str() + (p.enabled ? " enabled  " : " disabled ") + kinds_of(p.manifest) + "\n";
    return 0;
  }
  if (c == "info" && a.size() == 2) {
    InstalledPlugin p;
    if (!pm.get(a[1], &p)) { *out = "no installed plugin '" + a[1] + "'\n"; return 1; }
    const Manifest& m = p.manifest;
    std::vector<std::string> deps; for (auto& d : m.dependencies) deps.push_back(d.first + " " + d.second);
    if (json) {
      *out = "{\"id\":\"" + jesc(m.id) + "\",\"name\":\"" + jesc(m.name) + "\",\"version\":\"" + m.version.str() + "\",\"enabled\":" + (p.enabled ? "true" : "false") +
             ",\"kinds\":\"" + kinds_of(m) + "\",\"permissions\":\"" + jesc(join(m.permissions, ",")) + "\",\"granted\":\"" + jesc(join(p.granted, ",")) + "\"}\n";
    } else {
      *out = m.name + " (" + m.id + " " + m.version.str() + ")\n  author: " + m.author + "\n  license: " + m.license + "\n  kinds: " + kinds_of(m) +
             "\n  state: " + (p.enabled ? "enabled" : "disabled") + "\n  permissions asked: " + (m.permissions.empty() ? "none" : join(m.permissions, " ")) +
             "\n  permissions granted: " + (p.granted.empty() ? "none" : join(p.granted, " ")) + "\n  depends on: " + (deps.empty() ? "nothing" : join(deps, ", ")) +
             "\n  runs code: " + (m.entry_type == "process" ? "declares a host process (" + m.entry_path + "), runs only in the partial sandbox via plugin run" : "no (data only)") + "\n";
    }
    return 0;
  }
  if (c == "enable" && a.size() == 2) {
    auto s = pm.enable(a[1], split_csv(grant));
    if (!s.ok) { *out = "not enabled: " + s.error.message + "\n"; InstalledPlugin p; if (pm.get(a[1], &p) && !p.manifest.permissions.empty() && s.error.code == 403) *out += "to grant them: plugin enable " + a[1] + " --grant " + join(p.manifest.permissions, ",") + "\n"; return 1; }
    *out = "enabled " + a[1] + "\n";
    return 0;
  }
  if (c == "trust" && a.size() >= 2) {
    if (a[1] == "list") { auto k = pm.pinned_keys(); if (k.empty()) *out = "no pinned keys\n"; for (auto& x : k) *out += x.name + " " + x.pubkey_hex + "\n"; return 0; }
    if (a[1] == "add" && a.size() == 4) { auto s = pm.pin_key(a[2], a[3]); *out = s.ok ? "pinned key " + a[2] + "\n" : "not pinned: " + s.error.message + "\n"; return s.ok ? 0 : 1; }
    if (a[1] == "remove" && a.size() == 3) { auto s = pm.unpin_key(a[2]); *out = s.ok ? "unpinned " + a[2] + "\n" : "not unpinned: " + s.error.message + "\n"; return s.ok ? 0 : 1; }
  }
  if (c == "sign" && a.size() == 2) {  // author side. The seed is read from the file named by PNR_PLUGIN_SEED_FILE, never from the command line.
    const char* sf = std::getenv("PNR_PLUGIN_SEED_FILE");
    std::string seed; if (sf) { std::ifstream in(sf); std::getline(in, seed); }
    if (seed.empty()) { *out = "set PNR_PLUGIN_SEED_FILE to a file holding a 64-hex-character Ed25519 seed (keep it secret)\n"; return 1; }
    std::string pub, err;
    if (!sign_dir(a[1], seed, &pub, &err)) { *out = "not signed: " + err + "\n"; return 1; }
    *out = "signed " + a[1] + "\npublic key (give this to people who should pin it): " + pub + "\n";
    return 0;
  }
  if (c == "run" && a.size() == 2) {
    InstalledPlugin p;
    if (!pm.get(a[1], &p)) { *out = "no installed plugin '" + a[1] + "'\n"; return 1; }
    RunResult rr = run_plugin(p, pm.root());
    if (!rr.started) { *out = "not run: " + rr.error + "\n"; return 1; }
    for (auto& l : rr.logs) *out += "plugin log: " + l + "\n";
    *out += std::string(rr.completed ? "result: " + rr.result : "failed: " + rr.error) + "\nbroker: " + std::to_string(rr.served) + " served, " + std::to_string(rr.denied) + " denied (sandbox is PARTIAL isolation, not a guarantee)\n";
    return rr.completed ? 0 : 1;
  }
  if (c == "disable" && a.size() == 2) { auto s = pm.disable(a[1]); *out = s.ok ? "disabled " + a[1] + "\n" : "not disabled: " + s.error.message + "\n"; return s.ok ? 0 : 1; }
  if (c == "remove" && a.size() == 2) { auto s = pm.remove(a[1]); *out = s.ok ? "removed " + a[1] + "\n" : "not removed: " + s.error.message + "\n"; return s.ok ? 0 : 1; }
  *out = "usage: plugin add <dir|package.tar.gz|github-url> | list | info <id> | enable <id> --grant <perms> | disable <id> | remove <id> | run <id> | trust add <name> <pubkey-hex> | trust list | trust remove <name> | sign <dir>\n";
  return 2;
}

}  // namespace pnr::plugin
