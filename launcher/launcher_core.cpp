// PNR launcher core implementation, GPL-2.0. See launcher_core.hpp.
#include "launcher/launcher_core.hpp"

#include <cstdio>
#include <filesystem>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <fstream>

#include "loader/elf_parser.hpp"
#include "loader/import_resolver.hpp"
#include "loader/native_loader.hpp"
#include "package/importer.hpp"
#include "package/tar.hpp"
#include "prx/prx.hpp"

namespace pnr::launcher {

Status LauncherCore::open(const std::string& db_path) {
  if (open_) return Status::Ok();
  auto st = db_.open(db_path);
  if (st) {
    open_ = true;
    auto parent = std::filesystem::path(db_path).parent_path();
    root_ = (parent.empty() ? std::filesystem::path(".") : parent).string();
    jail_root_ = root_ + "/jail";
  }
  return st;
}

void LauncherCore::close() {
  if (open_) db_.close();
  open_ = false;
}

static GameEntry build(library::GameDb& db, const library::GameRecord& g) {
  GameEntry e;
  e.game_id = g.game_id; e.title = g.title; e.version = g.version; e.platform = g.platform;
  if (auto c = db.get_compatibility(g.game_id)) { e.compatibility = c->state; e.compatibility_notes = c->notes; }
  if (auto i = db.get_installation(g.game_id)) {
    e.installed = true; e.install_path = i->path; e.executable = i->executable; e.integrity = i->integrity;
  }
  e.launch_count = db.launch_count(g.game_id);
  e.compat_human_set = db.compat_is_human_set(g.game_id);
  if (!e.installed) e.not_runnable_reason = "not installed";
  else if (e.executable.empty()) e.not_runnable_reason = "installation has no entry point";
  else if (e.integrity == "failed") e.not_runnable_reason = "integrity check failed";
  else e.runnable = true;
  return e;
}

std::vector<GameEntry> LauncherCore::list() {
  std::vector<GameEntry> out;
  if (!open_) return out;
  for (const auto& g : db_.list_games()) out.push_back(build(db_, g));
  return out;
}

bool LauncherCore::get(const std::string& game_id, GameEntry* out) {
  if (!open_ || !out) return false;
  auto g = db_.get_game(game_id);
  if (!g) return false;
  *out = build(db_, *g);
  return true;
}

bool valid_game_id(const std::string& id) {
  if (id.empty() || id.size() > 128 || id == "." || id == "..") return false;
  return id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") == std::string::npos;
}

ImportOutcome LauncherCore::import_package(const std::string& pkg_path) {
  ImportOutcome o;
  if (!open_) { o.error = "library is not open"; return o; }
  // Remember what was installed before so the report can say what changed.
  std::string prev_ver, prev_path;
  bool had = false;
  auto r = pkg::inspect_package(pkg_path);  // read-only pre-check gives the id
  if (!r.status) { o.error = r.status.error.message; return o; }
  if (auto i = db_.get_installation(r.manifest.id)) { had = true; prev_ver = i->version; prev_path = i->path; }
  auto ir = pkg::import_package(pkg_path, root_, db_);
  if (!ir.status) { o.error = ir.status.error.message; return o; }
  o.ok = true;
  o.game_id = ir.manifest.id; o.title = ir.manifest.title; o.version = ir.manifest.version;
  o.install_path = ir.install_path;
  if (!had) o.kind = "fresh";
  else if (prev_ver == o.version) o.kind = "reinstall";
  else { o.kind = "different-version"; o.previous_version = prev_ver; o.previous_path = prev_path; }
  if (had) o.previous_version = prev_ver;
  return o;
}

static bool is_inside(const std::filesystem::path& base, const std::filesystem::path& p) {
  // both already canonical/absolute; p must be strictly below base
  auto b = base.begin(), be = base.end();
  auto q = p.begin(), qe = p.end();
  for (; b != be; ++b, ++q) if (q == qe || *q != *b) return false;
  return q != qe;
}

RemoveOutcome LauncherCore::remove(const std::string& game_id, bool keep_saves, bool confirmed) {
  namespace fs = std::filesystem;
  RemoveOutcome o;
  o.keep_saves = keep_saves;
  if (!open_) { o.error = "library is not open"; return o; }
  if (!valid_game_id(game_id)) { o.error = "not a valid game id"; return o; }
  if (!db_.get_game(game_id)) { o.error = "no such game id in the library"; return o; }
  o.found = true;
  std::error_code ec;
  const fs::path root = fs::canonical(root_, ec);
  if (ec) { o.refused = true; o.error = "library directory cannot be resolved"; return o; }
  const fs::path games_dir = root / "games", jail_dir = root / "jail";
  // Install tree: only as recorded, and only a direct child of <root>/games.
  fs::path install;
  if (auto i = db_.get_installation(game_id)) {
    o.install_path = i->path;
    std::error_code e1, e2, e3;
    const auto sst = fs::symlink_status(i->path, e1);
    const bool exists = !e1 && fs::exists(sst);
    if (exists && fs::is_symlink(sst)) { o.refused = true; o.error = "install path is a symlink: refusing to delete"; return o; }
    // Containment is judged on the resolved path whether or not it exists, so
    // a hostile row is refused rather than silently treated as "already gone".
    install = fs::weakly_canonical(i->path, e3);
    const fs::path gc = fs::canonical(games_dir, e2);
    const bool contained = !e3 && !e2 && install.parent_path() == gc && !install.filename().empty();
    if (!contained && (exists || !e2)) {
      o.refused = true; o.error = "install path is outside the library's games directory: refusing to delete"; return o;
    }
    o.install_exists = exists;
  }
  fs::path jail;
  {
    const fs::path jp = jail_dir / game_id;
    std::error_code e1;
    const auto sst = fs::symlink_status(jp, e1);
    o.jail_path = jp.string();
    if (!e1 && fs::exists(sst)) {
      if (fs::is_symlink(sst)) { o.refused = true; o.error = "jail path is a symlink: refusing to delete"; return o; }
      std::error_code e2; const fs::path jc = fs::canonical(jp, e2);
      if (e2 || !is_inside(fs::canonical(jail_dir, e2), jc) || jc.parent_path() != fs::canonical(jail_dir, e2)) {
        o.refused = true; o.error = "jail path is outside the library's jail directory: refusing to delete"; return o;
      }
      jail = jc; o.jail_exists = true;
    }
  }
  if (!confirmed) { o.needs_confirmation = true; return o; }
  if (o.install_exists) {
    std::error_code e3; fs::remove_all(install, e3);
    if (e3) { o.error = "could not delete the install tree: " + e3.message(); return o; }
  }
  if (o.jail_exists && !keep_saves) {
    std::error_code e3; fs::remove_all(jail, e3);
    if (e3) { o.error = "could not delete the jail folder: " + e3.message(); return o; }
  }
  auto st = db_.remove_game(game_id);
  if (!st) { o.error = "files deleted but library records were not: " + st.error.message; return o; }
  o.removed = true;
  return o;
}

VerifyOutcome LauncherCore::verify(const std::string& game_id) {
  namespace fs = std::filesystem;
  VerifyOutcome o;
  if (!open_ || !db_.get_game(game_id)) return o;
  o.found = true;
  auto inst = db_.get_installation(game_id);
  if (!inst) { o.problems.push_back("not installed"); return o; }
  std::error_code ec;
  if (!fs::is_directory(inst->path, ec)) o.problems.push_back("install directory is missing: " + inst->path);
  else if (inst->executable.empty()) o.problems.push_back("installation has no entry point");
  else {
    const fs::path base = fs::weakly_canonical(inst->path, ec);
    const fs::path exe = fs::weakly_canonical(fs::path(inst->path) / inst->executable, ec);
    if (ec || !is_inside(base, exe)) o.problems.push_back("entry point resolves outside the install directory");
    else if (!fs::is_regular_file(exe, ec)) o.problems.push_back("entry point file is missing: " + inst->executable);
    else {
      auto pr = loader::parse_elf(exe.string());
      if (!pr.ok) o.problems.push_back("entry point is not a valid ELF: " + pr.error);
      else if (!loader::is_x86_64(pr.info)) o.problems.push_back("entry point is not an x86-64 ELF");
      else if (!loader::is_executable_type(pr.info)) o.problems.push_back("entry point is not an executable ELF type");
    }
  }
  o.ok = o.problems.empty();
  if (!o.ok && inst->integrity != "failed") {
    auto upd = *inst; upd.integrity = "failed";
    o.marked_failed = bool(db_.set_installation(upd));
  }
  return o;
}

// Bytes under a path, never following symlinks.
static uint64_t tree_bytes(const std::filesystem::path& p) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const auto st = fs::symlink_status(p, ec);
  if (ec || fs::is_symlink(st)) return 0;
  if (fs::is_regular_file(st)) { auto n = fs::file_size(p, ec); return ec ? 0 : n; }
  if (!fs::is_directory(st)) return 0;
  uint64_t total = 0;
  for (fs::recursive_directory_iterator it(p, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    const auto s2 = it->symlink_status(e2);
    if (!e2 && fs::is_regular_file(s2)) { auto n = it->file_size(e2); if (!e2) total += n; }
  }
  return total;
}

static std::string entry_kind(const std::filesystem::path& p) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const auto st = fs::symlink_status(p, ec);
  if (fs::is_symlink(st)) return "symlink";
  if (fs::is_directory(st)) return "dir";
  return "file";
}

