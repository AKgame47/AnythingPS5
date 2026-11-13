// PNR launcher CORE (headless), GPL-2.0.
// Phase 6 first slice: library listing, per-game details with compatibility
// status, launch with history, and text/JSON rendering. This is NOT a desktop
// UI: there is no window, no GUI toolkit and no artwork. A GUI would sit on
// top of this API later. Status: Implemented, Tested (headless).
#pragma once
#include <cstdint>
#include <functional>
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
  bool compat_human_set = false;  // a person set it; automatic bookkeeping leaves it alone
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

// One inspected file from scan(). Read-only classification: nothing is
// imported, extracted or executed, and nothing here says a game is playable.
struct ScanItem {
  std::string file;      // file name inside the scanned directory
  std::string kind;      // "elf" or "pnrgame"
  bool ok = false;       // elf: valid x86-64 ELF; pnrgame: import would accept it
  std::string error;     // reason when !ok
  std::string game_id, title, version;  // pnrgame only
  bool in_library = false;              // pnrgame only: id already in the library
  bool has_imports = false;             // elf only: counts below are valid
  int host = 0, prx_implemented = 0, prx_declared = 0, unknown = 0, pnr_extension = 0;
};

struct ScanReport {
  std::vector<ScanItem> items;
  int skipped = 0;          // regular files that are neither ELF nor .pnrgame
  bool truncated = false;   // more than kScanMaxEntries entries; the rest were not inspected
};
constexpr size_t kScanMaxEntries = 1000;

struct ImportOutcome {
  bool ok = false;
  std::string error;
  std::string game_id, title, version, install_path;
  std::string kind;  // "fresh", "reinstall" (same version) or "different-version"
  std::string previous_version, previous_path;  // set when the id was already installed
};

// What remove() would delete, and what it did. Nothing is deleted unless
// confirmed, and nothing outside <library dir>/games and <library dir>/jail/<id>.
struct RemoveOutcome {
  bool found = false;
  bool refused = false;          // path-containment or safety refusal; nothing deleted
  bool needs_confirmation = false;
  bool removed = false;
  std::string error;
  std::string install_path, jail_path;
  bool install_exists = false, jail_exists = false;
  bool keep_saves = false;
};

struct VerifyOutcome {
  bool found = false;
  bool ok = false;
  std::vector<std::string> problems;
  bool marked_failed = false;    // integrity set to "failed" by this call
};

// Ids usable as a library directory name: 1..128 chars of [A-Za-z0-9._-],
// never "." or "..".
bool valid_game_id(const std::string& id);

struct DoctorIssue {
  std::string kind;     // install-missing, install-outside-library, integrity-failed
  std::string subject;  // game id
  std::string detail;
};
struct DoctorOrphan {
  std::string name;     // entry name directly under games/ or jail/
  std::string kind;     // "dir", "file" or "symlink"
  uint64_t bytes = 0;   // 0 for symlinks (not followed)
};
struct DoctorUsage {
  std::string game_id;
  uint64_t install_bytes = 0, saves_bytes = 0;
};
// Read-only health report. Deletes and fixes nothing.
struct DoctorReport {
  bool ok = false;       // the library was readable
  std::string error;
  int games = 0;
  std::vector<DoctorIssue> issues;
  std::vector<DoctorOrphan> orphan_installs;  // under <library>/games, no row points there
  std::vector<DoctorOrphan> orphan_jails;     // under <library>/jail, no game row (often kept saves)
  std::vector<DoctorUsage> usage;
};

struct CleanItem {
  std::string name;
  uint64_t bytes = 0;
  bool removed = false;
  std::string error;
};
struct CleanOutcome {
  bool ok = false;
  std::string error;
  bool needs_confirmation = false;
  std::vector<CleanItem> items;      // orphan install DIRECTORIES only
  int left_alone = 0;                // orphan files and symlinks, never deleted
};

struct SavesOutcome {
  bool ok = false;
  std::string error;
  int files = 0;
  uint64_t bytes = 0;
  int skipped = 0;   // export: symlinks and special files not followed; restore: existing files left alone
};

constexpr uint64_t kSavesMaxBytes = 64ull * 1024 * 1024;
constexpr size_t kSavesMaxFiles = 10000;

// The six compatibility states the library knows.
const std::vector<std::string>& compat_states();
bool valid_compat_state(const std::string& s);

