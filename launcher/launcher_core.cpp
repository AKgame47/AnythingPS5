// PNR launcher core implementation, GPL-2.0. See launcher_core.hpp.
#include "launcher/launcher_core.hpp"

#include <cstdio>
#include <filesystem>

#include "loader/native_loader.hpp"

namespace pnr::launcher {

Status LauncherCore::open(const std::string& db_path) {
  if (open_) return Status::Ok();
  auto st = db_.open(db_path);
  if (st) {
    open_ = true;
    auto parent = std::filesystem::path(db_path).parent_path();
    jail_root_ = (parent.empty() ? std::filesystem::path(".") : parent).string() + "/jail";
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
  if (o.error.empty() && e.compatibility == "Untested") {
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
