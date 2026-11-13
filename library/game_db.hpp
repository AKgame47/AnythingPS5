// PNR Game Manager storage - MODULE 09 data layer (first slice).
// SQLite-backed metadata: games, installations, compatibility records,
// launch history. Verified entitlement records are a separate future
// table; nothing here asserts ownership or execution rights.
// Status: Implemented, Tested.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/runtime.hpp"

namespace pnr::library {

struct GameRecord {
  int64_t id = 0;
  std::string game_id;   // stable id from the package manifest
  std::string title;
  std::string version;
  std::string platform = "pnr-dev";  // developer-controlled content
};

struct InstallationRecord {
  std::string game_id;
  std::string path;
  std::string version;
  std::string integrity = "unverified";  // unverified|verified|failed
  std::string executable;  // entry point inside the installed tree
};

struct CompatibilityRecord {
  std::string game_id;
  // Verified Playable | Playable With Issues | Launches Only |
  // Unsupported | Untested | Blocked by Missing Dependency
  std::string state = "Untested";
  std::string notes;
};

struct LaunchRow {
  int exit_code = 0;       // -1 when the game was killed by a signal
  double wall_seconds = 0;
  double at = 0;           // unix seconds
};

// A purchase ACCESS record synced from PSN (entitlement metadata only).
// It is not an installation, never runnable, and is not a compatibility
// record. linked_game_id is set only by a person.
struct OwnedRecord {
  std::string entitlement_id;  // primary key
  std::string product_id, title_id, concept_id;
  std::string name;
  std::string platform;        // as Sony reports it, upper-case (PS4, PS5, ...)
  std::string membership;      // as Sony reports it, may be empty
  std::string image_url;       // https only, may be empty
  bool is_active = false, is_downloadable = false, is_preorder = false;
  std::string source = "psn";
  double first_seen = 0, last_synced = 0;
  bool in_last_sync = true;    // false: a complete sync did not return it
  std::string linked_game_id;  // empty unless a person linked it
};

struct OwnedSyncCounts {
  int added = 0, updated = 0, unchanged = 0, marked_missing = 0, restored = 0;
};

class GameDb {
public:
  Status open(const std::string& path);   // creates schema if needed
  void close();

  Status add_game(const GameRecord& g);
  std::optional<GameRecord> get_game(const std::string& game_id);
  std::vector<GameRecord> list_games();

  Status set_installation(const InstallationRecord& inst);
  std::optional<InstallationRecord> get_installation(const std::string& game_id);

  Status set_compatibility(const CompatibilityRecord& c);
  std::optional<CompatibilityRecord> get_compatibility(const std::string& game_id);

  // Compatibility set by a person. Marked human_set so automatic launch
  // bookkeeping and re-imports never overwrite it. set_compatibility()
  // above is the automatic path and clears the mark.
  Status set_compatibility_human(const CompatibilityRecord& c);
  bool compat_is_human_set(const std::string& game_id);
  // Newest first, at most `limit` rows (1..1000).
  std::vector<LaunchRow> launch_history(const std::string& game_id, int limit);

  // Deletes the game's rows in every table (game, installation,
  // compatibility, launch history) in one transaction. Files on disk are
  // not touched here. Removing an unknown id is a no-op that succeeds.
  Status remove_game(const std::string& game_id);

  // --- Owned-on-PSN entitlements (separate from games/installations) ---
  // Upserts by entitlement_id (never touches linked_game_id). When
  // `complete` is true, previously synced rows absent from `rows` are
  // marked in_last_sync=false (kept, not deleted).
  Status sync_owned(const std::vector<OwnedRecord>& rows, bool complete, double now,
                    OwnedSyncCounts* counts);
  std::vector<OwnedRecord> list_owned();
  std::optional<OwnedRecord> get_owned(const std::string& entitlement_id);
  // Links to an existing games row. Fails for unknown entitlement or game.
  Status link_owned(const std::string& entitlement_id, const std::string& game_id);
  Status unlink_owned(const std::string& entitlement_id);
  Status forget_owned();  // deletes every owned row

  Status record_launch(const std::string& game_id, int exit_code,
                       double wall_seconds);
  int64_t launch_count(const std::string& game_id);

private:
  struct Impl;
  Impl* db_ = nullptr;
  Status exec(const char* sql, const char* what);
};

}  // namespace pnr::library