// Resolved install paths recorded in the library, as canonical strings where possible.
static std::vector<std::string> recorded_installs(library::GameDb& db) {
  std::vector<std::string> out;
  for (const auto& g : db.list_games())
    if (auto i = db.get_installation(g.game_id)) {
      std::error_code ec;
      out.push_back(std::filesystem::weakly_canonical(i->path, ec).string());
    }
  return out;
}

DoctorReport LauncherCore::doctor() {
  namespace fs = std::filesystem;
  DoctorReport r;
  if (!open_) { r.error = "library is not open"; return r; }
  std::error_code ec;
  const fs::path root = fs::canonical(root_, ec);
  if (ec) { r.error = "library directory cannot be resolved"; return r; }
  r.ok = true;
  const fs::path games_dir = root / "games", jail_dir = root / "jail";
  std::error_code eg;
  const fs::path games_c = fs::exists(games_dir) ? fs::canonical(games_dir, eg) : games_dir;
  auto games = db_.list_games();
  r.games = static_cast<int>(games.size());
  std::vector<std::string> rec = recorded_installs(db_);
  for (const auto& g : games) {
    DoctorUsage u; u.game_id = g.game_id;
    if (auto i = db_.get_installation(g.game_id)) {
      std::error_code e1;
      const fs::path rp = fs::weakly_canonical(i->path, e1);
      const bool contained = !e1 && rp.parent_path() == games_c && !rp.filename().empty();
      if (!contained) r.issues.push_back({"install-outside-library", g.game_id, "recorded install path does not resolve to a direct child of the library's games directory: " + i->path});
      else if (!fs::is_directory(rp, e1)) r.issues.push_back({"install-missing", g.game_id, "install directory is missing: " + i->path});
      else u.install_bytes = tree_bytes(rp);
      if (i->integrity == "failed") r.issues.push_back({"integrity-failed", g.game_id, "integrity is marked failed (the game is not runnable)"});
    }
    u.saves_bytes = tree_bytes(jail_dir / g.game_id);
    r.usage.push_back(u);
  }
  std::sort(r.usage.begin(), r.usage.end(), [](const DoctorUsage& a, const DoctorUsage& b) { return a.game_id < b.game_id; });
  std::sort(r.issues.begin(), r.issues.end(), [](const DoctorIssue& a, const DoctorIssue& b) { return a.subject != b.subject ? a.subject < b.subject : a.kind < b.kind; });
  std::error_code e2;
  if (fs::is_directory(games_dir, e2))
    for (fs::directory_iterator it(games_dir, e2), end; !e2 && it != end; it.increment(e2)) {
      const fs::path p = it->path();
      std::error_code e3;
      const fs::path rp = (fs::is_symlink(fs::symlink_status(p, e3)) ? games_c / p.filename() : fs::weakly_canonical(p, e3));
      if (std::find(rec.begin(), rec.end(), rp.string()) != rec.end()) continue;
      DoctorOrphan o; o.name = p.filename().string(); o.kind = entry_kind(p); o.bytes = tree_bytes(p);
      r.orphan_installs.push_back(o);
    }
  if (fs::is_directory(jail_dir, e2))
    for (fs::directory_iterator it(jail_dir, e2), end; !e2 && it != end; it.increment(e2)) {
      const std::string name = it->path().filename().string();
      if (db_.get_game(name)) continue;
      DoctorOrphan o; o.name = name; o.kind = entry_kind(it->path()); o.bytes = tree_bytes(it->path());
      r.orphan_jails.push_back(o);
    }
  auto byname = [](const DoctorOrphan& a, const DoctorOrphan& b) { return a.name < b.name; };
  std::sort(r.orphan_installs.begin(), r.orphan_installs.end(), byname);
  std::sort(r.orphan_jails.begin(), r.orphan_jails.end(), byname);
  return r;
}

