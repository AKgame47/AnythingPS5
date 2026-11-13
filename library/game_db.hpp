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
};

struct CompatibilityRecord {
  std::string game_id;
  // Verified Playable | Playable With Issues | Launches Only |
  // Unsupported | Untested | Blocked by Missing Dependency
  std::string state = "Untested";
  std::string notes;
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

  Status record_launch(const std::string& game_id, int exit_code,
                       double wall_seconds);
  int64_t launch_count(const std::string& game_id);

private:
  struct Impl;
  Impl* db_ = nullptr;
  Status exec(const char* sql, const char* what);
};

}  // namespace pnr::library