class LauncherCore {
public:
  Status open(const std::string& db_path);
  library::GameDb& db() { return db_; }
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
  // Hook run in the game's forked child before exec (the sandbox uses it).
  void set_child_setup(std::function<bool(std::string*)> f) { child_setup_ = std::move(f); }
  LaunchOutcome launch(const std::string& game_id,
                       const std::vector<std::string>& args = {},
                       const std::vector<std::string>& env = {});

  // Human-set compatibility. Rejects unknown ids, unknown state names and
  // overlong or control-character notes. The state is marked human-set so
  // launches and re-imports never overwrite it. Only a person reaches
  // "Verified Playable" / "Playable With Issues": no automatic path does.
  Status set_compat(const std::string& game_id, const std::string& state,
                    const std::string& note);
  // Newest first. Returns false for an unknown id (or no open library).
  bool history(const std::string& game_id, int limit,
               std::vector<library::LaunchRow>* out);
  // Read-only directory scan (non-recursive, symlinks not followed).
  Status scan(const std::string& dir, ScanReport* out);

  // Runs the importer (every check) into this library's own directory.
  // A previous install tree of another version is left in place and reported.
  ImportOutcome import_package(const std::string& pkg_path);
  // Dry run unless confirmed. keep_saves keeps <library dir>/jail/<id>.
  RemoveOutcome remove(const std::string& game_id, bool keep_saves, bool confirmed);
  // Re-checks the installed files (existence, entry is an x86-64 ELF). It
  // cannot re-check payload checksums (the manifest is not kept). It only ever
  // marks integrity "failed"; it never restores "verified" and never touches
  // compatibility.
  VerifyOutcome verify(const std::string& game_id);

  DoctorReport doctor();
  // Removes only orphan install directories under <library>/games. Dry run
  // unless confirmed. Never touches jail folders (saves) or library rows.
  CleanOutcome clean_orphans(bool confirmed);
  // Backs up <library>/jail/<id> as a plain ustar archive. Read-only on the
  // library. Refuses to overwrite out_path.
  SavesOutcome export_saves(const std::string& game_id, const std::string& out_path);
  // Restores an archive into <library>/jail/<id>. All names are validated
  // and every target checked before anything is written. Existing files are
  // left alone unless overwrite.
  SavesOutcome restore_saves(const std::string& game_id, const std::string& in_path, bool overwrite);

private:
  library::GameDb db_;
  std::string root_;  // library directory (parent of the db file)
  bool open_ = false;
  std::function<bool(std::string*)> child_setup_;
  std::string jail_root_;  // <library dir>/jail; each game gets <jail_root_>/<game_id>
};

// Human-readable and machine-readable renderings (valid JSON, escaped).
std::string json_escape(const std::string& s);
std::string render_list_text(const std::vector<GameEntry>& games);
std::string render_list_json(const std::vector<GameEntry>& games);
std::string render_entry_text(const GameEntry& g);
std::string render_entry_json(const GameEntry& g);
std::string render_history_text(const GameEntry& g, const std::vector<library::LaunchRow>& rows);
std::string render_history_json(const GameEntry& g, const std::vector<library::LaunchRow>& rows);
std::string render_scan_text(const ScanReport& r);
std::string render_scan_json(const ScanReport& r);
std::string render_import_text(const ImportOutcome& o);
std::string render_import_json(const ImportOutcome& o);
std::string render_remove_text(const std::string& id, const RemoveOutcome& o);
std::string render_remove_json(const std::string& id, const RemoveOutcome& o);
std::string render_verify_text(const std::string& id, const VerifyOutcome& o);
std::string render_verify_json(const std::string& id, const VerifyOutcome& o);
std::string render_doctor_text(const DoctorReport& r);
std::string render_doctor_json(const DoctorReport& r);
std::string render_clean_text(const CleanOutcome& o);
std::string render_clean_json(const CleanOutcome& o);
std::string render_saves_text(const char* verb, const SavesOutcome& o);
std::string render_saves_json(const char* verb, const SavesOutcome& o);
// Archive entry names a restore will accept: relative, no "." / ".." / empty
// components, no NUL or backslash, at most 100 bytes.
bool safe_archive_name(const std::string& n);
std::string render_outcome_json(const std::string& game_id, const LaunchOutcome& o);

}  // namespace pnr::launcher
