// Launcher library-management tests (headless): human-set compatibility
// rules, launch history, read-only directory scan, rejection and mutation
// cases, old-schema migration. Real launches, real ELF/package files.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "launcher/launcher_core.hpp"
#include "library/sqlite3_shim.h"
#include "package/importer.hpp"

namespace {
std::vector<uint8_t> slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
void spit(const std::string& p, const std::vector<uint8_t>& b) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}
std::string listing(const std::string& dir) {
  std::vector<std::string> v;
  for (auto& e : std::filesystem::directory_iterator(dir)) {
    std::error_code ec;
    v.push_back(e.path().filename().string() + ":" + (e.is_symlink() ? "L" : "F") + ":" +
                (e.is_regular_file(ec) ? std::to_string(std::filesystem::file_size(e.path())) : "0"));
  }
  std::sort(v.begin(), v.end());
  std::string s;
  for (auto& x : v) s += x + "|";
  return s;
}
const ::pnr::launcher::ScanItem* find_item(const ::pnr::launcher::ScanReport& r, const std::string& f) {
  for (auto& i : r.items) if (i.file == f) return &i;
  return nullptr;
}
}  // namespace

void pnr_launcher_mgmt_tests(int& pass, int& fail) {
  using namespace pnr::launcher;
  namespace fs = std::filesystem;
  auto check = [&](bool ok, const std::string& label) { if (ok) ++pass; else ++fail; std::printf("%s %s\n", ok ? "PASS" : "FAIL", label.c_str()); };
  const std::string dir = "/tmp/pnr_launcher_mgmt_" + std::to_string(::getpid());
  fs::remove_all(dir); fs::create_directories(dir);
  std::string demo_dir = "/bin", demo_exe = "true", demo_path = "/bin/true";
#ifdef PNR_PS_DEMO_PATH
  demo_path = PNR_PS_DEMO_PATH;
  demo_dir = fs::path(demo_path).parent_path().string();
  demo_exe = fs::path(demo_path).filename().string();
#endif
  std::vector<std::string> env = {"PNR_JAIL_ROOT=" + dir + "/jail"};
#ifdef PNR_PSABI_DIR
  env.push_back(std::string("LD_LIBRARY_PATH=") + PNR_PSABI_DIR);
#endif

  // ---- state names and the set_compat rules
  check(compat_states().size() == 6 && valid_compat_state("Verified Playable") && valid_compat_state("Untested"), "mgmt: six known states");
  for (const char* bad : {"", "playable", "verified playable", "Verified Playable ", "Playable", "Launches  Only", "Untested\n"})
    check(!valid_compat_state(bad), std::string("mgmt: invalid state name rejected: [") + bad + "]");

  LauncherCore core;
  check(!core.set_compat("g", "Untested", "").ok, "mgmt: set_compat without an open library fails");
  std::vector<pnr::library::LaunchRow> rows;
  check(!core.history("g", 5, &rows), "mgmt: history without an open library is false");
  ScanReport rep0;
  check(core.scan(dir, &rep0).ok, "mgmt: scan needs no open library (empty dir ok)");
  check(rep0.items.empty() && rep0.skipped == 0 && !rep0.truncated, "mgmt: empty directory scans to nothing");
  check(core.open(dir + "/library.db").ok, "mgmt: library opens");
  pnr::library::GameDb db;
  check(db.open(dir + "/library.db").ok, "mgmt: second handle opens");
  db.add_game({0, "m.a", "A", "1", "pnr-dev"});
  db.set_installation({"m.a", demo_dir, "1", "verified", demo_exe});
  db.add_game({0, "m.b", "B", "1", "pnr-dev"});
  db.set_installation({"m.b", demo_dir, "1", "verified", demo_exe});
  db.add_game({0, "m.c", "C", "1", "pnr-dev"});
  db.set_installation({"m.c", demo_dir, "1", "verified", demo_exe});
  db.add_game({0, "m.none", "N", "1", "pnr-dev"});

  check(!core.set_compat("nope", "Untested", "").ok, "mgmt: set_compat on unknown id rejected");
  check(!core.set_compat("m.A", "Untested", "").ok && !core.set_compat("", "Untested", "").ok, "mgmt: set_compat id match is exact (case, empty)");
  check(!core.set_compat("m.a", "Playable", "").ok && !core.set_compat("m.a", "", "").ok, "mgmt: set_compat rejects unknown or empty state");
  check(!core.set_compat("m.a", "Untested", std::string(501, 'x')).ok && core.set_compat("m.a", "Untested", std::string(500, 'x')).ok, "mgmt: note limit is 500 bytes (501 rejected, 500 accepted)");
  check(!core.set_compat("m.a", "Untested", "bad\nnote").ok && !core.set_compat("m.a", "Untested", std::string("bad\x01") + "x").ok && !core.set_compat("m.a", "Untested", "del\x7f").ok, "mgmt: notes with control characters rejected");
  check(core.set_compat("m.a", "Untested", "").ok, "mgmt: empty note accepted");
  GameEntry e;
  // refused calls changed nothing: m.b still untouched
  check(core.get("m.b", &e) && e.compatibility == "Untested" && !e.compat_human_set, "mgmt: rejected calls changed nothing");

  // ---- automatic launches never overwrite a human-set state, even Untested
  check(core.get("m.a", &e) && e.compat_human_set && e.compatibility == "Untested", "mgmt: human-set Untested is marked human-set");
  auto la = core.launch("m.a", {}, env);
  check(la.error.empty() && la.recorded && !la.compat_advanced, "mgmt: launch of a human-set Untested game is recorded but does not advance it");
  check(core.get("m.a", &e) && e.compatibility == "Untested" && e.launch_count == 1, "mgmt: human-set Untested stays Untested after a clean launch");
  auto lb = core.launch("m.b", {}, env);
  check(lb.compat_advanced && core.get("m.b", &e) && e.compatibility == "Launches Only" && !e.compat_human_set, "mgmt: an untouched game still advances automatically, not marked human-set");
  check(core.set_compat("m.c", "Verified Playable", "played 1h").ok, "mgmt: a person can set Verified Playable");
  auto lc = core.launch("m.c", {}, env);
  check(lc.error.empty() && !lc.compat_advanced && core.get("m.c", &e) && e.compatibility == "Verified Playable" && e.compatibility_notes == "played 1h" && e.compat_human_set, "mgmt: launch leaves a human Verified Playable and its note alone");
  check(core.set_compat("m.c", "Playable With Issues", "").ok && core.get("m.c", &e) && e.compatibility == "Playable With Issues", "mgmt: a person can change a human-set state");
  // no automatic outcome is ever a Playable state
  bool auto_playable = false;
  for (const char* id : {"m.a", "m.b", "m.c", "m.none"}) { GameEntry x; if (core.get(id, &x) && !x.compat_human_set && x.compatibility.find("Playable") != std::string::npos) auto_playable = true; }
  check(!auto_playable, "mgmt: no non-human state is ever a Playable state");
  check(core.get("m.none", &e) && e.compatibility == "Untested" && !e.compat_human_set, "mgmt: game with no compatibility row reads as Untested, automatic");
  check(core.set_compat("m.none", "Unsupported", "no binary") && core.get("m.none", &e) && e.compat_human_set && e.compatibility == "Unsupported", "mgmt: human set works on a game that had no compatibility row");
  // the automatic db path clears the mark (re-import semantics)
  db.set_compatibility({"m.a", "Untested", "auto"});
  check(!db.compat_is_human_set("m.a") && !db.compat_is_human_set("nope"), "mgmt: automatic write clears the human mark; unknown id is not human-set");
  db.set_compatibility_human({"m.a", "Blocked by Missing Dependency", "needs lib"});
  check(db.compat_is_human_set("m.a") && db.get_compatibility("m.a")->state == "Blocked by Missing Dependency", "mgmt: db-level human write marks and stores");

  // ---- history
  for (int i = 0; i < 3; ++i) core.launch("m.b", {}, env);
  check(core.history("m.b", 100, &rows) && rows.size() == 4 && rows[0].at >= rows[3].at, "mgmt: history lists every launch, newest first");
  check(core.history("m.b", 2, &rows) && rows.size() == 2, "mgmt: history limit applies");
  check(core.history("m.b", 0, &rows) && rows.size() == 1 && core.history("m.b", -5, &rows) && rows.size() == 1, "mgmt: history limit below 1 clamps to 1");
  check(core.history("m.b", 1000000, &rows) && rows.size() == 4, "mgmt: history limit above 1000 clamps, no overflow");
  check(core.history("m.none", 5, &rows) && rows.empty(), "mgmt: never-launched game has empty history");
  check(!core.history("nope", 5, &rows) && !core.history("m.b", 5, nullptr), "mgmt: history refuses unknown id and null output");
  core.get("m.b", &e);
  core.history("m.b", 3, &rows);
  const std::string ht = render_history_text(e, rows), hj = render_history_json(e, rows);
  check(ht.find("exit 0") != std::string::npos && hj.find("\"launch_count\":4") != std::string::npos && hj.find("\"exit_code\":0") != std::string::npos && hj.find('\n') == std::string::npos, "mgmt: history renders text and one-line JSON");
  core.get("m.none", &e); core.history("m.none", 3, &rows);
  check(render_history_text(e, rows).find("no launches recorded") != std::string::npos, "mgmt: empty history says so");
  auto fl = core.launch("m.nofile");
  check(!fl.found && !fl.recorded, "mgmt: unknown id launch leaves no history");

  // ---- old-schema migration: a library written before the human mark existed
  {
    const std::string old = dir + "/old.db";
    sqlite3* h = nullptr;
    check(sqlite3_open_v2(old.c_str(), &h, 0x2 | 0x4, nullptr) == 0, "mgmt: old-schema db created");
    const char* sql = "CREATE TABLE games(game_id TEXT PRIMARY KEY, title TEXT NOT NULL, version TEXT NOT NULL, platform TEXT NOT NULL);"
                      "CREATE TABLE installations(game_id TEXT PRIMARY KEY, path TEXT NOT NULL, version TEXT NOT NULL, integrity TEXT NOT NULL, executable TEXT NOT NULL DEFAULT '');"
                      "CREATE TABLE compatibility(game_id TEXT PRIMARY KEY, state TEXT NOT NULL, notes TEXT);"
                      "CREATE TABLE launches(id INTEGER PRIMARY KEY AUTOINCREMENT, game_id TEXT NOT NULL, exit_code INTEGER NOT NULL, wall_seconds REAL NOT NULL, at REAL NOT NULL);"
                      "INSERT INTO games VALUES('o.g','Old','1','pnr-dev');"
                      "INSERT INTO compatibility VALUES('o.g','Launches Only','legacy');";
    check(sqlite3_exec(h, sql, nullptr, nullptr, nullptr) == 0, "mgmt: old-schema rows written");
    sqlite3_close_v2(h);
    LauncherCore oc;
    check(oc.open(old).ok, "mgmt: old-schema library opens and migrates");
    GameEntry oe;
    check(oc.get("o.g", &oe) && oe.compatibility == "Launches Only" && oe.compatibility_notes == "legacy" && !oe.compat_human_set, "mgmt: legacy compatibility row survives migration as automatic");
    check(oc.set_compat("o.g", "Unsupported", "x").ok && oc.get("o.g", &oe) && oe.compat_human_set, "mgmt: migrated library accepts human state");
    oc.close();
    LauncherCore oc2;
    check(oc2.open(old).ok && oc2.get("o.g", &oe) && oe.compat_human_set, "mgmt: reopening an already-migrated library is fine and keeps the mark");
    oc2.close();
  }

  // ---- scan: read-only classification
  const std::string sd = dir + "/scan";
  fs::create_directories(sd + "/subdir");
  auto elf = slurp(demo_path);
  check(elf.size() > 64, "mgmt: demo ELF readable for scan fixtures");
  spit(sd + "/good.elf", elf);
  spit(sd + "/trunc.bin", std::vector<uint8_t>(elf.begin(), elf.begin() + 20));
  auto arm = elf; arm[18] = 0xB7; arm[19] = 0x00;  // e_machine = aarch64
  spit(sd + "/arm.bin", arm);
  spit(sd + "/empty.bin", {});
  spit(sd + "/notes.txt", {'h', 'i', '\n'});
  spit(sd + "/we\"ird.elf", elf);
  fs::create_symlink(sd + "/good.elf", sd + "/link.elf");
  pnr::pkg::Manifest man; man.id = "scan.pkg"; man.title = "Scan Pkg"; man.version = "1.0"; man.executable = "bin/game";
  check(pnr::pkg::build_package(sd + "/ok.pnrgame", man, {{"bin/game", demo_path}}).ok, "mgmt: scan fixture package built");
  auto pk = slurp(sd + "/ok.pnrgame");
  auto bad = pk; bad[bad.size() / 2] ^= 0xFF;
  spit(sd + "/corrupt.pnrgame", bad);
  spit(sd + "/garbage.pnrgame", {'n', 'o', 't', ' ', 'a', ' ', 't', 'a', 'r'});
  spit(sd + "/zero.pnrgame", {});
  const std::string before = listing(sd);
  const auto games_before = core.list().size();
  ScanReport rep;
  check(core.scan(sd, &rep).ok, "mgmt: scan succeeds on a mixed directory");
  const auto* g = find_item(rep, "good.elf");
  check(g && g->kind == "elf" && g->ok && g->has_imports && g->host + g->prx_implemented + g->prx_declared + g->unknown + g->pnr_extension > 0, "mgmt: scan classifies a real ELF's imports");
  check(find_item(rep, "we\"ird.elf") && find_item(rep, "we\"ird.elf")->ok, "mgmt: odd file name scanned");
  const auto* t = find_item(rep, "trunc.bin");
  check(t && t->kind == "elf" && !t->ok && !t->error.empty(), "mgmt: truncated ELF reported, not crashed");
  const auto* a = find_item(rep, "arm.bin");
  check(a && !a->ok && a->error.find("x86-64") != std::string::npos, "mgmt: non-x86-64 ELF rejected with a reason");
  check(!find_item(rep, "empty.bin") && !find_item(rep, "notes.txt") && !find_item(rep, "link.elf") && !find_item(rep, "subdir"), "mgmt: non-ELF files, symlinks and directories are not inspected");
  check(rep.skipped == 2, "mgmt: scan counts the 2 other regular files as skipped (empty.bin, notes.txt)");
  const auto* p = find_item(rep, "ok.pnrgame");
  check(p && p->kind == "pnrgame" && p->ok && p->game_id == "scan.pkg" && p->version == "1.0" && !p->in_library, "mgmt: valid package accepted, id read, not in library");
  const auto* c = find_item(rep, "corrupt.pnrgame");
  check(c && !c->ok && !c->error.empty(), "mgmt: corrupted package rejected (checksum or structure)");
  check(find_item(rep, "garbage.pnrgame") && !find_item(rep, "garbage.pnrgame")->ok && find_item(rep, "zero.pnrgame") && !find_item(rep, "zero.pnrgame")->ok, "mgmt: garbage and empty .pnrgame rejected");
  check(rep.items.size() == 8, "mgmt: 8 inspected (good, weird, trunc, arm, ok, corrupt, garbage, zero)");
  check(listing(sd) == before, "mgmt: scan changed nothing in the scanned directory (no extraction)");
  check(core.list().size() == games_before && !fs::exists(dir + "/games"), "mgmt: scan did not import anything or create install trees");
  // package already in library is flagged
  db.add_game({0, "scan.pkg", "Scan Pkg", "1.0", "pnr-dev"});
  ScanReport rep2; core.scan(sd, &rep2);
  check(find_item(rep2, "ok.pnrgame") && find_item(rep2, "ok.pnrgame")->in_library, "mgmt: scan flags a package whose id is already in the library");
  const std::string st = render_scan_text(rep2), sj = render_scan_json(rep2);
  check(st.find("read-only") != std::string::npos && st.find("REJECTED") != std::string::npos && sj.find("we\\\"ird.elf") != std::string::npos && sj.find('\n') == std::string::npos && sj.find("\"inspected\":8") != std::string::npos, "mgmt: scan renders text and escaped one-line JSON, states it is read-only");
  ScanReport bad1;
  check(!core.scan(dir + "/missing", &bad1).ok && !core.scan(sd + "/good.elf", &bad1).ok && !core.scan("", &bad1).ok, "mgmt: scan of a missing path, a file and an empty string is refused");
  check(!core.scan(sd, nullptr).ok, "mgmt: scan with a null report refused");
  fs::create_directories(dir + "/emptyd");
  ScanReport rep3; check(core.scan(dir + "/emptyd", &rep3).ok && rep3.items.empty() && rep3.skipped == 0, "mgmt: empty directory scans clean");
  // truncation cap
  fs::create_directories(dir + "/big");
  for (size_t i = 0; i < kScanMaxEntries + 5; ++i) spit(dir + "/big/f" + std::to_string(i), {1});
  ScanReport rep4; check(core.scan(dir + "/big", &rep4).ok && rep4.truncated && rep4.skipped <= static_cast<int>(kScanMaxEntries), "mgmt: scan stops at the entry cap and says so");

  core.close(); db.close();
  fs::remove_all(dir);
}