CleanOutcome LauncherCore::clean_orphans(bool confirmed) {
  namespace fs = std::filesystem;
  CleanOutcome o;
  auto rep = doctor();  // recomputed now; nothing from an earlier report is trusted
  if (!rep.ok) { o.error = rep.error; return o; }
  o.ok = true;
  std::error_code ec;
  const fs::path games_dir = fs::canonical(fs::path(root_), ec) / "games";
  std::error_code eg;
  const fs::path gc = fs::exists(games_dir) ? fs::canonical(games_dir, eg) : games_dir;
  for (const auto& orph : rep.orphan_installs) {
    if (orph.kind != "dir") { ++o.left_alone; continue; }
    CleanItem it; it.name = orph.name; it.bytes = orph.bytes;
    const fs::path p = gc / orph.name;
    std::error_code e1;
    const auto st = fs::symlink_status(p, e1);
    const fs::path rp = fs::weakly_canonical(p, e1);
    if (e1 || fs::is_symlink(st) || !fs::is_directory(st) || rp.parent_path() != gc || rp.filename().empty() || orph.name.empty() || orph.name == "." || orph.name == "..") {
      it.error = "refused: not a plain directory directly under the games directory";
      o.items.push_back(it); continue;
    }
    if (confirmed) {
      std::error_code e2; fs::remove_all(rp, e2);
      if (e2) it.error = "could not delete: " + e2.message(); else it.removed = true;
    }
    o.items.push_back(it);
  }
  if (!confirmed && !o.items.empty()) o.needs_confirmation = true;
  return o;
}

bool safe_archive_name(const std::string& n) {
  if (n.empty() || n.size() > 100 || n[0] == '/' || n.back() == '/') return false;
  if (n.find('\0') != std::string::npos || n.find('\\') != std::string::npos) return false;
  size_t start = 0;
  while (start <= n.size()) {
    size_t end = n.find('/', start);
    if (end == std::string::npos) end = n.size();
    const std::string c = n.substr(start, end - start);
    if (c.empty() || c == "." || c == "..") return false;
    start = end + 1;
  }
  return true;
}

