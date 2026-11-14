// PNR plugin system - plugin manager core (slice P1), GPL-2.0.
#include "plugin/manager.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>

#include "psn/json.hpp"

namespace pnr::plugin {
namespace fs = std::filesystem;
using psn::Json;

namespace {
std::string json_escape(const std::string& s) {
  std::string o;
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += static_cast<char>(c);
  }
  return o;
}
bool read_file(const std::string& p, std::string* out, std::size_t max) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss; ss << f.rdbuf();
  *out = ss.str();
  return out->size() <= max;
}
// Checks a source tree before anything is copied: regular files and directories only, bounded.
bool scan_tree(const fs::path& src, std::size_t* files, std::uint64_t* bytes, std::string* why) {
  std::error_code ec;
  *files = 0; *bytes = 0;
  for (auto it = fs::recursive_directory_iterator(src, fs::directory_options::none, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) { *why = "cannot read the plugin directory: " + ec.message(); return false; }
    const fs::path rel = fs::relative(it->path(), src, ec);
    if (it->is_symlink(ec)) { *why = "plugin contains a symbolic link: " + rel.string(); return false; }
    if (it.depth() > 8) { *why = "plugin directories are nested more than 8 deep"; return false; }
    const std::string name = it->path().filename().string();
    if (name.empty() || name[0] == '.' ) { *why = "plugin contains a hidden file or directory: " + rel.string(); return false; }
    if (it->is_directory(ec)) continue;
    if (!it->is_regular_file(ec)) { *why = "plugin contains something that is not a regular file: " + rel.string(); return false; }
    *bytes += it->file_size(ec);
    if (++*files > PluginManager::kMaxFiles) { *why = "plugin has too many files"; return false; }
    if (*bytes > PluginManager::kMaxBytes) { *why = "plugin is larger than 64 MiB"; return false; }
  }
  return true;
}
}  // namespace

PluginManager::PluginManager(std::string root) : root_(std::move(root)) {}

std::vector<std::pair<std::string, PluginManager::State>> PluginManager::load_state() const {
  std::vector<std::pair<std::string, State>> out;
  std::string text;
  if (!read_file(state_path(), &text, 1 << 20)) return out;
  Json j; std::string err;
  if (!psn::json_parse(text, &j, &err) || !j.is(Json::T::Obj)) return out;
  for (const auto& kv : j.o) {
    State s;
    if (const Json* e = kv.second.get("enabled")) s.enabled = e->is(Json::T::Bool) && e->b;
    if (const Json* g = kv.second.get("granted")) if (g->is(Json::T::Arr)) for (const Json& x : g->a) if (x.is(Json::T::Str)) s.granted.push_back(x.s);
    out.emplace_back(kv.first, std::move(s));
  }
  return out;
}

Status PluginManager::save_state(const std::vector<std::pair<std::string, State>>& s) const {
  std::string o = "{\n";
  for (std::size_t i = 0; i < s.size(); ++i) {
    o += "  \"" + json_escape(s[i].first) + "\": {\"enabled\": " + (s[i].second.enabled ? "true" : "false") + ", \"granted\": [";
    for (std::size_t k = 0; k < s[i].second.granted.size(); ++k) o += (k ? ", \"" : "\"") + json_escape(s[i].second.granted[k]) + "\"";
    o += "]}" + std::string(i + 1 < s.size() ? ",\n" : "\n");
  }
  o += "}\n";
  std::error_code ec;
  fs::create_directories(plugins_dir(), ec);
  const std::string tmp = state_path() + ".tmp";
  { std::ofstream f(tmp, std::ios::binary); f << o; if (!f) return Status::Fail(420, "plugin", "cannot write plugin state"); }
  fs::rename(tmp, state_path(), ec);
  return ec ? Status::Fail(420, "plugin", "cannot replace plugin state: " + ec.message()) : Status::Ok();
}

