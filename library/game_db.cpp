#include <cstring>
#include "library/game_db.hpp"

#include "library/sqlite3_shim.h"

#include <ctime>

#include <filesystem>

namespace pnr::library {

struct GameDb::Impl {
  sqlite3* h = nullptr;
};

namespace {
Status sql_status(int rc, sqlite3* h, const char* what) {
  if (rc == SQLITE_OK || rc == SQLITE_DONE) return Status::Ok();
  return Status::Fail(rc, "library.db",
                      std::string(what) + ": " +
                          (h ? sqlite3_errmsg(h) : "no db"));
}
}  // namespace

Status GameDb::exec(const char* sql, const char* what) {
  char* err = nullptr;
  int rc = sqlite3_exec(db_->h, sql, nullptr, nullptr, &err);
  if (rc != SQLITE_OK) {
    std::string msg = err ? err : "unknown";
    sqlite3_free(err);
    return Status::Fail(rc, "library.db", std::string(what) + ": " + msg);
  }
  return Status::Ok();
}

// Older databases predate the executable column; add it once.
static Status migrate_installations(sqlite3* h) {
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(h, "PRAGMA table_info(installations)", -1, &st,
                         nullptr) != SQLITE_OK)
    return Status::Fail(90, "library.db", "schema probe failed");
  bool has_exec = false;
  while (sqlite3_step(st) == SQLITE_ROW) {
    const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    if (n && std::strcmp(n, "executable") == 0) has_exec = true;
  }
  sqlite3_finalize(st);
  if (has_exec) return Status::Ok();
  sqlite3_stmt* a = nullptr;
  if (sqlite3_prepare_v2(
          h, "ALTER TABLE installations ADD COLUMN executable TEXT NOT NULL DEFAULT ''",
          -1, &a, nullptr) != SQLITE_OK)
    return Status::Fail(91, "library.db", "schema migration failed");
  const int rc = sqlite3_step(a);
  sqlite3_finalize(a);
  if (rc != SQLITE_DONE)
    return Status::Fail(92, "library.db", "schema migration step failed");
  return Status::Ok();
}

// Older databases predate the human_set mark; add it once (default 0 = automatic).
static Status migrate_compatibility(sqlite3* h) {
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(h, "PRAGMA table_info(compatibility)", -1, &st, nullptr) != SQLITE_OK)
    return Status::Fail(93, "library.db", "compat schema probe failed");
  bool has = false;
  while (sqlite3_step(st) == SQLITE_ROW) {
    const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    if (n && std::strcmp(n, "human_set") == 0) has = true;
  }
  sqlite3_finalize(st);
  if (has) return Status::Ok();
  sqlite3_stmt* a = nullptr;
  if (sqlite3_prepare_v2(h, "ALTER TABLE compatibility ADD COLUMN human_set INTEGER NOT NULL DEFAULT 0",
                         -1, &a, nullptr) != SQLITE_OK)
    return Status::Fail(94, "library.db", "compat migration failed");
  const int rc = sqlite3_step(a);
  sqlite3_finalize(a);
  if (rc != SQLITE_DONE) return Status::Fail(95, "library.db", "compat migration step failed");
  return Status::Ok();
}

Status GameDb::open(const std::string& path) {
  close();
  // Ensure the parent directory exists so SQLite can create the file.
  std::error_code ec;
  auto parent = std::filesystem::path(path).parent_path();
  if (!parent.empty()) std::filesystem::create_directories(parent, ec);
  db_ = new Impl();
  int rc = sqlite3_open_v2(path.c_str(), &db_->h,
                           SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                               SQLITE_OPEN_FULLMUTEX,
                           nullptr);
  if (rc != SQLITE_OK)
    return Status::Fail(rc, "library.db", "open failed: " + path);
  {
    Status st = exec(
      "PRAGMA journal_mode=WAL;"
      "CREATE TABLE IF NOT EXISTS games("
      " game_id TEXT PRIMARY KEY, title TEXT NOT NULL,"
      " version TEXT NOT NULL, platform TEXT NOT NULL);"
      "CREATE TABLE IF NOT EXISTS installations("
      " game_id TEXT PRIMARY KEY, path TEXT NOT NULL,"
      " version TEXT NOT NULL, integrity TEXT NOT NULL,"
      " executable TEXT NOT NULL DEFAULT '');"
      "CREATE TABLE IF NOT EXISTS compatibility("
      " game_id TEXT PRIMARY KEY, state TEXT NOT NULL, notes TEXT);"
      "CREATE TABLE IF NOT EXISTS launches("
      " id INTEGER PRIMARY KEY AUTOINCREMENT, game_id TEXT NOT NULL,"
      " exit_code INTEGER NOT NULL, wall_seconds REAL NOT NULL,"
      " at REAL NOT NULL);",
      "schema");
    if (!st) return st;
  }
  auto mst = migrate_installations(db_->h);
  if (!mst) return mst;
  return migrate_compatibility(db_->h);
}