SavesOutcome LauncherCore::export_saves(const std::string& game_id, const std::string& out_path) {
  namespace fs = std::filesystem;
  SavesOutcome o;
  if (!open_) { o.error = "library is not open"; return o; }
  if (!valid_game_id(game_id)) { o.error = "not a valid game id"; return o; }
  if (!db_.get_game(game_id)) { o.error = "no such game id in the library"; return o; }
  if (out_path.empty()) { o.error = "no output path"; return o; }
  std::error_code ec;
  const fs::path jail = fs::path(root_) / "jail" / game_id;
  const auto jst = fs::symlink_status(jail, ec);
  if (ec || !fs::exists(jst)) { o.error = "this game has no saves folder"; return o; }
  if (fs::is_symlink(jst) || !fs::is_directory(jst)) { o.error = "saves folder is not a plain directory: refusing"; return o; }
  if (fs::exists(fs::symlink_status(out_path, ec))) { o.error = "output file already exists: refusing to overwrite"; return o; }
  const fs::path outp = fs::weakly_canonical(out_path, ec);
  const fs::path jailc = fs::canonical(jail, ec);
  if (is_inside(jailc, outp)) { o.error = "output file is inside the saves folder being exported"; return o; }
  if (!fs::is_directory(outp.parent_path(), ec)) { o.error = "output directory does not exist"; return o; }
  std::vector<pkg::TarEntry> entries;
  std::vector<std::string> names;
  for (fs::recursive_directory_iterator it(jailc, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    const auto st = it->symlink_status(e2);
    if (e2 || !fs::is_regular_file(st)) { if (!fs::is_directory(st)) ++o.skipped; continue; }
    const std::string rel = fs::relative(it->path(), jailc, e2).generic_string();
    if (e2 || !safe_archive_name(rel)) { o.error = "a saves path cannot be archived (name over 100 bytes or unsafe): " + rel; return o; }
    const uint64_t sz = it->file_size(e2);
    if (e2) { o.error = "cannot read file size: " + rel; return o; }
    if (entries.size() >= kSavesMaxFiles || o.bytes + sz > kSavesMaxBytes) { o.error = "saves folder is over the export limit (10000 files / 64 MiB)"; return o; }
    std::ifstream f(it->path(), std::ios::binary);
    pkg::TarEntry te; te.name = rel;
    te.data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (te.data.size() != sz) { o.error = "file changed while reading: " + rel; return o; }
    o.bytes += sz; entries.push_back(std::move(te));
  }
  if (ec) { o.error = "cannot read the saves folder"; return o; }
  std::sort(entries.begin(), entries.end(), [](const pkg::TarEntry& a, const pkg::TarEntry& b) { return a.name < b.name; });
  auto st = pkg::tar_write(outp.string(), entries);
  if (!st) { o.error = st.error.message; std::error_code e3; fs::remove(outp, e3); return o; }
  o.files = static_cast<int>(entries.size());
  o.ok = true;
  return o;
}

SavesOutcome LauncherCore::restore_saves(const std::string& game_id, const std::string& in_path, bool overwrite) {
  namespace fs = std::filesystem;
  SavesOutcome o;
  if (!open_) { o.error = "library is not open"; return o; }
  if (!valid_game_id(game_id)) { o.error = "not a valid game id"; return o; }
  if (!db_.get_game(game_id)) { o.error = "no such game id in the library"; return o; }
  std::error_code ec;
  if (in_path.empty() || !fs::is_regular_file(in_path, ec)) { o.error = "archive is not a readable file"; return o; }
  if (fs::file_size(in_path, ec) > kSavesMaxBytes + 512ull * (kSavesMaxFiles * 2 + 2) || ec) { o.error = "archive is over the restore limit"; return o; }
  std::vector<pkg::TarEntry> entries;
  auto st = pkg::tar_read(in_path, entries);
  if (!st) { o.error = "not a valid archive: " + st.error.message; return o; }
  if (entries.size() > kSavesMaxFiles) { o.error = "archive has too many files (limit 10000)"; return o; }
  // Preflight: every name, size and target is checked before anything is written.
  uint64_t total = 0;
  std::vector<std::string> seen;
  for (const auto& e : entries) {
    if (!safe_archive_name(e.name)) { o.error = "unsafe name in archive: " + e.name; return o; }
    total += e.data.size();
    if (total > kSavesMaxBytes) { o.error = "archive contents are over the restore limit (64 MiB)"; return o; }
    seen.push_back(e.name);
  }
  std::sort(seen.begin(), seen.end());
  for (size_t i = 1; i < seen.size(); ++i)
    if (seen[i] == seen[i - 1]) { o.error = "duplicate name in archive: " + seen[i]; return o; }
  for (size_t i = 0; i < seen.size(); ++i)  // "a" and "a/b" cannot both be files
    for (size_t j = i + 1; j < seen.size() && seen[j].compare(0, seen[i].size() + 1, seen[i] + "/") == 0; ++j)
      { o.error = "archive has a file that is also used as a directory: " + seen[i]; return o; }
  const fs::path jail_root = fs::path(root_) / "jail";
  const fs::path jail = jail_root / game_id;
  {
    const auto jst = fs::symlink_status(jail, ec);
    if (!ec && fs::exists(jst) && (fs::is_symlink(jst) || !fs::is_directory(jst))) { o.error = "saves folder is not a plain directory: refusing"; return o; }
    const auto rst = fs::symlink_status(jail_root, ec);
    if (!ec && fs::exists(rst) && (fs::is_symlink(rst) || !fs::is_directory(rst))) { o.error = "library jail directory is not a plain directory: refusing"; return o; }
  }
  for (const auto& e : entries) {
    fs::path cur = jail;
    size_t start = 0;
    while (true) {  // every existing component below the jail must be a plain dir (or the final file)
      size_t end = e.name.find('/', start);
      const bool last = end == std::string::npos;
      cur /= e.name.substr(start, last ? std::string::npos : end - start);
      std::error_code e1;
      const auto cst = fs::symlink_status(cur, e1);
      if (!e1 && fs::exists(cst)) {
        if (fs::is_symlink(cst)) { o.error = "a symlink is in the way: " + e.name; return o; }
        if (last && !fs::is_regular_file(cst)) { o.error = "target exists and is not a plain file: " + e.name; return o; }
        if (!last && !fs::is_directory(cst)) { o.error = "a file is in the way of a folder: " + e.name; return o; }
      }
      if (last) break;
      start = end + 1;
    }
  }
  // Write.
  std::error_code e0;
  fs::create_directories(jail, e0);
  if (e0) { o.error = "cannot create the saves folder: " + e0.message(); return o; }
  for (const auto& e : entries) {
    const fs::path target = jail / e.name;
    std::error_code e1;
    if (fs::exists(fs::symlink_status(target, e1)) && !overwrite) { ++o.skipped; continue; }
    fs::create_directories(target.parent_path(), e1);
    if (e1) { o.error = "cannot create folder for " + e.name + " (earlier files may already be restored)"; return o; }
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(e.data.data()), static_cast<std::streamsize>(e.data.size()));
    if (!out) { o.error = "write failed: " + e.name + " (earlier files may already be restored)"; return o; }
    ++o.files; o.bytes += e.data.size();
  }
  o.ok = true;
  return o;
}

const std::vector<std::string>& compat_states() {
  static const std::vector<std::string> s = {
      "Untested", "Launches Only", "Playable With Issues", "Verified Playable",
      "Unsupported", "Blocked by Missing Dependency"};
  return s;
}

bool valid_compat_state(const std::string& s) {
  const auto& v = compat_states();
  return std::find(v.begin(), v.end(), s) != v.end();
}

