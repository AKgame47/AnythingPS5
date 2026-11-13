// PNR launcher CORE (headless), GPL-2.0.
// Phase 6 first slice: library listing, per-game details with compatibility
// status, launch with history, and text/JSON rendering. This is NOT a desktop
// UI: there is no window, no GUI toolkit and no artwork. A GUI would sit on
// top of this API later. Status: Implemented, Tested (headless).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/runtime.hpp"
#include "library/game_db.hpp"

namespace pnr::launcher {

struct GameEntry {
  std::string game_id, title, version, platform;
  std::string compatibility = "Untested";  // library compatibility state
  std::string compatibility_notes;
  bool installed = false;      // an installation record exists
  std::string install_path, executable, integrity;
  bool runnable = false;       // installed, with an entry point
  std::string not_runnable_reason;  // empty when runnable
  int64_t launch_count = 0;
};

struct LaunchOutcome {
  bool found = false;       // the game id exists in the library
  bool attempted = false;   // a child process was spawned
  bool finished = false;
  bool signaled = false;
  int exit_code = -1;
  double wall_seconds = 0.0;
  bool recorded = false;    // launch row written to the history
  bool compat_advanced = false;  // Untested -> Launches Only
  std::string error;        // empty when the game ran and exited
};

class LauncherCore {
public:
  Status open(const std::string& db_path);
  void close();
  std::vector<GameEntry> list();
  // Exact game id only; never a prefix or fuzzy match.
  bool get(const std::string& game_id, GameEntry* out);
  // Starts the installed game natively through the same loader as `pnr run`.
  // Like `pnr run <id>`: the game gets its own jail (PNR_JAIL_ROOT) and the PS-ABI
  // shim directory (LD_LIBRARY_PATH = this executable's directory) unless the
  // caller already supplied those variables in env.
  // A clean exit (0) moves Untested -> Launches Only; nothing else changes
  // compatibility, and no state ever claims playability.
  LaunchOutcome launch(const std::string& game_id,
                       const std::vector<std::string>& args = {},
                       const std::vector<std::string>& env = {});

private:
  library::GameDb db_;
  bool open_ = false;
  std::string jail_root_;  // <library dir>/jail; each game gets <jail_root_>/<game_id>
};

// Human-readable and machine-readable renderings (valid JSON, escaped).
std::string json_escape(const std::string& s);
std::string render_list_text(const std::vector<GameEntry>& games);
std::string render_list_json(const std::vector<GameEntry>& games);
std::string render_entry_text(const GameEntry& g);
std::string render_entry_json(const GameEntry& g);
std::string render_outcome_json(const std::string& game_id, const LaunchOutcome& o);

}  // namespace pnr::launcher
