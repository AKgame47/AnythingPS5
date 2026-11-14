// Slice UI-1 tests: the desktop UI's model layer against the REAL backend
// (library, import, scan, launch). No display is needed or used here.
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include "package/importer.hpp"
#include "psn/http.hpp"
#include "ui/ui_model.hpp"

namespace {
struct UiFakeHttp : pnr::psn::HttpClient {
  std::string body; std::vector<std::string> urls; bool fail = false;
  pnr::Status perform(const pnr::psn::HttpRequest& r, pnr::psn::HttpResponse* res) override {
    urls.push_back(r.url); *res = pnr::psn::HttpResponse{};
    if (fail) return pnr::Status::Fail(214, "psn", "tls");
    res->status = 200; res->body = body; return pnr::Status::Ok();
  }
};
}  // namespace

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

  // ---- UI-2 game page: same core calls as the CLI
  {
    pnr::launcher::GameEntry ge;
    check(ui.game("ui.demo", &ge) && ge.title == "UI Demo Game" && !ui.game("nope", &ge), "ui2: game() finds an exact id and rejects an unknown one");
    auto hist = ui.history("ui.demo", 10);
    check(hist.size() >= 2 && hist[0].at >= hist.back().at && ui.history("nope", 10).empty(), "ui2: history lists real launches newest first; unknown id is empty");
    check(!ui.set_compat("ui.demo", "Not A State", "").ok, "ui2: an unknown status name is rejected");
    check(!ui.set_compat("nope", "Untested", "").ok, "ui2: set_compat on an unknown id is rejected");
    check(ui.set_compat("ui.demo", "Playable With Issues", "tested by hand").ok, "ui2: a person can set a status");
    check(ui.games()[0].compatibility == "Playable With Issues" && ui.games()[0].compat_human_set, "ui2: the list shows the human-set status");
    ui.start_launch("ui.demo", false, &why); ui.wait_idle(); ui.poll();
    check(ui.games()[0].compatibility == "Playable With Issues", "ui2: a later launch never overwrites a human-set status");
    auto v = ui.verify("ui.demo");
    check(v.found && v.ok, "ui2: verify passes on intact files");
    std::string tarp = dir + "/saves.tar";
    auto ex = ui.export_saves("ui.demo", tarp);
    check(ex.ok || !ex.error.empty(), "ui2: export saves returns a definite result");
    if (ex.ok) check(!ui.export_saves("ui.demo", tarp).ok, "ui2: export refuses to overwrite an existing file");
    auto d1 = ui.remove("ui.demo", true, false);
    check(d1.found && !d1.removed && d1.needs_confirmation && fs::exists(d1.install_path) && ui.games().size() == 1, "ui2: remove without confirmation is a dry run and deletes nothing");
  }

  // a game whose files vanished cannot launch
  fs::remove_all(dir + "/lib/games");
  ui.refresh();
  ui.start_launch("ui.demo", false, &why);  // may be refused up front or fail at launch; either way it must end
  ui.wait_idle(); ui.poll();
  r = ui.last_launch();
  check(r.finished && !r.running, "ui: a launch of a game with missing files ends (no hang)");
  {
    auto v2 = ui.verify("ui.demo");
    check(v2.found && !v2.ok && !v2.problems.empty(), "ui2: verify reports missing files and marks integrity failed");
    auto d2 = ui.remove("ui.demo", false, true);
    check(d2.removed && ui.games().empty(), "ui2: a confirmed remove deletes the entry and the list refreshes");
  }
  check(!ui.launching(), "ui: nothing is running at the end");
  // ---- UI-3 store: same cmd_* code as the CLI; the network is a fake client here
  {
    UiModel st(dir + "/slib/library.db");
    std::string e3;
    check(st.open(&e3) && st.store_sources().empty(), "ui3: a fresh library has no store source (no default source)");
    bool ok = false;
    std::string msg = st.store_add_source("demo", "https://games.example.invalid/index.json", "", &ok);
    check(!ok && has(msg, "rights"), "ui3: a source needs a declared rights statement");
    msg = st.store_add_source("demo", "http://games.example.invalid/index.json", "mine", &ok);
    check(!ok, "ui3: a non-https source URL is refused");
    msg = st.store_add_source("demo", "https://games.example.invalid/index.json", "Author's own games", &ok);
    check(ok && st.store_sources().size() == 1 && has(msg, "cannot be verified"), "ui3: a source with a rights statement is added; rights are labelled unverified");
    check(st.store_rows("", "").empty(), "ui3: nothing is listed before a refresh");
    UiFakeHttp fk;
    std::string h = "\"format\":\"pnr-store-index-1\",\"name\":\"Demo\",\"rights\":\"Own games by the author\"";
    auto pk = [](const std::string& id, const std::string& kind) { return "{\"id\":\"" + id + "\",\"name\":\"Game " + id + "\",\"version\":\"1.0\",\"license\":\"MIT\",\"kind\":\"" + kind + "\",\"size\":1234,\"sha256\":\"" + std::string(64, 'a') + "\",\"url\":\"https://games.example.invalid/p/" + id + ".pnrgame\",\"description\":\"A tiny demo\",\"category\":\"puzzle\"}"; };
    fk.body = "{" + h + ",\"packages\":[" + pk("alpha", "pnr-package") + "," + pk("beta", "ps5-pkg") + "]}";
    check(st.store_start_refresh("demo", &fk), "ui3: refresh starts on a worker");
    st.wait_idle(); st.store_poll();
    check(st.store_job().finished && st.store_job().ok && !fk.urls.empty() && fk.urls[0] == "https://games.example.invalid/index.json", "ui3: refresh fetched only the source's own index URL");
    auto rows = st.store_rows("", "");
    check(rows.size() == 2 && rows[0].label == "supported" && rows[1].label == "unsupported", "ui3: packages listed; a non-pnr kind is labelled unsupported");
    check(st.store_rows("", "alph").size() == 1 && st.store_rows("demo", "zzz").empty() && st.store_rows("nope", "").empty(), "ui3: search and source filters work");
    fk.fail = true;
    st.store_start_refresh("demo", &fk); st.wait_idle(); st.store_poll();
    check(st.store_job().finished && !st.store_job().ok && st.store_rows("", "").size() == 2, "ui3: a failed refresh is reported and keeps the cached list");
    bool iok = true; std::string scan;
    std::string im = st.store_install("demo", "alpha", &iok, &scan);
    check(!iok && !im.empty(), "ui3: install of a package that was never downloaded is refused");
    msg = st.store_remove_source("demo", false, &ok);
    check(!ok && st.store_sources().size() == 1 && has(msg, "WOULD REMOVE"), "ui3: remove source is a dry run until confirmed");
    msg = st.store_remove_source("demo", true, &ok);
    check(ok && st.store_sources().empty() && st.store_rows("", "").empty(), "ui3: confirmed remove deletes the source and its cached index");
  }
  fs::remove_all(dir);
}
