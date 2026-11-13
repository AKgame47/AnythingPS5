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
      " at REAL NOT NULL);"
      "CREATE TABLE IF NOT EXISTS owned_entitlements("
      " entitlement_id TEXT PRIMARY KEY, product_id TEXT NOT NULL,"
      " title_id TEXT NOT NULL, concept_id TEXT NOT NULL, name TEXT NOT NULL,"
      " platform TEXT NOT NULL, membership TEXT NOT NULL, image_url TEXT NOT NULL,"
      " is_active INTEGER NOT NULL, is_downloadable INTEGER NOT NULL,"
      " is_preorder INTEGER NOT NULL, source TEXT NOT NULL,"
      " first_seen REAL NOT NULL, last_synced REAL NOT NULL,"
      " in_last_sync INTEGER NOT NULL DEFAULT 1, linked_game_id TEXT NOT NULL DEFAULT '');",
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

Status GameDb::remove_game(const std::string& game_id) {
  Status st = exec("BEGIN IMMEDIATE", "remove_game begin");
  if (!st) return st;
  for (const char* sql : {"UPDATE owned_entitlements SET linked_game_id='' WHERE linked_game_id=?",
                          "DELETE FROM launches WHERE game_id=?", "DELETE FROM compatibility WHERE game_id=?",
                          "DELETE FROM installations WHERE game_id=?", "DELETE FROM games WHERE game_id=?"}) {
    Stmt s(db_->h, sql);
    if (!s.ok()) { exec("ROLLBACK", "remove_game rollback"); return sql_status(s.rc(), db_->h, "remove_game prepare"); }
    s.bind_text(1, game_id);
    st = s.run(db_->h, "remove_game");
    if (!st) { exec("ROLLBACK", "remove_game rollback"); return st; }
  }
  return exec("COMMIT", "remove_game commit");
}

namespace {
OwnedRecord read_owned(Stmt& s) {
  OwnedRecord r;
  r.entitlement_id = s.text(0); r.product_id = s.text(1); r.title_id = s.text(2);
  r.concept_id = s.text(3); r.name = s.text(4); r.platform = s.text(5);
  r.membership = s.text(6); r.image_url = s.text(7);
  r.is_active = s.integer(8) != 0; r.is_downloadable = s.integer(9) != 0;
  r.is_preorder = s.integer(10) != 0; r.source = s.text(11);
  r.first_seen = static_cast<double>(s.integer(12)); r.last_synced = static_cast<double>(s.integer(13));
  r.in_last_sync = s.integer(14) != 0; r.linked_game_id = s.text(15);
  return r;
}
const char* kOwnedCols =
    "entitlement_id,product_id,title_id,concept_id,name,platform,membership,image_url,"
    "is_active,is_downloadable,is_preorder,source,CAST(first_seen AS INTEGER),"
    "CAST(last_synced AS INTEGER),in_last_sync,linked_game_id";
}  // namespace

