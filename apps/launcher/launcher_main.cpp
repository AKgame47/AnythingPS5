// pnr-launcher: headless launcher front end (text or --json). NOT a desktop UI.
// Library: ./pnr_data/library.db (override with PNR_LIBRARY_DB). GPL-2.0.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <fstream>
#include <iostream>
#include <iterator>

#include "launcher/launcher_core.hpp"
#include "sandbox/sandbox.hpp"
#include "research/research.hpp"
#include "scan/scan.hpp"
#include "psn/psn_cli.hpp"
#include "store/store_cli.hpp"

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
            "  pnr-launcher verify <game-id> [--json]\n"
            "  pnr-launcher doctor [--json]   (read-only health report)\n"
            "  pnr-launcher clean --orphans [--yes] [--json]   (orphan install dirs only; dry run by default)\n"
            "  pnr-launcher export <game-id> <out.tar> [--json]   (back up saves)\n"
            "  pnr-launcher restore-saves <game-id> <in.tar> [--overwrite] [--json]\n"
            "  pnr-launcher store source add <id> <https-index-url> --rights <statement> | store source remove <id> [--yes] | store source list\n"
            "  pnr-launcher store get <source> <package> | store queue list|run [<source> <package>] | store queue pause|resume|cancel <source> <package> [--yes] | store install <source> <package>\n"
            "  pnr-launcher store refresh [id] | store search [text] [--source id] | store show <source> <package> [--json]\n"
            "  pnr-launcher research log add <text> | log list [--limit N] | log verify   (append-only, hash-chained)\n"
            "  pnr-launcher research fetch <https-url> <out-file> --rights <statement> [--sha256 HEX] [--force]   (user-supplied URL only)\n"
            "  pnr-launcher research inspect <file> [--log]   (read-only; recognises PNR formats, never opens Sony packages)\n"
            "  pnr-launcher psn status [--json]   (local state; no network)\n"
            "  pnr-launcher psn set-token [--file PATH]   (token from stdin or a file, never argv)\n"
            "  pnr-launcher psn import-json <file> [--complete] [--json]   (saved purchased-games response)\n"
            "  pnr-launcher psn sync [--complete] [--json]   (LIVE: contacts Sony hosts with your stored token; untested vs Sony)\n"
            "  pnr-launcher psn owned [--platform PS4|PS5] [--json]\n"
            "  pnr-launcher psn link <entitlement-id> <game-id> | psn unlink <entitlement-id>\n"
            "  pnr-launcher psn disconnect [--forget-owned] [--yes]");
}

