#include "package/importer.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "loader/elf_parser.hpp"
#include "package/sha256.hpp"
#include "package/tar.hpp"

namespace pnr::pkg {
namespace {

Status fail(int code, const std::string& msg) {
  return Status::Fail(code, "pkg.import", msg);
}

// key=value manifest parsing (same grammar as ConfigurationManager)
std::map<std::string, std::string> parse_manifest(const std::string& text,
                                                  Status* st) {
  std::map<std::string, std::string> out;
  std::istringstream in(text);
  std::string line;
  int n = 0;
  while (std::getline(in, line)) {
    ++n;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    auto hash = line.find('#');
    if (hash != std::string::npos) line.erase(hash);
    auto eq = line.find('=');
    if (eq == std::string::npos) {
      if (line.find_first_not_of(" \t") == std::string::npos) continue;
      *st = fail(60, "manifest parse error at line " + std::to_string(n));
      return {};
    }
    auto trim = [](std::string& s) {
      const char* ws = " \t";
      s.erase(0, s.find_first_not_of(ws));
      s.erase(s.find_last_not_of(ws) + 1);
    };
    std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    trim(k); trim(v);
    if (k.empty()) { *st = fail(61, "empty manifest key"); return {}; }
    out[k] = v;
  }
  return out;
}

Status validate_manifest(const std::map<std::string, std::string>& kv,
                         Manifest& m) {
  auto need = [&](const char* key, std::string* dst) -> Status {
    auto it = kv.find(key);
    if (it == kv.end() || it->second.empty())
      return fail(62, std::string("manifest missing required field: ") + key);
    *dst = it->second;
    return Status::Ok();
  };
  Status st = need("id", &m.id);
  if (!st) return st;
  st = need("title", &m.title);
  if (!st) return st;
  st = need("version", &m.version);
  if (!st) return st;
  st = need("executable", &m.executable);
  if (!st) return st;
  if (auto it = kv.find("arch"); it != kv.end()) m.arch = it->second;
  if (m.arch != "x86-64")
    return fail(63, "unsupported arch in manifest: " + m.arch);
  if (auto it = kv.find("min_runtime"); it != kv.end()) {
    try { m.min_runtime = std::stoll(it->second); }
    catch (...) { return fail(64, "bad min_runtime value"); }
  }
  // id is used as a directory name: refuse hostile ids
  if (m.id.find_first_not_of(
          "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") !=
      std::string::npos)
    return fail(65, "manifest id contains unsafe characters");
  return Status::Ok();
}

bool safe_entry_name(const std::string& n) {
  if (n.empty() || n[0] == '/' || n.find('\\') != std::string::npos ||
      n.find('\0') != std::string::npos)
    return false;
  // reject any .. path segment
  std::string seg;
  std::istringstream in(n);
  while (std::getline(in, seg, '/'))
    if (seg == "..") return false;
  return true;
}

}  // namespace

Status build_package(
    const std::string& out_path, const Manifest& m,
    const std::vector<std::pair<std::string, std::string>>& files) {
  std::vector<TarEntry> entries;

  std::ostringstream mf;
  mf << "id = " << m.id << "\n"
     << "title = " << m.title << "\n"
     << "version = " << m.version << "\n"
     << "executable = " << m.executable << "\n"
     << "arch = " << m.arch << "\n"
     << "min_runtime = " << m.min_runtime << "\n";
  std::string mftext = mf.str();
  entries.push_back({"manifest.pnr",
                     {mftext.begin(), mftext.end()}});

  std::ostringstream sums;
  for (const auto& [pkg_path, host_path] : files) {
    if (!safe_entry_name(pkg_path))
      return fail(66, "unsafe payload path: " + pkg_path);
    std::ifstream in(host_path, std::ios::binary);
    if (!in) return fail(67, "cannot read payload: " + host_path);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    sums << sha256_hex(bytes.data(), bytes.size()) << "  " << pkg_path << "\n";
    entries.push_back({pkg_path, std::move(bytes)});
  }
  std::string sumtext = sums.str();
  entries.push_back({"checksums.sha256", {sumtext.begin(), sumtext.end()}});

  return tar_write(out_path, entries);
}

ImportResult import_package(const std::string& package_path,
                            const std::string& library_root,
                            pnr::library::GameDb& db) {
  ImportResult r;

  std::vector<TarEntry> entries;
  Status st = tar_read(package_path, entries);
  if (!st) {
    r.status = fail(68, "not a valid .pnrgame archive: " + st.error.message);
    return r;
  }

  std::map<std::string, const TarEntry*> by_name;
  for (const auto& e : entries) {
    if (!safe_entry_name(e.name)) {
      r.status = fail(69, "unsafe entry name in archive: " + e.name);
      return r;
    }
    by_name[e.name] = &e;
  }

  auto mit = by_name.find("manifest.pnr");
  auto cit = by_name.find("checksums.sha256");
  if (mit == by_name.end() || cit == by_name.end()) {
    r.status = fail(70, "missing manifest.pnr or checksums.sha256 - this is "
                        "not a .pnrgame package (proprietary/protected "
                        "formats are unsupported, never guessed)");
    return r;
  }

  std::string mftext(mit->second->data.begin(), mit->second->data.end());
  st = Status::Ok();
  auto kv = parse_manifest(mftext, &st);
  if (!st) { r.status = st; return r; }
  st = validate_manifest(kv, r.manifest);
  if (!st) { r.status = st; return r; }

  // Verify every payload checksum before touching the disk.
  std::string sumtext(cit->second->data.begin(), cit->second->data.end());
  std::istringstream sums(sumtext);
  std::string line;
  size_t verified = 0;
  while (std::getline(sums, line)) {
    if (line.empty()) continue;
    auto sep = line.find("  ");
    if (sep == std::string::npos) {
      r.status = fail(71, "malformed checksum line: " + line);
      return r;
    }
    std::string want = line.substr(0, sep);
    std::string name = line.substr(sep + 2);
    auto eit = by_name.find(name);
    if (eit == by_name.end()) {
      r.status = fail(72, "checksum lists missing payload: " + name);
      return r;
    }
    std::string got =
        sha256_hex(eit->second->data.data(), eit->second->data.size());
    if (got != want) {
      r.status = fail(73, "CHECKSUM MISMATCH for " + name +
                              " (package corrupted or tampered)");
      return r;
    }
    ++verified;
  }
  if (verified == 0) {
    r.status = fail(74, "package carries no payload");
    return r;
  }

  auto eit = by_name.find(r.manifest.executable);
  if (eit == by_name.end()) {
    r.status = fail(75, "manifest executable not in package: " +
                            r.manifest.executable);
    return r;
  }
  {
    // Validate the executable from its bytes before it is ever written out.
    auto pr = pnr::loader::parse_elf_bytes(eit->second->data);
    if (!pr.ok || !pnr::loader::is_x86_64(pr.info) ||
        !pnr::loader::is_executable_type(pr.info)) {
      r.status = fail(76, "package executable is not a valid x86-64 ELF");
      return r;
    }
  }

  // Extract payload (not manifest/checksums) into the library root.
  std::string dest = library_root + "/games/" + r.manifest.id + "-" +
                     r.manifest.version;
  std::error_code ec;
  for (const auto& e : entries) {
    if (e.name == "manifest.pnr" || e.name == "checksums.sha256") continue;
    std::string target = dest + "/" + e.name;  // names already traversal-safe
    std::filesystem::create_directories(
        std::filesystem::path(target).parent_path(), ec);
    if (ec) { r.status = fail(77, "mkdir failed: " + ec.message()); return r; }
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(e.data.data()), e.data.size());
    if (!out) { r.status = fail(78, "extract failed: " + target); return r; }
    if (e.name == r.manifest.executable)
      std::filesystem::permissions(target,
                                   std::filesystem::perms::owner_all |
                                       std::filesystem::perms::group_read |
                                       std::filesystem::perms::group_exec |
                                       std::filesystem::perms::others_read |
                                       std::filesystem::perms::others_exec,
                                   ec);
  }
  r.install_path = dest;

  // Register in the library database. Compatibility starts at Untested:
  // importing never asserts playability.
  st = db.add_game({0, r.manifest.id, r.manifest.title, r.manifest.version,
                    "pnr-dev"});
  if (!st) { r.status = st; return r; }
  st = db.set_installation({r.manifest.id, dest, r.manifest.version,
                            "verified"});
  if (!st) { r.status = st; return r; }
  st = db.set_compatibility(
      {r.manifest.id, "Untested",
       "imported .pnrgame; no execution test yet"});
  if (!st) { r.status = st; return r; }

  r.status = Status::Ok();
  return r;
}

}  // namespace pnr::pkg