void GameDb::close() {
  if (db_) {
    if (db_->h) sqlite3_close_v2(db_->h);
    delete db_;
    db_ = nullptr;
  }
}

namespace {
class Stmt {
public:
  Stmt(sqlite3* h, const char* sql) : h_(h) {
    rc_ = sqlite3_prepare_v2(h_, sql, -1, &st_, nullptr);
  }
  ~Stmt() { if (st_) sqlite3_finalize(st_); }
  bool ok() const { return rc_ == SQLITE_OK && st_; }
  int rc() const { return rc_; }
  bool step_row() { return sqlite3_step(st_) == SQLITE_ROW; }
  Status run(sqlite3* h, const char* what) {
    int rc = sqlite3_step(st_);
    return sql_status(rc, h, what);
  }
  void bind_text(int i, const std::string& v) {
    sqlite3_bind_text(st_, i, v.c_str(), -1, SQLITE_TRANSIENT);
  }
  void bind_int(int i, int64_t v) { sqlite3_bind_int64(st_, i, v); }
  void bind_double(int i, double v) { sqlite3_bind_double(st_, i, v); }
  std::string text(int i) {
    const unsigned char* t = sqlite3_column_text(st_, i);
    return t ? reinterpret_cast<const char*>(t) : "";
  }
  int64_t integer(int i) { return sqlite3_column_int64(st_, i); }
  sqlite3_stmt* raw() { return st_; }

private:
  sqlite3* h_;
  sqlite3_stmt* st_ = nullptr;
  int rc_ = SQLITE_OK;
};
}  // namespace

Status GameDb::add_game(const GameRecord& g) {
  Stmt s(db_->h, "INSERT OR REPLACE INTO games(game_id,title,version,platform)"
                 " VALUES(?,?,?,?)");
  if (!s.ok()) return sql_status(s.rc(), db_->h, "add_game prepare");
  s.bind_text(1, g.game_id);
  s.bind_text(2, g.title);
  s.bind_text(3, g.version);
  s.bind_text(4, g.platform);
  return s.run(db_->h, "add_game");
}

std::optional<GameRecord> GameDb::get_game(const std::string& game_id) {
  Stmt s(db_->h, "SELECT game_id,title,version,platform FROM games"
                 " WHERE game_id=?");
  if (!s.ok()) return std::nullopt;
  s.bind_text(1, game_id);
  if (!s.step_row()) return std::nullopt;
  GameRecord g;
  g.game_id = s.text(0);
  g.title = s.text(1);
  g.version = s.text(2);
  g.platform = s.text(3);
  return g;
}

std::vector<GameRecord> GameDb::list_games() {
  std::vector<GameRecord> out;
  Stmt s(db_->h, "SELECT game_id,title,version,platform FROM games"
                 " ORDER BY title");
  if (!s.ok()) return out;
  while (s.step_row()) {
    GameRecord g;
    g.game_id = s.text(0);
    g.title = s.text(1);
    g.version = s.text(2);
    g.platform = s.text(3);
    out.push_back(g);
  }
  return out;
}

Status GameDb::set_installation(const InstallationRecord& inst) {
  Stmt s(db_->h, "INSERT OR REPLACE INTO installations"
                 "(game_id,path,version,integrity,executable)"
                 " VALUES(?,?,?,?,?)");
  if (!s.ok()) return sql_status(s.rc(), db_->h, "set_installation prepare");
  s.bind_text(1, inst.game_id);
  s.bind_text(2, inst.path);
  s.bind_text(3, inst.version);
  s.bind_text(4, inst.integrity);
  s.bind_text(5, inst.executable);
  return s.run(db_->h, "set_installation");
}

