// PNR plugin system - manifest (slice P1), GPL-2.0.
#include "plugin/manifest.hpp"

#include <algorithm>
#include <cctype>
#include <set>

#include "psn/json.hpp"

namespace pnr::plugin {
using psn::Json;

namespace {
struct KindName { Kind k; const char* n; };
const KindName kKinds[] = {
    {Kind::GameLibrary, "game_library"}, {Kind::StoreSource, "store_source"}, {Kind::Catalog, "catalog"},
    {Kind::DownloadProvider, "download_provider"}, {Kind::PackageExtractor, "package_extractor"}, {Kind::Installer, "installer"},
    {Kind::NativeRuntime, "native_runtime"}, {Kind::Compatibility, "compatibility"}, {Kind::Performance, "performance"},
    {Kind::Metadata, "metadata"}};
const char* const kPerms[] = {"fs_write_plugin_data", "library_read", "library_write", "store_add_source", "download_request",
                              "install_package", "runtime_hook", "compat_report_write", "perf_report_write"};

bool valid_host(const std::string& h) {
  if (h.empty() || h.size() > 253 || h.front() == '.' || h.back() == '.' || h.find("..") != std::string::npos) return false;
  return std::all_of(h.begin(), h.end(), [](unsigned char c) { return std::islower(c) || std::isdigit(c) || c == '.' || c == '-'; });
}
bool str_field(const Json& o, const char* key, bool required, std::size_t maxlen, std::string* out, std::vector<std::string>* errs) {
  const Json* j = o.get(key);
  if (!j) { if (required) errs->push_back(std::string("missing field '") + key + "'"); return false; }
  if (!j->is(Json::T::Str) || j->s.empty() || j->s.size() > maxlen) {
    errs->push_back(std::string("field '") + key + "' must be a non-empty string of at most " + std::to_string(maxlen) + " characters");
    return false;
  }
  *out = j->s;
  return true;
}
}  // namespace

const char* kind_name(Kind k) {
  for (auto& e : kKinds) if (e.k == k) return e.n;
  return "?";
}
bool kind_from_name(const std::string& s, Kind* out) {
  for (auto& e : kKinds) if (s == e.n) { if (out) *out = e.k; return true; }
  return false;
}
bool valid_permission(const std::string& p) {
  if (p.compare(0, 4, "net:") == 0) return valid_host(p.substr(4));
  for (const char* k : kPerms) if (p == k) return true;
  return false;
}
bool valid_plugin_id(const std::string& id) {
  if (id.size() < 3 || id.size() > 64 || !std::islower(static_cast<unsigned char>(id[0]))) return false;
  return std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::islower(c) || std::isdigit(c) || c == '.' || c == '_' || c == '-'; }) &&
         id.find("..") == std::string::npos && id.back() != '.';
}
bool valid_relative_path(const std::string& p) {
  if (p.empty() || p.size() > 255 || p.front() == '/' || p.back() == '/' || p.find('\\') != std::string::npos || p.find('\0') != std::string::npos) return false;
  std::size_t start = 0;
  while (start <= p.size()) {
    std::size_t e = p.find('/', start);
    if (e == std::string::npos) e = p.size();
    const std::string seg = p.substr(start, e - start);
    if (seg.empty() || seg == "." || seg == "..") return false;
    start = e + 1;
  }
  return true;
}
const char* host_platform() {
#if defined(_WIN32)
  return "windows";
#else
  return "linux";
#endif
}