int main(int argc, char** argv) {
  std::vector<std::string> a(argv + 1, argv + argc);
  if (a.empty()) { usage(); return 64; }
  bool json = false;
  std::vector<std::string> pos, gameargs;
  bool after = false, complete = false, forget = false, have_note = false, yes = false, keep = false, overwrite = false, orphans = false;
  bool sandbox_on = false, allow_flagged = false, force = false, log_it = false;
  std::string sha256_arg;
  std::string note, limit_s = "20", file, platform, rights, source;
  for (size_t i = 1; i < a.size(); ++i) {
    if (after) gameargs.push_back(a[i]);
    else if (a[i] == "--") after = true;
    else if (a[i] == "--json") json = true;
    else if (a[i] == "--yes") yes = true;
    else if (a[i] == "--sandbox") sandbox_on = true;
    else if (a[i] == "--allow-flagged") allow_flagged = true;
    else if (a[i] == "--force") force = true;
    else if (a[i] == "--log") log_it = true;
    else if (a[i] == "--sha256" && i + 1 < a.size()) sha256_arg = a[++i];
    else if (a[i] == "--overwrite") overwrite = true;
    else if (a[i] == "--orphans") orphans = true;
    else if (a[i] == "--complete") complete = true;
    else if (a[i] == "--forget-owned") forget = true;
    else if (a[i] == "--file" && i + 1 < a.size()) file = a[++i];
    else if (a[i] == "--rights" && i + 1 < a.size()) rights = a[++i];
    else if (a[i] == "--source" && i + 1 < a.size()) source = a[++i];
    else if (a[i] == "--platform" && i + 1 < a.size()) platform = a[++i];
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
    {  // slice B1b: games installed from a pinned-key source are re-verified before every launch
      pnr::store::Registry preg(pnr::store::store_dir_for(dbp ? dbp : "./pnr_data/library.db"));
      std::string note;
      if (preg.load()) {
        auto pv = pnr::store::check_provenance(preg.dir(), preg, pos[0], &note);
        if (!pv) { std::printf("LAUNCH NOT STARTED: %s\n", pv.error.message.c_str()); return 1; }
      }
    }
    if (sandbox_on) {  // no silent fallback: an unusable sandbox means the game is not started
      auto sp = pnr::sandbox::probe();
      if (!sp.usable()) { std::printf("LAUNCH NOT STARTED: --sandbox was requested but the sandbox is not usable here (%s)\n", sp.notes.c_str()); return 1; }
      GameEntry sge;
      if (core.get(pos[0], &sge)) {  // an unknown id is reported by launch() itself, before anything is forked
        std::error_code aec;
        auto gd = std::filesystem::absolute(sge.install_path, aec).lexically_normal();
        std::string gds = gd.string(); while (gds.size() > 1 && gds.back() == '/') gds.pop_back();
        if (aec || gds.empty()) { std::printf("LAUNCH NOT STARTED: cannot resolve the game directory for the sandbox\n"); return 1; }
        // the game's jail root and the PS-ABI shim dir are what launch() gives it; both are env in the child
        core.set_child_setup([gds](std::string* e) {
          pnr::sandbox::Policy pol; pol.game_dir = gds;
          if (const char* j = std::getenv("PNR_JAIL_ROOT")) if (*j) pol.rw_dirs.push_back(std::filesystem::absolute(j).lexically_normal().string());
          if (const char* l = std::getenv("LD_LIBRARY_PATH")) { std::string s = l; size_t i = 0; while (i <= s.size()) { size_t k = s.find(':', i); if (k == std::string::npos) k = s.size(); if (k > i) pol.ro_dirs.push_back(std::filesystem::absolute(s.substr(i, k - i)).lexically_normal().string()); i = k + 1; } }
          return pnr::sandbox::enter(pol, e); });
      }
    }
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
  } else if (a[0] == "doctor" && pos.empty()) {
    auto r = core.doctor();
    std::fputs((json ? render_doctor_json(r) + "\n" : render_doctor_text(r)).c_str(), stdout);
    rc = r.ok ? 0 : 2;
  } else if (a[0] == "clean" && pos.empty() && orphans) {
    auto o = core.clean_orphans(yes);
    std::fputs((json ? render_clean_json(o) + "\n" : render_clean_text(o)).c_str(), stdout);
    bool bad = !o.ok;
    for (const auto& i : o.items) if (!i.error.empty()) bad = true;
    rc = bad ? 1 : o.needs_confirmation ? 3 : 0;
  } else if (a[0] == "clean" && pos.empty()) {
    std::fputs("launcher: clean needs --orphans (it removes only orphan install directories)\n", stderr);
    rc = 64;
  } else if (a[0] == "export" && pos.size() == 2) {
    auto o = core.export_saves(pos[0], pos[1]);
    std::fputs((json ? render_saves_json("export", o) + "\n" : render_saves_text("export", o)).c_str(), stdout);
    rc = o.ok ? 0 : 1;
  } else if (a[0] == "restore-saves" && pos.size() == 2) {
    auto o = core.restore_saves(pos[0], pos[1], overwrite);
    std::fputs((json ? render_saves_json("restore", o) + "\n" : render_saves_text("restore", o)).c_str(), stdout);
    rc = o.ok ? 0 : 1;
  } else if (a[0] == "sandbox" && pos.size() == 1 && pos[0] == "probe") {
    std::fputs(pnr::sandbox::describe(pnr::sandbox::probe()).c_str(), stdout);
    rc = 0;
  } else if (a[0] == "research" && !pos.empty()) {
    pnr::research::CliOptions ro; ro.rights = rights; ro.expect_sha256 = sha256_arg; ro.limit = limit_s; ro.json = json; ro.force = force; ro.log_it = log_it;
    std::string out;
    rc = pnr::research::cmd_research(pos, ro, pnr::store::store_dir_for(dbp ? dbp : "./pnr_data/library.db") + "/research", &out);
    std::fputs(out.c_str(), rc == 0 ? stdout : (rc == 64 ? stdout : stderr));
  } else if (a[0] == "store" && !pos.empty()) {
    pnr::store::Registry reg(pnr::store::store_dir_for(dbp ? dbp : "./pnr_data/library.db"));
    std::string out;
    auto ls = reg.load();
    if (!ls) { std::fprintf(stderr, "store: %s\n", ls.error.message.c_str()); rc = 1; }
    else {
      if (pos[0] == "source" && pos.size() == 4 && pos[1] == "add") rc = pnr::store::cmd_source_add(reg, pos[2], pos[3], rights, json, &out);
      else if (pos[0] == "source" && pos.size() == 4 && pos[1] == "pin") rc = pnr::store::cmd_source_pin(reg, pos[2], pos[3], json, &out);
      else if (pos[0] == "source" && pos.size() == 3 && pos[1] == "remove") rc = pnr::store::cmd_source_remove(reg, pos[2], yes, json, &out);
      else if (pos[0] == "source" && pos.size() == 2 && pos[1] == "list") rc = pnr::store::cmd_source_list(reg, json, &out);
      else if (pos[0] == "refresh" && pos.size() <= 2) rc = pnr::store::cmd_refresh(reg, pos.size() == 2 ? pos[1] : "", json, &out);
      else if (pos[0] == "search" && pos.size() <= 2) rc = pnr::store::cmd_search(reg, pos.size() == 2 ? pos[1] : "", source, json, &out);
      else if (pos[0] == "show" && pos.size() == 3) rc = pnr::store::cmd_show(reg, pos[1], pos[2], json, &out);
      else if (pos[0] == "get" && pos.size() == 3) { pnr::store::Queue qu(reg, reg.dir()); rc = pnr::store::cmd_get(qu, pos[1], pos[2], json, &out); }
      else if (pos[0] == "queue" && pos.size() >= 2) {
        pnr::store::Queue qu(reg, reg.dir());
        if (pos[1] == "list" && pos.size() == 2) rc = pnr::store::cmd_queue_list(qu, json, &out);
        else if (pos[1] == "run" && (pos.size() == 2 || pos.size() == 4)) rc = pnr::store::cmd_queue_run(qu, pos.size() == 4 ? pos[2] : "", pos.size() == 4 ? pos[3] : "", json, &out);
        else if ((pos[1] == "pause" || pos[1] == "resume") && pos.size() == 4) rc = pnr::store::cmd_queue_pause(qu, pos[2], pos[3], pos[1] == "pause", json, &out);
        else if (pos[1] == "cancel" && pos.size() == 4) rc = pnr::store::cmd_queue_cancel(qu, pos[2], pos[3], yes, json, &out);
        else usage();
      } else if (pos[0] == "scan" && pos.size() == 3) {
        pnr::store::Queue qu(reg, reg.dir());
        std::string path, sha;
        auto ps = qu.prepare_install(pos[1], pos[2], &path, &sha);
        if (!ps) { out = "store error: " + ps.error.message + "\n"; rc = 1; }
        else {
          pnr::store::Package spk; { pnr::store::Index sx; if (reg.load_index(pos[1], &sx)) for (auto& k : sx.packages) if (k.id == pos[2]) spk = k; }
          auto srep = pnr::scan::scan_package(path, sha, spk.size, pnr::scan::config_from_env());
          out = srep.text(); rc = srep.flagged() ? 1 : 0;
        }
      } else if (pos[0] == "install" && pos.size() == 3) {
        pnr::store::Queue qu(reg, reg.dir());
        std::string path, sha;
        auto ps = qu.prepare_install(pos[1], pos[2], &path, &sha);
        pnr::store::Package spk; { pnr::store::Index sx; if (reg.load_index(pos[1], &sx)) for (auto& k : sx.packages) if (k.id == pos[2]) spk = k; }
        auto srep = pnr::scan::scan_package(path, sha, spk.size, pnr::scan::config_from_env());
        if (ps) std::fputs(srep.text().c_str(), stdout);
        if (!ps) { out = "store error: " + ps.error.message + "\n"; rc = 1; }
        else if (srep.flagged() && !allow_flagged) { out = "install refused: a scan stage flagged this package. Review the scan above; --allow-flagged overrides, at your own risk. A clean scan never proves safety either.\n"; rc = 1; }
        else {
          auto o = core.import_package(path);
          std::fputs((json ? render_import_json(o) + "\n" : render_import_text(o)).c_str(), stdout);
          if (o.ok) {
            qu.mark_installed(pos[1], pos[2], o.game_id);
            out = "installed from source '" + pos[1] + "', package '" + pos[2] + "', sha256 " + sha + " (verified against the source's index; the importer then ran its own package checks)\nnew games start as Untested; nothing claims it is playable\n";
          }
          rc = o.ok ? 0 : 1;
        }
      }
      else usage();
      std::fputs(out.c_str(), stdout);
    }
  } else if (a[0] == "psn" && !pos.empty()) {
    const std::string tp = pnr::psn::token_path_for(dbp ? dbp : "./pnr_data/library.db");
    std::string out;
    if (pos[0] == "status" && pos.size() == 1) rc = pnr::psn::cmd_status(core.db(), tp, json, &out);
    else if (pos[0] == "set-token" && pos.size() == 1) {
      std::string in;
      if (!file.empty()) { std::ifstream f(file, std::ios::binary); in.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
      else in.assign((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
      rc = pnr::psn::cmd_set_token(tp, in, json, &out);
    }
    else if (pos[0] == "import-json" && pos.size() == 2) rc = pnr::psn::cmd_import_json(core.db(), pos[1], complete, json, &out);
    else if (pos[0] == "sync" && pos.size() == 1) rc = pnr::psn::cmd_sync(core.db(), tp, complete, json, &out);
    else if (pos[0] == "owned" && pos.size() == 1) rc = pnr::psn::cmd_owned(core.db(), json, platform, &out);
    else if (pos[0] == "link" && pos.size() == 3) rc = pnr::psn::cmd_link(core.db(), pos[1], pos[2], json, &out);
    else if (pos[0] == "unlink" && pos.size() == 2) rc = pnr::psn::cmd_unlink(core.db(), pos[1], json, &out);
    else if (pos[0] == "disconnect" && pos.size() == 1) rc = pnr::psn::cmd_disconnect(core.db(), tp, forget, yes, json, &out);
    else usage();
    std::fputs(out.c_str(), stdout);
  } else usage();
  core.close();
  return rc;
}
