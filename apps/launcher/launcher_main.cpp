// pnr-launcher: headless launcher front end (text or --json). NOT a desktop UI.
// Library: ./pnr_data/library.db (override with PNR_LIBRARY_DB). GPL-2.0.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "launcher/launcher_core.hpp"

static void usage() {
  std::puts("pnr-launcher (headless launcher core, not a desktop UI)\n"
            "usage:\n"
            "  pnr-launcher list [--json]\n"
            "  pnr-launcher show <game-id> [--json]\n"
            "  pnr-launcher launch <game-id> [--json] [-- args...]\n"
            "  pnr-launcher compat states\n"
            "  pnr-launcher compat show <game-id> [--json]\n"
            "  pnr-launcher compat set <game-id> <state> [--note TEXT]\n"
            "  pnr-launcher history <game-id> [--limit N] [--json]\n"
            "  pnr-launcher scan <dir> [--json]   (read-only; imports and runs nothing)\n"
            "  pnr-launcher import <file.pnrgame> [--json]\n"
            "  pnr-launcher remove <game-id> [--keep-saves] [--yes] [--json]\n"
            "  pnr-launcher verify <game-id> [--json]");
}

int main(int argc, char** argv) {
  std::vector<std::string> a(argv + 1, argv + argc);
  if (a.empty()) { usage(); return 64; }
  bool json = false;
  std::vector<std::string> pos, gameargs;
  bool after = false, have_note = false, yes = false, keep = false;
  std::string note, limit_s = "20";
  for (size_t i = 1; i < a.size(); ++i) {
    if (after) gameargs.push_back(a[i]);
    else if (a[i] == "--") after = true;
    else if (a[i] == "--json") json = true;
    else if (a[i] == "--yes") yes = true;
    else if (a[i] == "--keep-saves") keep = true;
    else if (a[i] == "--note" && i + 1 < a.size()) note = a[++i], have_note = true;
    else if (a[i] == "--limit" && i + 1 < a.size()) limit_s = a[++i];
    else pos.push_back(a[i]);
  }
  const char* dbp = std::getenv("PNR_LIBRARY_DB");
  pnr::launcher::LauncherCore core;
  auto st = core.open(dbp ? dbp : "./pnr_data/library.db");
  if (!st) { std::fprintf(stderr, "launcher: library open failed: %s\n", st.error.message.c_str()); return 2; }
  using namespace pnr::launcher;
  int rc = 64;
  if (a[0] == "list" && pos.empty()) {
    auto g = core.list();
    std::fputs((json ? render_list_json(g) + "\n" : render_list_text(g)).c_str(), stdout);
    rc = 0;
  } else if (a[0] == "show" && pos.size() == 1) {
    GameEntry e;
    if (!core.get(pos[0], &e)) { std::fprintf(stderr, "launcher: no game with id '%s'\n", pos[0].c_str()); rc = 2; }
    else { std::fputs((json ? render_entry_json(e) + "\n" : render_entry_text(e)).c_str(), stdout); rc = 0; }
  } else if (a[0] == "launch" && pos.size() == 1) {
    auto o = core.launch(pos[0], gameargs);
    if (json) std::puts(render_outcome_json(pos[0], o).c_str());
    else if (!o.error.empty()) std::printf("LAUNCH %s: %s\n", o.attempted ? "RAN, NOT CLEAN" : "NOT STARTED", o.error.c_str());
    else std::printf("LAUNCH OK: exit 0 in %.3fs%s\n", o.wall_seconds, o.compat_advanced ? " (compatibility Untested -> Launches Only)" : "");
    rc = o.error.empty() ? 0 : (o.found ? 1 : 2);
  } else if (a[0] == "compat" && pos.size() == 1 && pos[0] == "states") {
    for (const auto& s : compat_states()) std::printf("%s\n", s.c_str());
    rc = 0;
  } else if (a[0] == "compat" && pos.size() == 2 && pos[0] == "show") {
    GameEntry e;
    if (!core.get(pos[1], &e)) { std::fprintf(stderr, "launcher: no game with id '%s'\n", pos[1].c_str()); rc = 2; }
    else if (json) {
      std::printf("{\"game_id\":\"%s\",\"state\":\"%s\",\"human_set\":%s,\"notes\":\"%s\"}\n", json_escape(e.game_id).c_str(),
                  json_escape(e.compatibility).c_str(), e.compat_human_set ? "true" : "false", json_escape(e.compatibility_notes).c_str());
      rc = 0;
    } else {
      std::printf("%s: %s%s\n", e.game_id.c_str(), e.compatibility.c_str(), e.compat_human_set ? "  (set by a person)" : "  (automatic)");
      if (!e.compatibility_notes.empty()) std::printf("  notes: %s\n", e.compatibility_notes.c_str());
      rc = 0;
    }
  } else if (a[0] == "compat" && pos.size() == 3 && pos[0] == "set") {
    auto s = core.set_compat(pos[1], pos[2], have_note ? note : "set by a person");
    if (!s) {
      std::fprintf(stderr, "launcher: compat set refused: %s\n", s.error.message.c_str());
      rc = s.error.message.rfind("no such game", 0) == 0 ? 2 : 64;
    } else {
      std::printf("COMPAT SET: %s -> %s (set by a person)\n", pos[1].c_str(), pos[2].c_str());
      rc = 0;
    }
  } else if (a[0] == "history" && pos.size() == 1) {
    char* end = nullptr;
    long lim = std::strtol(limit_s.c_str(), &end, 10);
    GameEntry e; std::vector<pnr::library::LaunchRow> rows;
    if (limit_s.empty() || *end != 0 || lim < 1 || lim > 1000) { std::fprintf(stderr, "launcher: --limit must be 1..1000\n"); rc = 64; }
    else if (!core.get(pos[0], &e) || !core.history(pos[0], static_cast<int>(lim), &rows)) { std::fprintf(stderr, "launcher: no game with id '%s'\n", pos[0].c_str()); rc = 2; }
    else { std::fputs((json ? render_history_json(e, rows) + "\n" : render_history_text(e, rows)).c_str(), stdout); rc = 0; }
  } else if (a[0] == "scan" && pos.size() == 1) {
    ScanReport rep;
    auto s = core.scan(pos[0], &rep);
    if (!s) { std::fprintf(stderr, "launcher: scan failed: %s\n", s.error.message.c_str()); rc = 2; }
    else { std::fputs((json ? render_scan_json(rep) + "\n" : render_scan_text(rep)).c_str(), stdout); rc = 0; }
  } else if (a[0] == "import" && pos.size() == 1) {
    auto o = core.import_package(pos[0]);
    std::fputs((json ? render_import_json(o) + "\n" : render_import_text(o)).c_str(), stdout);
    rc = o.ok ? 0 : 1;
  } else if (a[0] == "remove" && pos.size() == 1) {
    auto o = core.remove(pos[0], keep, yes);
    std::fputs((json ? render_remove_json(pos[0], o) + "\n" : render_remove_text(pos[0], o)).c_str(), stdout);
    rc = o.removed ? 0 : !o.found ? 2 : o.needs_confirmation ? 3 : 1;
  } else if (a[0] == "verify" && pos.size() == 1) {
    auto o = core.verify(pos[0]);
    std::fputs((json ? render_verify_json(pos[0], o) + "\n" : render_verify_text(pos[0], o)).c_str(), stdout);
    rc = !o.found ? 2 : o.ok ? 0 : 1;
  } else usage();
  core.close();
  return rc;
}