ParseResult parse_manifest(const std::string& text) {
  ParseResult r;
  Json root;
  std::string jerr;
  if (text.size() > 256 * 1024) { r.errors.push_back("manifest is larger than 256 KiB"); return r; }
  if (!psn::json_parse(text, &root, &jerr)) { r.errors.push_back("manifest is not valid JSON: " + jerr); return r; }
  if (!root.is(Json::T::Obj)) { r.errors.push_back("manifest must be a JSON object"); return r; }
  Manifest& m = r.manifest;
  auto& errs = r.errors;
  if (str_field(root, "id", true, 64, &m.id, &errs) && !valid_plugin_id(m.id))
    errs.push_back("id must be 3..64 characters of a-z, 0-9, '.', '_', '-', starting with a letter");
  str_field(root, "name", true, 80, &m.name, &errs);
  str_field(root, "description", false, 1000, &m.description, &errs);
  str_field(root, "author", true, 80, &m.author, &errs);
  str_field(root, "license", true, 40, &m.license, &errs);
  std::string v;
  if (str_field(root, "version", true, 32, &v, &errs) && !parse_version(v, &m.version))
    errs.push_back("version must be MAJOR.MINOR.PATCH with digits only (pre-release suffixes are not accepted)");
  std::string api;
  if (str_field(root, "pnr_api", true, 64, &api, &errs)) {
    const std::string e = m.pnr_api.parse(api);
    if (!e.empty()) errs.push_back("pnr_api: " + e);
  }
  if (const Json* k = root.get("kinds")) {
    if (!k->is(Json::T::Arr) || k->a.empty() || k->a.size() > 10) errs.push_back("kinds must be a list of 1..10 names");
    else for (const Json& e : k->a) {
      Kind kk;
      if (!e.is(Json::T::Str) || !kind_from_name(e.s, &kk)) errs.push_back("unknown plugin kind '" + (e.is(Json::T::Str) ? e.s : std::string("?")) + "'");
      else if (std::find(m.kinds.begin(), m.kinds.end(), kk) == m.kinds.end()) m.kinds.push_back(kk);
    }
  } else errs.push_back("missing field 'kinds'");
  if (const Json* p = root.get("permissions")) {
    if (!p->is(Json::T::Arr) || p->a.size() > 32) errs.push_back("permissions must be a list of at most 32 names");
    else for (const Json& e : p->a) {
      if (!e.is(Json::T::Str) || !valid_permission(e.s)) errs.push_back("unknown or malformed permission '" + (e.is(Json::T::Str) ? e.s : std::string("?")) + "'");
      else if (std::find(m.permissions.begin(), m.permissions.end(), e.s) == m.permissions.end()) m.permissions.push_back(e.s);
    }
  }
  if (const Json* p = root.get("platforms")) {
    if (!p->is(Json::T::Arr) || p->a.empty()) errs.push_back("platforms must be a non-empty list");
    else for (const Json& e : p->a) {
      if (!e.is(Json::T::Str) || (e.s != "linux" && e.s != "windows")) errs.push_back("unknown platform (use \"linux\" or \"windows\")");
      else m.platforms.push_back(e.s);
    }
  } else errs.push_back("missing field 'platforms'");
  if (const Json* d = root.get("dependencies")) {
    if (!d->is(Json::T::Obj) || d->o.size() > 32) errs.push_back("dependencies must be an object of at most 32 entries");
    else for (const auto& kv : d->o) {
      Constraint c;
      if (!valid_plugin_id(kv.first)) errs.push_back("dependency id '" + kv.first + "' is not a valid plugin id");
      else if (!kv.second.is(Json::T::Str)) errs.push_back("dependency '" + kv.first + "' needs a constraint string");
      else if (kv.first == m.id) errs.push_back("a plugin cannot depend on itself");
      else { const std::string e = c.parse(kv.second.s); if (!e.empty()) errs.push_back("dependency '" + kv.first + "': " + e); else m.dependencies[kv.first] = kv.second.s; }
    }
  }
  if (const Json* e = root.get("entry")) {
    const Json* t = e->is(Json::T::Obj) ? e->get("type") : nullptr;
    if (!t || !t->is(Json::T::Str)) errs.push_back("entry must be an object with a string 'type'");
    else if (t->s == "data") m.entry_type = "data";
    else if (t->s == "process") {
      m.entry_type = "process";
      const Json* p = e->get("path");
      if (!p || !p->is(Json::T::Str) || !valid_relative_path(p->s)) errs.push_back("entry.path must be a relative path inside the plugin (no '..', no leading '/')");
      else m.entry_path = p->s;
    } else if (t->s == "elf" || t->s == "self" || t->s == "prx" || t->s == "ps5" || t->s == "console")
      errs.push_back("entry type '" + t->s + "' is a console binary: PNR never runs those as plugins. Ask the author for a PC plugin, or use an adapter");
    else errs.push_back("unknown entry type '" + t->s + "' (use \"data\" or \"process\")");
  }
  if (m.entry_type == "process" && std::find(m.permissions.begin(), m.permissions.end(), "runtime_hook") != m.permissions.end() &&
      std::find(m.kinds.begin(), m.kinds.end(), Kind::NativeRuntime) == m.kinds.end())
    errs.push_back("permission runtime_hook is only for plugins of kind native_runtime");
  r.ok = errs.empty();
  return r;
}

std::vector<std::string> check_compatible(const Manifest& m) {
  std::vector<std::string> why;
  if (std::find(m.platforms.begin(), m.platforms.end(), host_platform()) == m.platforms.end())
    why.push_back(std::string("plugin does not list this platform (") + host_platform() + ")");
  if (!m.pnr_api.satisfied_by(kPluginApiVersion))
    why.push_back("plugin needs plugin API '" + m.pnr_api.text() + "' but this PNR provides " + kPluginApiVersion.str());
  return why;
}

}  // namespace pnr::plugin