std::optional<InstallationRecord> GameDb::get_installation(
    const std::string& game_id) {
  Stmt s(db_->h, "SELECT game_id,path,version,integrity,executable"
                 " FROM installations WHERE game_id=?");
  if (!s.ok()) return std::nullopt;
  s.bind_text(1, game_id);
  if (!s.step_row()) return std::nullopt;
  InstallationRecord r;
  r.game_id = s.text(0);
  r.path = s.text(1);
  r.version = s.text(2);
  r.integrity = s.text(3);
  r.executable = s.text(4);
  return r;
}

Status GameDb::set_compatibility(const CompatibilityRecord& c) {
  Stmt s(db_->h, "INSERT OR REPLACE INTO compatibility(game_id,state,notes,human_set)"
                 " VALUES(?,?,?,0)");
  if (!s.ok()) return sql_status(s.rc(), db_->h, "set_compat prepare");
  s.bind_text(1, c.game_id);
  s.bind_text(2, c.state);
  s.bind_text(3, c.notes);
  return s.run(db_->h, "set_compat");
}

Status GameDb::set_compatibility_human(const CompatibilityRecord& c) {
  Stmt s(db_->h, "INSERT OR REPLACE INTO compatibility(game_id,state,notes,human_set)"
                 " VALUES(?,?,?,1)");
  if (!s.ok()) return sql_status(s.rc(), db_->h, "set_compat_human prepare");
  s.bind_text(1, c.game_id);
  s.bind_text(2, c.state);
  s.bind_text(3, c.notes);
  return s.run(db_->h, "set_compat_human");
}

bool GameDb::compat_is_human_set(const std::string& game_id) {
  Stmt s(db_->h, "SELECT human_set FROM compatibility WHERE game_id=?");
  if (!s.ok()) return false;
  s.bind_text(1, game_id);
  if (!s.step_row()) return false;
  return s.integer(0) != 0;
}

std::vector<LaunchRow> GameDb::launch_history(const std::string& game_id, int limit) {
  std::vector<LaunchRow> out;
  if (limit < 1) limit = 1;
  if (limit > 1000) limit = 1000;
  Stmt s(db_->h, "SELECT exit_code,wall_seconds,at FROM launches WHERE game_id=?"
                 " ORDER BY id DESC LIMIT ?");
  if (!s.ok()) return out;
  s.bind_text(1, game_id);
  s.bind_int(2, limit);
  while (s.step_row()) {
    LaunchRow r;
    r.exit_code = static_cast<int>(s.integer(0));
    r.wall_seconds = sqlite3_column_double(s.raw(), 1);
    r.at = sqlite3_column_double(s.raw(), 2);
    out.push_back(r);
  }
  return out;
}

std::optional<CompatibilityRecord> GameDb::get_compatibility(
    const std::string& game_id) {
  Stmt s(db_->h, "SELECT game_id,state,notes FROM compatibility"
                 " WHERE game_id=?");
  if (!s.ok()) return std::nullopt;
  s.bind_text(1, game_id);
  if (!s.step_row()) return std::nullopt;
  CompatibilityRecord r;
  r.game_id = s.text(0);
  r.state = s.text(1);
  r.notes = s.text(2);
  return r;
}

Status GameDb::record_launch(const std::string& game_id, int exit_code,
                             double wall_seconds) {
  Stmt s(db_->h, "INSERT INTO launches(game_id,exit_code,wall_seconds,at)"
                 " VALUES(?,?,?,?)");
  if (!s.ok()) return sql_status(s.rc(), db_->h, "record_launch prepare");
  s.bind_text(1, game_id);
  s.bind_int(2, exit_code);
  s.bind_double(3, wall_seconds);
  s.bind_double(4, static_cast<double>(::time(nullptr)));
  return s.run(db_->h, "record_launch");
}

int64_t GameDb::launch_count(const std::string& game_id) {
  Stmt s(db_->h, "SELECT COUNT(*) FROM launches WHERE game_id=?");
  if (!s.ok()) return -1;
  s.bind_text(1, game_id);
  if (!s.step_row()) return -1;
  return s.integer(0);
}

}  // namespace pnr::library
