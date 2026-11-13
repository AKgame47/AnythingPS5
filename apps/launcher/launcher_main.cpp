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
            "  pnr-launcher launch <game-id> [--json] [-- args...]");
}

int main(int argc, char** argv) {
  std::vector<std::string> a(argv + 1, argv + argc);
  if (a.empty()) { usage(); return 64; }
  bool json = false;
  std::vector<std::string> pos, gameargs;
  bool after = false;
  for (size_t i = 1; i < a.size(); ++i) {
    if (after) gameargs.push_back(a[i]);
    else if (a[i] == "--") after = true;
    else if (a[i] == "--json") json = true;
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
  } else usage();
  core.close();
  return rc;
}
