// Slice UI-1 tests: the desktop UI's model layer against the REAL backend
// (library, import, scan, launch). No display is needed or used here.
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include "package/importer.hpp"
#include "ui/ui_model.hpp"

void pnr_ui_model_tests(int& pass, int& fail) {
  using namespace pnr::ui;
  namespace fs = std::filesystem;
  auto check = [&](bool ok, const std::string& l) { if (ok) ++pass; else ++fail; std::printf("%s %s\n", ok ? "PASS" : "FAIL", l.c_str()); };
  auto has = [](const std::string& h, const std::string& n) { return h.find(n) != std::string::npos; };
  const std::string dir = "/tmp/pnr_ui_model_" + std::to_string(::getpid());
  fs::remove_all(dir); fs::create_directories(dir + "/pkgs");
  std::string demo = "/bin/true";
#ifdef PNR_PS_DEMO_PATH
  demo = PNR_PS_DEMO_PATH;
#endif
  pnr::pkg::Manifest m; m.id = "ui.demo"; m.title = "UI Demo Game"; m.version = "1.2.3"; m.executable = "bin/game";
  check(pnr::pkg::build_package(dir + "/pkgs/ui.pnrgame", m, {{"bin/game", demo}}).ok, "ui: fixture package built");

  UiModel ui(dir + "/lib/library.db");
  std::string err;
  check(ui.open(&err) && ui.games().empty() && ui.filter("").empty(), "ui: an empty library opens and lists nothing");
  std::string why;
  check(!ui.start_launch("nope", false, &why) && has(why, "no game"), "ui: launching an unknown id is refused with a reason");

  // scan + import go through the real backend
  pnr::launcher::ScanReport rep;
  check(ui.scan(dir + "/pkgs", &rep).ok && rep.items.size() == 1 && rep.items[0].kind == "pnrgame" && rep.items[0].ok && !rep.items[0].in_library, "ui: scan finds the package, not yet in the library");
  auto imp = ui.import_package(dir + "/pkgs/ui.pnrgame");
  check(imp.ok && ui.games().size() == 1 && ui.games()[0].title == "UI Demo Game", "ui: import adds the game and the list refreshes");
  if (ui.games().empty()) { check(false, "ui: nothing imported, stopping"); fs::remove_all(dir); return; }
  check(!ui.import_package(dir + "/pkgs/missing.pnrgame").ok, "ui: a missing package is an error, not a crash");
  rep = {}; ui.scan(dir + "/pkgs", &rep);
  check(rep.items.size() == 1 && rep.items[0].in_library, "ui: scan now reports it as already in the library");

  // filter
  check(ui.filter("demo").size() == 1 && ui.filter("DEMO").size() == 1 && ui.filter("ui.demo").size() == 1 && ui.filter("1.2.3").size() == 1 && ui.filter("untested").size() == 1, "ui: filter matches title, id, version and compatibility, case-insensitively");
  check(ui.filter("zzz").empty(), "ui: filter with no match is empty");
  check(ui.launch_blocker(ui.games()[0]).empty(), "ui: an installed runnable game has no launch blocker");
  pnr::launcher::GameEntry bad; bad.game_id = "x"; bad.installed = false;
  check(ui.launch_blocker(bad) == "not installed", "ui: an uninstalled entry says why it cannot launch");

  // real launch on a worker thread
  check(ui.start_launch("ui.demo", false, &why), "ui: launch starts");
  ui.wait_idle(); ui.poll(); ui.refresh();
  auto r = ui.last_launch();
  check(r.finished && !r.running && r.started && r.clean && has(r.summary, "LAUNCH OK") && r.compat_advanced, "ui: launch finished cleanly through the real loader");
  check(has(r.output, "ALL SCE CALLS DISPATCHED OK"), "ui: the game's own output is captured for the window");
  check(ui.games()[0].compatibility == "Launches Only" && ui.games()[0].launch_count == 1, "ui: the list shows Launches Only and 1 launch (and never Playable)");

  // sandbox request: either it runs or it refuses loudly; never a silent success without isolation
  check(ui.start_launch("ui.demo", true, &why), "ui: sandboxed launch starts");
  ui.wait_idle(); ui.poll();
  r = ui.last_launch();
  check(r.finished && r.sandbox && ((r.clean && has(r.summary, "LAUNCH OK")) || has(r.summary, "LAUNCH NOT STARTED")), "ui: sandboxed launch runs or refuses loudly");

  // a game whose files vanished cannot launch
  fs::remove_all(dir + "/lib/games");
  ui.refresh();
  ui.start_launch("ui.demo", false, &why);  // may be refused up front or fail at launch; either way it must end
  ui.wait_idle(); ui.poll();
  r = ui.last_launch();
  check(r.finished && !r.running, "ui: a launch of a game with missing files ends (no hang)");
  check(!ui.launching(), "ui: nothing is running at the end");
  fs::remove_all(dir);
}