Status LauncherCore::set_compat(const std::string& game_id, const std::string& state,
                                const std::string& note) {
  if (!open_) return Status::Fail(2, "launcher", "library is not open");
  if (!db_.get_game(game_id)) return Status::Fail(3, "launcher", "no such game id in the library");
  if (!valid_compat_state(state)) return Status::Fail(4, "launcher", "unknown compatibility state: " + state);
  if (note.size() > 500) return Status::Fail(5, "launcher", "note is longer than 500 bytes");
  for (unsigned char c : note)
    if (c < 0x20 || c == 0x7f) return Status::Fail(6, "launcher", "note contains a control character");
  return db_.set_compatibility_human({game_id, state, note});
}

bool LauncherCore::history(const std::string& game_id, int limit,
                           std::vector<library::LaunchRow>* out) {
  if (!open_ || !out || !db_.get_game(game_id)) return false;
  *out = db_.launch_history(game_id, limit);
  return true;
}

Status LauncherCore::scan(const std::string& dir, ScanReport* out) {
  namespace fs = std::filesystem;
  if (!out) return Status::Fail(2, "launcher", "no output");
  *out = ScanReport{};
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return Status::Fail(7, "launcher", "not a directory: " + dir);
  std::vector<fs::path> files;
  size_t seen = 0;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (++seen > kScanMaxEntries) { out->truncated = true; break; }
    std::error_code e2;
    const auto st = fs::symlink_status(it->path(), e2);
    if (e2 || !fs::is_regular_file(st)) continue;  // dirs, symlinks, devices: not followed
    files.push_back(it->path());
  }
  if (ec) return Status::Fail(8, "launcher", "cannot read directory: " + dir);
  std::sort(files.begin(), files.end());
  const prx::PrxRegistry reg;
  constexpr uintmax_t kMaxBytes = 512ull * 1024 * 1024;
  for (const auto& p : files) {
    ScanItem it;
    it.file = p.filename().string();
    const bool pkg = p.extension() == ".pnrgame";
    bool elf = false;
    if (!pkg) {
      std::ifstream f(p, std::ios::binary);
      char m[4] = {0, 0, 0, 0};
      f.read(m, 4);
      elf = f.gcount() == 4 && std::memcmp(m, "\x7f" "ELF", 4) == 0;
    }
    if (!pkg && !elf) { ++out->skipped; continue; }
    std::error_code e3;
    const auto sz = fs::file_size(p, e3);
    if (pkg) {
      it.kind = "pnrgame";
      if (e3 || sz > kMaxBytes) { it.error = "file too large to inspect"; }
      else {
        auto r = pkg::inspect_package(p.string());
        it.ok = bool(r.status);
        if (!it.ok) it.error = r.status.error.message;
        else {
          it.game_id = r.manifest.id; it.title = r.manifest.title; it.version = r.manifest.version;
          it.in_library = bool(db_.get_game(it.game_id));
        }
      }
    } else {
      it.kind = "elf";
      auto pr = loader::parse_elf(p.string());
      if (!pr.ok) it.error = pr.error;
      else if (!loader::is_x86_64(pr.info)) it.error = "not an x86-64 ELF";
      else if (!loader::is_executable_type(pr.info)) it.error = "ELF is not an executable type";
      else {
        loader::ImportResolution ir;
        auto st = loader::resolve_imports(p.string(), reg, &ir);
        if (!st) it.error = "import table: " + st.error.message;
        else {
          it.ok = true; it.has_imports = true;
          it.host = ir.host; it.prx_implemented = ir.prx_implemented; it.prx_declared = ir.prx_declared;
          it.unknown = ir.unknown; it.pnr_extension = ir.pnr_extension;
        }
      }
    }
    out->items.push_back(it);
  }
  return Status::Ok();
}

LaunchOutcome LauncherCore::launch(const std::string& game_id,
                                   const std::vector<std::string>& args,
                                   const std::vector<std::string>& env) {
  LaunchOutcome o;
  GameEntry e;
  if (!get(game_id, &e)) { o.error = "no such game id in the library"; return o; }
  o.found = true;
  if (!e.runnable) { o.error = "not runnable: " + e.not_runnable_reason; return o; }
  const std::string exe = e.install_path + "/" + e.executable;
  if (!std::filesystem::exists(exe)) { o.error = "entry point missing on disk: " + exe; return o; }
  std::vector<std::string> full = env;
  auto has = [&](const char* key) {
    const std::string k = std::string(key) + "=";
    for (const auto& v : env) if (v.compare(0, k.size(), k) == 0) return true;
    return false;
  };
  if (!has("PNR_JAIL_ROOT")) full.push_back("PNR_JAIL_ROOT=" + jail_root_ + "/" + game_id);
  if (!has("LD_LIBRARY_PATH")) {
    std::error_code ec;
    auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) full.push_back("LD_LIBRARY_PATH=" + self.parent_path().string());
  }
  loader::NativeLoader loader;
  auto r = loader.launch(exe, args, full);
  o.attempted = r.started;
  o.finished = r.finished;
  o.signaled = r.signaled;
  o.exit_code = r.exit_code;
  o.wall_seconds = r.wall_seconds;
  if (!r.validated || !r.started) { o.error = r.error.empty() ? "launch failed before start" : r.error; return o; }
  if (r.signaled) o.error = "game was killed by a signal";
  else if (!r.finished) o.error = "game did not finish";
  else if (r.exit_code != 0) o.error = "game exited with a non-zero code";
  // A started game is part of the history even when it crashed.
  o.recorded = bool(db_.record_launch(game_id, r.signaled ? -1 : r.exit_code, r.wall_seconds));
  if (o.error.empty() && e.compatibility == "Untested" && !db_.compat_is_human_set(game_id)) {
    o.compat_advanced = bool(db_.set_compatibility(
        {game_id, "Launches Only", "launched natively, exit 0; gameplay not verified"}));
  }
  return o;
}