bool PluginManager::get(const std::string& id, InstalledPlugin* out) const {
  if (!valid_plugin_id(id)) return false;
  std::string text;
  const std::string dir = plugins_dir() + "/" + id;
  if (!read_file(dir + "/" + kManifestFile, &text, 256 * 1024)) return false;
  auto pr = parse_manifest(text);
  if (!pr.ok || pr.manifest.id != id) return false;
  InstalledPlugin p;
  p.manifest = pr.manifest; p.dir = dir;
  for (auto& kv : load_state()) if (kv.first == id) { p.enabled = kv.second.enabled; p.granted = kv.second.granted; }
  if (out) *out = std::move(p);
  return true;
}

std::vector<InstalledPlugin> PluginManager::list() const {
  std::vector<InstalledPlugin> o;
  std::error_code ec;
  if (!fs::is_directory(plugins_dir(), ec)) return o;
  for (auto& e : fs::directory_iterator(plugins_dir(), ec)) {
    if (!e.is_directory(ec)) continue;
    InstalledPlugin p;
    if (get(e.path().filename().string(), &p)) o.push_back(std::move(p));
  }
  std::sort(o.begin(), o.end(), [](const InstalledPlugin& a, const InstalledPlugin& b) { return a.manifest.id < b.manifest.id; });
  return o;
}

std::vector<InstalledPlugin> PluginManager::enabled_of_kind(Kind k) const {
  std::vector<InstalledPlugin> o;
  for (auto& p : list()) if (p.enabled && std::find(p.manifest.kinds.begin(), p.manifest.kinds.end(), k) != p.manifest.kinds.end()) o.push_back(p);
  return o;
}

std::vector<std::string> PluginManager::dependency_problems(const Manifest& m) const {
  std::vector<std::string> why;
  for (const auto& d : m.dependencies) {
    InstalledPlugin dep;
    if (!get(d.first, &dep)) { why.push_back("needs plugin '" + d.first + "' (" + d.second + "), which is not installed"); continue; }
    Constraint c; c.parse(d.second);
    if (!c.satisfied_by(dep.manifest.version)) why.push_back("needs '" + d.first + "' " + d.second + " but " + dep.manifest.version.str() + " is installed");
    else if (!dep.enabled) why.push_back("needs '" + d.first + "' to be enabled first");
  }
  return why;
}

InstallResult PluginManager::install_from_dir(const std::string& src_dir) {
  InstallResult r;
  std::error_code ec;
  const fs::path src(src_dir);
  if (!fs::is_directory(src, ec)) { r.errors.push_back("not a directory: " + src_dir); return r; }
  std::string text;
  if (!read_file((src / kManifestFile).string(), &text, 256 * 1024)) { r.errors.push_back(std::string("no readable ") + kManifestFile + " in the plugin directory"); return r; }
  auto pr = parse_manifest(text);
  if (!pr.ok) { r.errors = pr.errors; return r; }
  r.manifest = pr.manifest;
  const Manifest& m = pr.manifest;
  if (auto why = check_compatible(m); !why.empty()) { r.errors = why; return r; }
  std::size_t files = 0; std::uint64_t bytes = 0; std::string why;
  if (!scan_tree(src, &files, &bytes, &why)) { r.errors.push_back(why); return r; }
  if (m.entry_type == "process" && !fs::is_regular_file(src / m.entry_path, ec)) { r.errors.push_back("entry.path '" + m.entry_path + "' is not a file in the plugin"); return r; }

  // Dependency cycle check over the installed set plus this plugin.
  std::map<std::string, std::vector<std::string>> g;
  for (auto& p : list()) for (auto& d : p.manifest.dependencies) g[p.manifest.id].push_back(d.first);
  g[m.id].clear();
  for (auto& d : m.dependencies) g[m.id].push_back(d.first);
  std::set<std::string> done, stack;
  bool cycle = false;
  std::function<void(const std::string&)> dfs = [&](const std::string& n) {
    if (cycle || done.count(n)) return;
    if (!stack.insert(n).second) { cycle = true; return; }
    for (auto& d : g[n]) dfs(d);
    stack.erase(n); done.insert(n);
  };
  dfs(m.id);
  if (cycle) { r.errors.push_back("installing this plugin would create a dependency cycle"); return r; }

  InstalledPlugin old;
  const bool had = get(m.id, &old);
  if (had && !(old.manifest.version < m.version)) {
    r.errors.push_back("version " + old.manifest.version.str() + " is already installed; this is " + m.version.str() + " (only newer versions update)");
    return r;
  }
  // Copy to a temporary directory, then swap. A failed copy never damages an installed plugin.
  fs::create_directories(plugins_dir(), ec);
  const fs::path tmp = fs::path(plugins_dir()) / (".incoming-" + m.id);
  fs::remove_all(tmp, ec);
  fs::copy(src, tmp, fs::copy_options::recursive, ec);
  if (ec) { fs::remove_all(tmp, ec); r.errors.push_back("copy failed: " + ec.message()); return r; }
  const fs::path dst = fs::path(plugins_dir()) / m.id;
  fs::remove_all(dst, ec);
  fs::rename(tmp, dst, ec);
  if (ec) { r.errors.push_back("could not place the plugin: " + ec.message()); return r; }

  auto st = load_state();
  st.erase(std::remove_if(st.begin(), st.end(), [&](auto& kv) { return kv.first == m.id; }), st.end());
  State ns;
  if (had) {
    r.updated = true;
    ns.granted = old.granted;
    bool covered = true;
    for (auto& p : m.permissions) if (std::find(old.granted.begin(), old.granted.end(), p) == old.granted.end()) covered = false;
    ns.enabled = old.enabled && covered;
    r.disabled_by_update = old.enabled && !covered;
    if (!covered) ns.granted.clear();
  }
  st.emplace_back(m.id, ns);
  std::sort(st.begin(), st.end(), [](auto& a, auto& b) { return a.first < b.first; });
  auto s = save_state(st);
  if (!s.ok) { r.errors.push_back(s.error.message); return r; }
  r.ok = true;
  return r;
}

