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
