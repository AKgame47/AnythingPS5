// Launcher core tests (headless): listing, exact-id lookup, launch + history,
// honest compatibility advancement, rejection of non-runnable entries, JSON
// escaping. Launches real native processes through the real loader.
#include <cstdio>
#include <filesystem>
#include <string>
#include <unistd.h>

#include "launcher/launcher_core.hpp"

void pnr_launcher_tests(int& pass, int& fail) {
  using namespace pnr::launcher;
  namespace fs = std::filesystem;
  auto check = [&](bool ok, const char* label) { if (ok) ++pass; else ++fail; std::printf("%s %s\n", ok ? "PASS" : "FAIL", label); };
  const std::string dir = "/tmp/pnr_launcher_test_" + std::to_string(::getpid());
  fs::remove_all(dir); fs::create_directories(dir);
  LauncherCore core;
  check(!core.open("/nonexistent_dir_zz/x/library.db").ok, "launcher: opening a library in a missing directory fails");
  check(core.list().empty(), "launcher: listing without an open library is empty, no crash");
  check(core.launch("x").error.size() > 0, "launcher: launch without an open library reports an error");
  check(core.open(dir + "/library.db").ok, "launcher: library opens");
  check(core.list().empty() && render_list_text({}).find("empty") != std::string::npos, "launcher: empty library lists as empty");
  pnr::library::GameDb db;  // second handle on the same file, the way an importer would write
  check(db.open(dir + "/library.db").ok, "launcher: importer-side handle opens");
  std::string demo_dir = "/bin", demo_exe = "true";
#ifdef PNR_PS_DEMO_PATH
  demo_dir = fs::path(PNR_PS_DEMO_PATH).parent_path().string();
  demo_exe = fs::path(PNR_PS_DEMO_PATH).filename().string();
#endif
  db.add_game({0, "g.ok", "Good \"Quote\"\nGame", "1.0", "pnr-dev"});
  db.set_installation({"g.ok", demo_dir, "1.0", "verified", demo_exe});
  db.add_game({0, "g.noinst", "Not Installed", "0.1", "pnr-dev"});
  db.add_game({0, "g.failed", "Bad Integrity", "0.1", "pnr-dev"});
  db.set_installation({"g.failed", demo_dir, "0.1", "failed", demo_exe});
  db.add_game({0, "g.noexe", "No Entry", "0.1", "pnr-dev"});
  db.set_installation({"g.noexe", demo_dir, "0.1", "verified", ""});
  db.add_game({0, "g.gone", "Missing File", "0.1", "pnr-dev"});
  db.set_installation({"g.gone", dir, "0.1", "verified", "does_not_exist"});
  db.add_game({0, "g.false", "Exits Nonzero", "0.1", "pnr-dev"});
  db.set_installation({"g.false", "/bin", "0.1", "verified", "false"});
  auto games = core.list();
  check(games.size() == 6, "launcher: list sees all 6 games written by the other handle");
  GameEntry e;
  check(core.get("g.ok", &e) && e.runnable && e.installed && e.compatibility == "Untested" && e.launch_count == 0, "launcher: installed game is runnable, Untested, 0 launches");
  check(core.get("g.noinst", &e) && !e.runnable && e.not_runnable_reason == "not installed", "launcher: uninstalled game is not runnable, with the reason");
  check(core.get("g.failed", &e) && !e.runnable && e.not_runnable_reason.find("integrity") != std::string::npos, "launcher: failed-integrity install is not runnable");
  check(core.get("g.noexe", &e) && !e.runnable, "launcher: installation without an entry point is not runnable");
  check(!core.get("g.o", &e) && !core.get("G.OK", &e) && !core.get("", &e) && !core.get("g.ok ", &e), "launcher: lookup is exact id only (no prefix, case or whitespace match)");
  check(!core.get("g.ok", nullptr), "launcher: null output pointer rejected");
  for (const char* id : {"g.noinst", "g.failed", "g.noexe", "g.gone", "nope"}) {
    auto o = core.launch(id);
    GameEntry after;
    const bool known = core.get(id, &after);
    check(!o.attempted && !o.error.empty() && !o.recorded && (!known || after.launch_count == 0),
          (std::string("launcher: launch refused without spawning or recording history: ") + id).c_str());
  }
  // real launches
  std::vector<std::string> env = {"PNR_JAIL_ROOT=" + dir + "/jail"};
#ifdef PNR_PSABI_DIR
  env.push_back(std::string("LD_LIBRARY_PATH=") + PNR_PSABI_DIR);
#endif
  auto o1 = core.launch("g.ok", {}, env);
  check(o1.found && o1.attempted && o1.finished && !o1.signaled && o1.exit_code == 0 && o1.error.empty(), "launcher: installed game really ran natively and exited 0");
  check(o1.recorded && o1.compat_advanced, "launcher: clean exit recorded in history and advanced Untested -> Launches Only");
  check(core.get("g.ok", &e) && e.launch_count == 1 && e.compatibility == "Launches Only", "launcher: history count 1, state Launches Only (never playable-claiming)");
  auto o2 = core.launch("g.ok", {}, env);
  check(o2.error.empty() && o2.recorded && !o2.compat_advanced && core.get("g.ok", &e) && e.launch_count == 2, "launcher: second launch counts again, does not re-advance");
  db.set_compatibility({"g.ok", "Verified Playable", "human tested"});
  auto o3 = core.launch("g.ok", {}, env);
  check(o3.error.empty() && !o3.compat_advanced && core.get("g.ok", &e) && e.compatibility == "Verified Playable" && e.compatibility_notes == "human tested", "launcher: a launch never overwrites a human-set compatibility state");
  auto of = core.launch("g.false");
  check(of.attempted && of.finished && of.exit_code != 0 && !of.error.empty() && of.recorded && !of.compat_advanced, "launcher: non-zero exit is recorded, reported as an error, and never advances compatibility");
  check(core.get("g.false", &e) && e.compatibility == "Untested" && e.launch_count == 1, "launcher: failed game stays Untested with 1 recorded launch");
  // rendering
  auto j = render_list_json(core.list());
  check(j.find("\"count\":6") != std::string::npos && j.find("Good \\\"Quote\\\"\\nGame") != std::string::npos && j.find('\n') == std::string::npos, "launcher: JSON escapes quotes and newlines, one line, count matches");
  check(json_escape(std::string("a\x01") + "b\\") == "a\\u0001b\\\\", "launcher: control characters and backslashes are escaped");
  check(render_outcome_json("g.ok", o1).find("\"exit_code\":0") != std::string::npos && render_outcome_json("g.ok", o1).find("\"compat_advanced\":true") != std::string::npos, "launcher: outcome JSON reports exit code and advancement");
  core.get("g.ok", &e);
  check(render_entry_text(e).find("Verified Playable") != std::string::npos && render_list_text(core.list()).find("not runnable: not installed") != std::string::npos, "launcher: text rendering shows state and reasons");
  core.close(); db.close();
  fs::remove_all(dir);
}