Status PluginManager::enable(const std::string& id, const std::vector<std::string>& grant) {
  InstalledPlugin p;
  if (!get(id, &p)) return Status::Fail(404, "plugin", "no installed plugin '" + id + "'");
  std::vector<std::string> missing;
  for (auto& need : p.manifest.permissions) if (std::find(grant.begin(), grant.end(), need) == grant.end()) missing.push_back(need);
  if (!missing.empty()) {
    std::string msg = "this plugin asks for permissions you did not grant:";
    for (auto& m : missing) msg += " " + m;
    return Status::Fail(403, "plugin", msg);
  }
  auto why = dependency_problems(p.manifest);
  if (!why.empty()) return Status::Fail(409, "plugin", why.front());
  auto st = load_state();
  bool found = false;
  for (auto& kv : st) if (kv.first == id) { kv.second.enabled = true; kv.second.granted = p.manifest.permissions; found = true; }
  if (!found) st.emplace_back(id, State{true, p.manifest.permissions});
  return save_state(st);
}

Status PluginManager::disable(const std::string& id) {
  InstalledPlugin p;
  if (!get(id, &p)) return Status::Fail(404, "plugin", "no installed plugin '" + id + "'");
  for (auto& o : list())
    if (o.enabled && o.manifest.dependencies.count(id)) return Status::Fail(409, "plugin", "plugin '" + o.manifest.id + "' is enabled and depends on it; disable that first");
  auto st = load_state();
  for (auto& kv : st) if (kv.first == id) kv.second.enabled = false;
  return save_state(st);
}

Status PluginManager::remove(const std::string& id) {
  InstalledPlugin p;
  if (!get(id, &p)) return Status::Fail(404, "plugin", "no installed plugin '" + id + "'");
  for (auto& o : list())
    if (o.manifest.id != id && o.manifest.dependencies.count(id)) return Status::Fail(409, "plugin", "plugin '" + o.manifest.id + "' depends on it; remove that first");
  std::error_code ec;
  fs::remove_all(p.dir, ec);
  if (ec) return Status::Fail(421, "plugin", "could not remove files: " + ec.message());
  auto st = load_state();
  st.erase(std::remove_if(st.begin(), st.end(), [&](auto& kv) { return kv.first == id; }), st.end());
  return save_state(st);
}

}  // namespace pnr::plugin