std::string json_escape(const std::string& s) {
  std::string r;
  for (unsigned char c : s) {
    switch (c) {
      case '"': r += "\\\""; break;
      case '\\': r += "\\\\"; break;
      case '\n': r += "\\n"; break;
      case '\r': r += "\\r"; break;
      case '\t': r += "\\t"; break;
      default:
        if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); r += b; }
        else r += static_cast<char>(c);
    }
  }
  return r;
}

static std::string q(const std::string& s) { return "\"" + json_escape(s) + "\""; }

std::string render_entry_json(const GameEntry& g) {
  std::string j = "{";
  j += "\"game_id\":" + q(g.game_id) + ",\"title\":" + q(g.title) + ",\"version\":" + q(g.version);
  j += ",\"platform\":" + q(g.platform) + ",\"compatibility\":" + q(g.compatibility);
  j += ",\"compatibility_notes\":" + q(g.compatibility_notes);
  j += std::string(",\"installed\":") + (g.installed ? "true" : "false");
  j += ",\"install_path\":" + q(g.install_path) + ",\"executable\":" + q(g.executable);
  j += ",\"integrity\":" + q(g.integrity);
  j += std::string(",\"runnable\":") + (g.runnable ? "true" : "false");
  j += ",\"not_runnable_reason\":" + q(g.not_runnable_reason);
  j += std::string(",\"compatibility_human_set\":") + (g.compat_human_set ? "true" : "false");
  j += ",\"launch_count\":" + std::to_string(g.launch_count) + "}";
  return j;
}

std::string render_list_json(const std::vector<GameEntry>& games) {
  std::string j = "{\"games\":[";
  for (size_t i = 0; i < games.size(); ++i) { if (i) j += ","; j += render_entry_json(games[i]); }
  j += "],\"count\":" + std::to_string(games.size()) + "}";
  return j;
}

std::string render_entry_text(const GameEntry& g) {
  std::string t = g.title + "  v" + g.version + "\n";
  t += "  id:            " + g.game_id + "\n";
  t += "  compatibility: " + g.compatibility + "\n";
  if (g.compat_human_set) t += "  set by:        a person (automatic launches will not change it)\n";
  if (!g.compatibility_notes.empty()) t += "  notes:         " + g.compatibility_notes + "\n";
  t += "  installed:     " + std::string(g.installed ? g.install_path : "no") + "\n";
  t += "  runnable:      " + std::string(g.runnable ? "yes" : "no (" + g.not_runnable_reason + ")") + "\n";
  t += "  launches:      " + std::to_string(g.launch_count) + "\n";
  return t;
}

std::string render_list_text(const std::vector<GameEntry>& games) {
  if (games.empty()) return "library is empty (import a .pnrgame first)\n";
  std::string t;
  for (const auto& g : games)
    t += g.title + "  v" + g.version + "  [" + g.compatibility + "]  " +
         (g.runnable ? "runnable" : "not runnable: " + g.not_runnable_reason) +
         "  id=" + g.game_id + "\n";
  return t;
}

static std::string when(double at) {
  char b[32]; std::time_t t = static_cast<std::time_t>(at);
  std::tm tm{}; gmtime_r(&t, &tm);
  std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%SZ", &tm);
  return b;
}

std::string render_history_text(const GameEntry& g, const std::vector<library::LaunchRow>& rows) {
  std::string t = g.title + "  (" + g.game_id + ")  total launches: " + std::to_string(g.launch_count) + "\n";
  if (rows.empty()) return t + "  no launches recorded\n";
  for (const auto& r : rows) {
    char w[32]; std::snprintf(w, sizeof w, "%.3f", r.wall_seconds);
    t += "  " + when(r.at) + "  " + (r.exit_code == -1 ? std::string("killed by signal") : "exit " + std::to_string(r.exit_code)) + "  " + w + "s\n";
  }
  return t;
}

std::string render_history_json(const GameEntry& g, const std::vector<library::LaunchRow>& rows) {
  std::string j = "{\"game_id\":" + q(g.game_id) + ",\"launch_count\":" + std::to_string(g.launch_count) + ",\"launches\":[";
  for (size_t i = 0; i < rows.size(); ++i) {
    char w[32]; std::snprintf(w, sizeof w, "%.3f", rows[i].wall_seconds);
    if (i) j += ",";
    j += "{\"at\":" + q(when(rows[i].at)) + ",\"exit_code\":" + std::to_string(rows[i].exit_code) + ",\"wall_seconds\":" + w + "}";
  }
  return j + "]}";
}

std::string render_scan_text(const ScanReport& r) {
  std::string t;
  for (const auto& i : r.items) {
    t += i.file + "  [" + i.kind + "]  ";
    if (!i.ok) { t += "REJECTED: " + i.error + "\n"; continue; }
    if (i.kind == "pnrgame") {
      t += "valid package id=" + i.game_id + " v" + i.version + (i.in_library ? "  already in library" : "  not in library") + "\n";
    } else {
      t += "valid x86-64 ELF; imports: " + std::to_string(i.host) + " host, " + std::to_string(i.prx_implemented) +
           " prx-implemented, " + std::to_string(i.prx_declared) + " prx-declared-only, " + std::to_string(i.unknown) +
           " unknown, " + std::to_string(i.pnr_extension) + " pnr-extension\n";
    }
  }
  t += std::to_string(r.items.size()) + " inspected, " + std::to_string(r.skipped) + " other files skipped";
  if (r.truncated) t += " (stopped at " + std::to_string(kScanMaxEntries) + " entries)";
  t += "\nscan is read-only classification: nothing imported, extracted or run\n";
  return t;
}

