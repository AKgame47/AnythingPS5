// PNR desktop UI model (slice UI-1), GPL-2.0. No GUI code here: this is the
// layer between the window and the REAL backend (library, scan, import,
// launch). It is unit tested without a display. Status: Implemented, Tested.
#pragma once
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "launcher/launcher_core.hpp"
#include "store/download.hpp"
#include "store/store.hpp"

namespace pnr::ui {

struct LaunchResult {
  std::string game_id;
  bool sandbox = false;
  bool running = false;
  bool finished = false;   // false until the first launch has completed
  bool started = false;    // a child process was spawned
  bool clean = false;      // exited 0
  double seconds = 0.0;
  bool compat_advanced = false;
  std::string summary;     // one line a person can read, same wording as the CLI
  std::string output;      // what the game printed (stdout+stderr, last 8 KiB)
};

// Store screen (slice UI-3). One row per package in a user-added source's
// cached index. There is no default source.
struct StoreRow {
  std::string source, id, name, version, description, license, rights, category, kind;
  std::string label;   // supported | experimental | unsupported (from the index kind)
  std::string status;  // "", queued, paused, partial, verified, failed, installed
  long long size = 0, have = 0;
  std::string error;
};
struct StoreJob {
  bool running = false, finished = false, ok = false;
  std::string what;     // "refresh demo", "download demo/alpha"
  std::string summary;  // the CLI's own result text
};

class UiModel {
public:
  explicit UiModel(std::string db_path);
  ~UiModel();
  UiModel(const UiModel&) = delete;
  UiModel& operator=(const UiModel&) = delete;

  bool open(std::string* error);
  const std::string& db_path() const { return db_path_; }

  // Library. refresh() is a no-op while a launch is running (the launch
  // worker owns its own database connection and writes the history row).
  void refresh();
  const std::vector<launcher::GameEntry>& games() const { return games_; }
  // Indexes into games() that match `query` (case-insensitive substring of
  // title, id, version or compatibility state). Empty query matches all.
  std::vector<size_t> filter(const std::string& query) const;
  // Why this game cannot be launched right now, or "" when it can.
  std::string launch_blocker(const launcher::GameEntry& g) const;

  // Real launch through the same pre-checks as the CLI (pinned-key
  // provenance, optional sandbox) and the same loader. Runs on a worker thread.
  bool start_launch(const std::string& game_id, bool sandbox, std::string* why_not);
  bool launching() const;
  LaunchResult last_launch() const;
  // Call once per frame from the UI thread: joins a finished worker and reloads the list.
  void poll();
  // Blocks until no launch is running (tests, shutdown).
  void wait_idle();

  // Game page (slice UI-2): every call is the same LauncherCore call the CLI makes.
  // Mutating calls are refused while a launch is running. Nothing here ever
  // sets a state on its own: set_compat is called only from a person's click.
  bool game(const std::string& id, launcher::GameEntry* out);
  std::vector<library::LaunchRow> history(const std::string& id, int limit);
  Status set_compat(const std::string& id, const std::string& state, const std::string& note);
  launcher::VerifyOutcome verify(const std::string& id);
  launcher::SavesOutcome export_saves(const std::string& id, const std::string& out_path);
  // Dry run unless confirmed. Refreshes the list after a real removal.
  launcher::RemoveOutcome remove(const std::string& id, bool keep_saves, bool confirmed);

  // Store (UI-3). Same cmd_* code as `pnr-launcher store ...`. Network work
  // (refresh, download) runs on a worker with its own Registry; the list is
  // reloaded when it ends. `http` overrides the real client (tests only).
  void store_reload();
  const std::vector<store::Source>& store_sources() const { return sources_; }
  std::string store_add_source(const std::string& id, const std::string& url, const std::string& rights, bool* ok);
  std::string store_remove_source(const std::string& id, bool confirmed, bool* ok);
  std::vector<StoreRow> store_rows(const std::string& source, const std::string& query);
  bool store_start_refresh(const std::string& source_or_empty, psn::HttpClient* http = nullptr);
  bool store_start_get(const std::string& source, const std::string& pkg, psn::HttpClient* http = nullptr);
  StoreJob store_job() const;
  // Scan, then import, like `store install`. A flagged scan refuses (no override in the UI).
  std::string store_install(const std::string& source, const std::string& pkg, bool* ok, std::string* scan_text);
  void store_poll();

  // Folder scan and import, same backend calls as `scan` and `import`.
  Status scan(const std::string& dir, launcher::ScanReport* out);
  launcher::ImportOutcome import_package(const std::string& path);

private:
  void worker(std::string game_id, bool sandbox);
  std::string db_path_;
  launcher::LauncherCore core_;
  bool opened_ = false;
  std::vector<launcher::GameEntry> games_;
  mutable std::mutex mu_;
  LaunchResult last_;
  std::thread th_;
  bool worker_done_ = false;
  std::vector<store::Source> sources_;
  mutable std::mutex smu_;
  StoreJob sjob_;
  std::thread sth_;
  bool sworker_done_ = false;
  std::string store_dir() const;
};

}  // namespace pnr::ui