Status GameDb::sync_owned(const std::vector<OwnedRecord>& rows, bool complete, double now,
                          OwnedSyncCounts* counts) {
  OwnedSyncCounts c;
  Status st = exec("BEGIN IMMEDIATE", "sync_owned begin");
  if (!st) return st;
  auto fail = [&](Status e) { exec("ROLLBACK", "sync_owned rollback"); return e; };
  for (const auto& r : rows) {
    std::optional<OwnedRecord> old = get_owned(r.entitlement_id);
    const bool same = old && old->product_id == r.product_id && old->title_id == r.title_id &&
        old->concept_id == r.concept_id && old->name == r.name && old->platform == r.platform &&
        old->membership == r.membership && old->image_url == r.image_url &&
        old->is_active == r.is_active && old->is_downloadable == r.is_downloadable &&
        old->is_preorder == r.is_preorder;
    Stmt s(db_->h,
           "INSERT INTO owned_entitlements(entitlement_id,product_id,title_id,concept_id,name,platform,"
           "membership,image_url,is_active,is_downloadable,is_preorder,source,first_seen,last_synced,"
           "in_last_sync,linked_game_id) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,1,'') "
           "ON CONFLICT(entitlement_id) DO UPDATE SET product_id=excluded.product_id,"
           "title_id=excluded.title_id,concept_id=excluded.concept_id,name=excluded.name,"
           "platform=excluded.platform,membership=excluded.membership,image_url=excluded.image_url,"
           "is_active=excluded.is_active,is_downloadable=excluded.is_downloadable,"
           "is_preorder=excluded.is_preorder,source=excluded.source,last_synced=excluded.last_synced,"
           "in_last_sync=1");
    if (!s.ok()) return fail(sql_status(s.rc(), db_->h, "sync_owned prepare"));
    s.bind_text(1, r.entitlement_id); s.bind_text(2, r.product_id); s.bind_text(3, r.title_id);
    s.bind_text(4, r.concept_id); s.bind_text(5, r.name); s.bind_text(6, r.platform);
    s.bind_text(7, r.membership); s.bind_text(8, r.image_url);
    s.bind_int(9, r.is_active); s.bind_int(10, r.is_downloadable); s.bind_int(11, r.is_preorder);
    s.bind_text(12, r.source); s.bind_double(13, now); s.bind_double(14, now);
    Status rs = s.run(db_->h, "sync_owned upsert");
    if (!rs) return fail(rs);
    if (!old) ++c.added;
    else if (!old->in_last_sync) ++c.restored;
    else if (same) ++c.unchanged;
    else ++c.updated;
    if (old && !old->in_last_sync && same) { /* restored counted above */ }
  }
  if (complete) {
    std::vector<std::string> keep;
    for (const auto& r : rows) keep.push_back(r.entitlement_id);
    for (const auto& o : list_owned()) {
      bool found = false;
      for (const auto& k : keep) if (k == o.entitlement_id) { found = true; break; }
      if (found || !o.in_last_sync) continue;
      Stmt s(db_->h, "UPDATE owned_entitlements SET in_last_sync=0 WHERE entitlement_id=?");
      if (!s.ok()) return fail(sql_status(s.rc(), db_->h, "sync_owned mark prepare"));
      s.bind_text(1, o.entitlement_id);
      Status rs = s.run(db_->h, "sync_owned mark");
      if (!rs) return fail(rs);
      ++c.marked_missing;
    }
  }
  st = exec("COMMIT", "sync_owned commit");
  if (!st) return fail(st);
  if (counts) *counts = c;
  return Status::Ok();
}

std::vector<OwnedRecord> GameDb::list_owned() {
  std::vector<OwnedRecord> v;
  std::string sql = std::string("SELECT ") + kOwnedCols + " FROM owned_entitlements ORDER BY name COLLATE NOCASE, entitlement_id";
  Stmt s(db_->h, sql.c_str());
  if (!s.ok()) return v;
  while (s.step_row()) v.push_back(read_owned(s));
  return v;
}

std::optional<OwnedRecord> GameDb::get_owned(const std::string& id) {
  std::string sql = std::string("SELECT ") + kOwnedCols + " FROM owned_entitlements WHERE entitlement_id=?";
  Stmt s(db_->h, sql.c_str());
  if (!s.ok()) return std::nullopt;
  s.bind_text(1, id);
  if (!s.step_row()) return std::nullopt;
  return read_owned(s);
}

Status GameDb::link_owned(const std::string& eid, const std::string& game_id) {
  if (!get_owned(eid)) return Status::Fail(120, "library.db", "no such owned entitlement");
  if (!get_game(game_id)) return Status::Fail(121, "library.db", "no such game");
  Stmt s(db_->h, "UPDATE owned_entitlements SET linked_game_id=? WHERE entitlement_id=?");
  if (!s.ok()) return sql_status(s.rc(), db_->h, "link_owned prepare");
  s.bind_text(1, game_id); s.bind_text(2, eid);
  return s.run(db_->h, "link_owned");
}

Status GameDb::unlink_owned(const std::string& eid) {
  if (!get_owned(eid)) return Status::Fail(120, "library.db", "no such owned entitlement");
  Stmt s(db_->h, "UPDATE owned_entitlements SET linked_game_id='' WHERE entitlement_id=?");
  if (!s.ok()) return sql_status(s.rc(), db_->h, "unlink_owned prepare");
  s.bind_text(1, eid);
  return s.run(db_->h, "unlink_owned");
}

Status GameDb::forget_owned() { return exec("DELETE FROM owned_entitlements", "forget_owned"); }

int64_t GameDb::launch_count(const std::string& game_id) {
  Stmt s(db_->h, "SELECT COUNT(*) FROM launches WHERE game_id=?");
  if (!s.ok()) return -1;
  s.bind_text(1, game_id);
  if (!s.step_row()) return -1;
  return s.integer(0);
}

}  // namespace pnr::library