std::string render_scan_json(const ScanReport& r) {
  std::string j = "{\"items\":[";
  for (size_t n = 0; n < r.items.size(); ++n) {
    const auto& i = r.items[n];
    if (n) j += ",";
    j += "{\"file\":" + q(i.file) + ",\"kind\":" + q(i.kind) + std::string(",\"ok\":") + (i.ok ? "true" : "false") + ",\"error\":" + q(i.error);
    if (i.kind == "pnrgame") {
      j += ",\"game_id\":" + q(i.game_id) + ",\"title\":" + q(i.title) + ",\"version\":" + q(i.version) +
           std::string(",\"in_library\":") + (i.in_library ? "true" : "false");
    } else if (i.has_imports) {
      j += ",\"imports\":{\"host\":" + std::to_string(i.host) + ",\"prx_implemented\":" + std::to_string(i.prx_implemented) +
           ",\"prx_declared\":" + std::to_string(i.prx_declared) + ",\"unknown\":" + std::to_string(i.unknown) +
           ",\"pnr_extension\":" + std::to_string(i.pnr_extension) + "}";
    }
    j += "}";
  }
  j += "],\"inspected\":" + std::to_string(r.items.size()) + ",\"skipped\":" + std::to_string(r.skipped) +
       std::string(",\"truncated\":") + (r.truncated ? "true" : "false") + "}";
  return j;
}

std::string render_import_text(const ImportOutcome& o) {
  if (!o.ok) return "IMPORT REFUSED: " + o.error + "\n";
  std::string t = "IMPORT OK: " + o.game_id + " v" + o.version + " (" + o.title + ")\n  installed at " + o.install_path + "\n";
  if (o.kind == "fresh") t += "  new install\n";
  else if (o.kind == "reinstall") t += "  reinstall of the same version (files overwritten in place)\n";
  else t += "  replaces v" + o.previous_version + "; the old install tree is left at " + o.previous_path + "\n";
  return t + "  compatibility is unchanged by import (new games start Untested; a person-set state is kept)\n";
}

std::string render_import_json(const ImportOutcome& o) {
  std::string j = std::string("{\"ok\":") + (o.ok ? "true" : "false") + ",\"error\":" + q(o.error) + ",\"game_id\":" + q(o.game_id) +
                  ",\"title\":" + q(o.title) + ",\"version\":" + q(o.version) + ",\"install_path\":" + q(o.install_path) +
                  ",\"kind\":" + q(o.kind) + ",\"previous_version\":" + q(o.previous_version) + ",\"previous_path\":" + q(o.previous_path) + "}";
  return j;
}

std::string render_remove_text(const std::string& id, const RemoveOutcome& o) {
  if (!o.found) return "REMOVE NOT DONE: " + o.error + "\n";
  if (o.refused) return "REMOVE REFUSED: " + o.error + "\n  nothing was deleted\n";
  std::string t;
  t += std::string(o.removed ? "REMOVED " : o.needs_confirmation ? "WOULD REMOVE (add --yes to do it) " : "NOT REMOVED ") + id + "\n";
  t += "  install tree:  " + (o.install_path.empty() ? std::string("none recorded") : o.install_path + (o.install_exists ? "" : "  (already gone)")) + "\n";
  t += "  saves/jail:    " + o.jail_path + (o.jail_exists ? (o.keep_saves ? "  (kept: --keep-saves)" : "") : "  (none)") + "\n";
  t += "  library rows:  game, installation, compatibility and launch history\n";
  if (!o.error.empty()) t += "  error: " + o.error + "\n";
  return t;
}

std::string render_remove_json(const std::string& id, const RemoveOutcome& o) {
  auto b = [](bool v) { return v ? "true" : "false"; };
  return "{\"game_id\":" + q(id) + ",\"found\":" + b(o.found) + ",\"refused\":" + b(o.refused) +
         ",\"needs_confirmation\":" + b(o.needs_confirmation) + ",\"removed\":" + b(o.removed) +
         ",\"install_path\":" + q(o.install_path) + ",\"install_exists\":" + b(o.install_exists) +
         ",\"jail_path\":" + q(o.jail_path) + ",\"jail_exists\":" + b(o.jail_exists) +
         ",\"keep_saves\":" + b(o.keep_saves) + ",\"error\":" + q(o.error) + "}";
}

std::string render_verify_text(const std::string& id, const VerifyOutcome& o) {
  if (!o.found) return "VERIFY NOT DONE: no such game id in the library\n";
  if (o.ok) return "VERIFY OK: " + id + ": install directory present, entry point is a valid x86-64 ELF\n  payload checksums are not re-checked (the manifest is not kept after import)\n";
  std::string t = "VERIFY FAILED: " + id + "\n";
  for (const auto& p : o.problems) t += "  - " + p + "\n";
  if (o.marked_failed) t += "  integrity marked failed in the library (the game is no longer runnable)\n";
  return t;
}

std::string render_verify_json(const std::string& id, const VerifyOutcome& o) {
  std::string j = "{\"game_id\":" + q(id) + std::string(",\"found\":") + (o.found ? "true" : "false") + ",\"ok\":" + (o.ok ? "true" : "false") + ",\"problems\":[";
  for (size_t i = 0; i < o.problems.size(); ++i) { if (i) j += ","; j += q(o.problems[i]); }
  return j + "],\"marked_failed\":" + (o.marked_failed ? "true" : "false") + ",\"checksums_rechecked\":false}";
}

static std::string mib(uint64_t b) {
  char s[32];
  if (b < 1024) std::snprintf(s, sizeof s, "%llu B", (unsigned long long)b);
  else if (b < 1024 * 1024) std::snprintf(s, sizeof s, "%.1f KiB", b / 1024.0);
  else std::snprintf(s, sizeof s, "%.1f MiB", b / 1048576.0);
  return s;
}

std::string render_doctor_text(const DoctorReport& r) {
  if (!r.ok) return "DOCTOR NOT DONE: " + r.error + "\n";
  std::string t = "library readable, " + std::to_string(r.games) + " game(s)\n";
  if (r.issues.empty()) t += "no row problems found\n";
  for (const auto& i : r.issues) t += "ISSUE " + i.kind + "  " + i.subject + ": " + i.detail + "\n";
  for (const auto& o : r.orphan_installs) t += "ORPHAN INSTALL  games/" + o.name + "  [" + o.kind + ", " + mib(o.bytes) + "]  no library row points here\n";
  for (const auto& o : r.orphan_jails) t += "ORPHAN SAVES  jail/" + o.name + "  [" + o.kind + ", " + mib(o.bytes) + "]  no library row (kept saves from a removed game?)\n";
  for (const auto& u : r.usage) t += "usage  " + u.game_id + ": install " + mib(u.install_bytes) + ", saves " + mib(u.saves_bytes) + "\n";
  t += "doctor is read-only: nothing was changed\n";
  return t;
}

std::string render_doctor_json(const DoctorReport& r) {
  auto orph = [](const std::vector<DoctorOrphan>& v) {
    std::string j = "[";
    for (size_t i = 0; i < v.size(); ++i) { if (i) j += ","; j += "{\"name\":" + q(v[i].name) + ",\"kind\":" + q(v[i].kind) + ",\"bytes\":" + std::to_string(v[i].bytes) + "}"; }
    return j + "]";
  };
  std::string j = std::string("{\"ok\":") + (r.ok ? "true" : "false") + ",\"error\":" + q(r.error) + ",\"games\":" + std::to_string(r.games) + ",\"issues\":[";
  for (size_t i = 0; i < r.issues.size(); ++i) { if (i) j += ","; j += "{\"kind\":" + q(r.issues[i].kind) + ",\"game_id\":" + q(r.issues[i].subject) + ",\"detail\":" + q(r.issues[i].detail) + "}"; }
  j += "],\"orphan_installs\":" + orph(r.orphan_installs) + ",\"orphan_jails\":" + orph(r.orphan_jails) + ",\"usage\":[";
  for (size_t i = 0; i < r.usage.size(); ++i) { if (i) j += ","; j += "{\"game_id\":" + q(r.usage[i].game_id) + ",\"install_bytes\":" + std::to_string(r.usage[i].install_bytes) + ",\"saves_bytes\":" + std::to_string(r.usage[i].saves_bytes) + "}"; }
  return j + "]}";
}

std::string render_clean_text(const CleanOutcome& o) {
  if (!o.ok) return "CLEAN NOT DONE: " + o.error + "\n";
  if (o.items.empty()) return "nothing to clean: no orphan install directories" + std::string(o.left_alone ? " (" + std::to_string(o.left_alone) + " orphan file(s)/symlink(s) left alone)" : "") + "\n";
  std::string t = o.needs_confirmation ? "WOULD DELETE these orphan install directories (add --yes to do it):\n" : "CLEAN RESULT:\n";
  for (const auto& i : o.items) t += "  games/" + i.name + "  " + mib(i.bytes) + (i.removed ? "  DELETED" : i.error.empty() ? "" : "  " + i.error) + "\n";
  if (o.left_alone) t += "  " + std::to_string(o.left_alone) + " orphan file(s)/symlink(s) left alone\n";
  t += "saves (jail folders) and library rows are never touched by clean\n";
  return t;
}

std::string render_clean_json(const CleanOutcome& o) {
  std::string j = std::string("{\"ok\":") + (o.ok ? "true" : "false") + ",\"error\":" + q(o.error) + std::string(",\"needs_confirmation\":") + (o.needs_confirmation ? "true" : "false") + ",\"left_alone\":" + std::to_string(o.left_alone) + ",\"items\":[";
  for (size_t i = 0; i < o.items.size(); ++i) { if (i) j += ","; j += "{\"name\":" + q(o.items[i].name) + ",\"bytes\":" + std::to_string(o.items[i].bytes) + std::string(",\"removed\":") + (o.items[i].removed ? "true" : "false") + ",\"error\":" + q(o.items[i].error) + "}"; }
  return j + "]}";
}

std::string render_saves_text(const char* verb, const SavesOutcome& o) {
  const std::string v = verb;
  if (!o.ok) return v + " REFUSED: " + o.error + "\n";
  std::string t = (v == "export" ? "EXPORT OK: " : "RESTORE OK: ") + std::to_string(o.files) + " file(s), " + mib(o.bytes);
  if (o.skipped) t += v == "export" ? ", " + std::to_string(o.skipped) + " symlink/special file(s) not followed" : ", " + std::to_string(o.skipped) + " existing file(s) left alone (use --overwrite)";
  return t + "\n";
}

std::string render_saves_json(const char* verb, const SavesOutcome& o) {
  return std::string("{\"action\":") + q(verb) + ",\"ok\":" + (o.ok ? "true" : "false") + ",\"error\":" + q(o.error) + ",\"files\":" + std::to_string(o.files) +
         ",\"bytes\":" + std::to_string(o.bytes) + ",\"skipped\":" + std::to_string(o.skipped) + "}";
}

std::string render_outcome_json(const std::string& game_id, const LaunchOutcome& o) {
  std::string j = "{\"game_id\":" + q(game_id);
  j += std::string(",\"found\":") + (o.found ? "true" : "false");
  j += std::string(",\"attempted\":") + (o.attempted ? "true" : "false");
  j += std::string(",\"finished\":") + (o.finished ? "true" : "false");
  j += std::string(",\"signaled\":") + (o.signaled ? "true" : "false");
  j += ",\"exit_code\":" + std::to_string(o.exit_code);
  char w[32]; std::snprintf(w, sizeof w, "%.3f", o.wall_seconds);
  j += std::string(",\"wall_seconds\":") + w;
  j += std::string(",\"recorded\":") + (o.recorded ? "true" : "false");
  j += std::string(",\"compat_advanced\":") + (o.compat_advanced ? "true" : "false");
  j += ",\"error\":" + q(o.error) + "}";
  return j;
}

}  // namespace pnr::launcher
