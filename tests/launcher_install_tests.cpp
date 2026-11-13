// Launcher install/remove/verify tests (headless): import into the
// launcher's own library directory, destructive removal with strict path
// containment, on-disk verification. Real packages, real files, hostile
// database rows and symlinks.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "launcher/launcher_core.hpp"
#include "package/importer.hpp"

namespace {
void spit(const std::string& p, const std::string& s) {
  std::filesystem::create_directories(std::filesystem::path(p).parent_path());
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f << s;
}
}  // namespace

void pnr_launcher_install_tests(int& pass, int& fail) {
  using namespace pnr::launcher;
  namespace fs = std::filesystem;
  auto check = [&](bool ok, const std::string& label) { if (ok) ++pass; else ++fail; std::printf("%s %s\n", ok ? "PASS" : "FAIL", label.c_str()); };
  const std::string dir = "/tmp/pnr_launcher_inst_" + std::to_string(::getpid());
  fs::remove_all(dir); fs::create_directories(dir);
  std::string demo = "/bin/true";
#ifdef PNR_PS_DEMO_PATH
  demo = PNR_PS_DEMO_PATH;
#endif
  const std::string root = dir + "/lib";
  auto mk = [&](const std::string& out, const std::string& id, const std::string& ver, const std::string& title = "T") {
    pnr::pkg::Manifest m; m.id = id; m.title = title; m.version = ver; m.executable = "bin/game";
    return pnr::pkg::build_package(out, m, {{"bin/game", demo}}).ok;
  };

  // ---- ids
  for (const char* ok : {"a", "ps-demo", "g.1_x", "A9"}) check(valid_game_id(ok), std::string("inst: valid id: ") + ok);
  for (const char* bad : {"", ".", "..", "a/b", "../x", "a b", "a\n", "é"}) check(!valid_game_id(bad), std::string("inst: invalid id rejected: [") + bad + "]");
  check(!valid_game_id(std::string(129, 'a')) && valid_game_id(std::string(128, 'a')), "inst: id length limit is 128");

  LauncherCore core;
  check(!core.import_package(dir + "/x.pnrgame").ok, "inst: import without an open library refused");
  check(!core.remove("a", false, true).found && !core.verify("a").found, "inst: remove/verify without an open library do nothing");
  check(core.open(root + "/library.db").ok, "inst: library opens in its own directory");

  // ---- import
  check(mk(dir + "/g1.pnrgame", "inst.g1", "1.0", "Game One") && mk(dir + "/g1v2.pnrgame", "inst.g1", "2.0", "Game One") && mk(dir + "/g2.pnrgame", "inst.g2", "1.0"), "inst: fixture packages built");
  auto i1 = core.import_package(dir + "/g1.pnrgame");
  check(i1.ok && i1.kind == "fresh" && i1.install_path == root + "/games/inst.g1-1.0" && fs::is_regular_file(root + "/games/inst.g1-1.0/bin/game"), "inst: fresh import installs under the launcher's own library directory");
  GameEntry e;
  check(core.get("inst.g1", &e) && e.runnable && e.compatibility == "Untested" && e.integrity == "verified", "inst: imported game is runnable, Untested, verified");
  check(core.set_compat("inst.g1", "Playable With Issues", "kept").ok, "inst: person sets a state");
  auto i2 = core.import_package(dir + "/g1.pnrgame");
  check(i2.ok && i2.kind == "reinstall" && core.get("inst.g1", &e) && e.compatibility == "Playable With Issues" && e.compat_human_set, "inst: reinstall of the same version is reported and keeps the human-set state");
  auto i3 = core.import_package(dir + "/g1v2.pnrgame");
  check(i3.ok && i3.kind == "different-version" && i3.previous_version == "1.0" && i3.previous_path == root + "/games/inst.g1-1.0" && fs::exists(root + "/games/inst.g1-1.0") && fs::exists(root + "/games/inst.g1-2.0"), "inst: different version is reported; the old tree is left in place, not deleted");
  check(render_import_text(i3).find("left at") != std::string::npos && render_import_json(i3).find("\"kind\":\"different-version\"") != std::string::npos && render_import_json(i3).find('\n') == std::string::npos, "inst: import renders text and one-line JSON");
  check(core.import_package(dir + "/g2.pnrgame").ok && core.list().size() == 2, "inst: second game imports");

  // import rejections leave library and disk untouched
  const auto before_games = core.list().size();
  std::ifstream pf(dir + "/g2.pnrgame", std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(pf)), std::istreambuf_iterator<char>());
  std::string flipped = bytes; flipped[flipped.size() / 2] ^= 0xFF;
  spit(dir + "/corrupt.pnrgame", flipped);
  spit(dir + "/garbage.pnrgame", "not a tar");
  spit(dir + "/zero.pnrgame", "");
  check(mk(dir + "/dotdot.pnrgame", "..", "1.0") && mk(dir + "/dot.pnrgame", ".", "1.0") && mk(dir + "/slash.pnrgame", "a/b", "1.0"), "inst: hostile-id packages built");
  for (const char* f : {"corrupt", "garbage", "zero", "dotdot", "dot", "slash", "missing"}) {
    auto o = core.import_package(dir + "/" + f + ".pnrgame");
    check(!o.ok && !o.error.empty() && render_import_text(o).find("REFUSED") != std::string::npos, std::string("inst: import refused: ") + f);
  }
  check(!core.import_package("").ok && !core.import_package(dir).ok, "inst: import of an empty path and a directory refused");
  check(core.list().size() == before_games, "inst: refused imports added no library rows");
  check(!fs::exists(root + "/games/..-1.0") && !fs::exists(root + "/games/.-1.0") && !fs::exists(root + "/games/a"), "inst: refused imports created no install trees");
  { int n = 0; for (auto& x : fs::directory_iterator(root + "/games")) { (void)x; ++n; } check(n == 3, "inst: games dir holds exactly the 3 good installs"); }

  // ---- remove: dry run, confirm, keep-saves, rows
  spit(root + "/jail/inst.g2/saves/slot1.sav", "SAVE");
  spit(root + "/jail/inst.g1/saves/slot1.sav", "SAVE1");
  GameEntry g2; core.get("inst.g2", &g2);
  auto d0 = core.remove("inst.g2", false, false);
  check(d0.found && d0.needs_confirmation && !d0.removed && d0.install_exists && d0.jail_exists && fs::exists(g2.install_path) && fs::exists(root + "/jail/inst.g2/saves/slot1.sav") && core.get("inst.g2", &g2), "inst: remove without --yes is a dry run that deletes nothing");
  check(render_remove_text("inst.g2", d0).find("add --yes") != std::string::npos, "inst: dry run says what to add");
  core.get("inst.g2", &g2);
  const std::string g2path = g2.install_path;
  check(!core.remove("inst.g2", false, false).removed && core.get("inst.g2", &g2), "inst: repeated dry run still deletes nothing");
  pnr::library::GameDb db; db.open(root + "/library.db");
  db.record_launch("inst.g2", 0, 0.1);
  auto rm = core.remove("inst.g2", false, true);
  check(rm.removed && !fs::exists(g2path) && !fs::exists(root + "/jail/inst.g2") && !core.get("inst.g2", &g2), "inst: confirmed remove deletes install tree, jail folder and the library row");
  check(db.launch_count("inst.g2") == 0 && !db.get_installation("inst.g2") && !db.get_compatibility("inst.g2") && !db.get_game("inst.g2"), "inst: all four tables are cleared, history included");
  check(fs::exists(dir + "/g2.pnrgame"), "inst: the source .pnrgame is never touched");
  check(fs::exists(root + "/jail/inst.g1/saves/slot1.sav") && core.get("inst.g1", &e), "inst: other games are untouched");
  auto rm2 = core.remove("inst.g2", false, true);
  check(!rm2.found && !rm2.removed && render_remove_text("inst.g2", rm2).find("NOT DONE") != std::string::npos, "inst: removing twice reports no such game");
  check(!core.remove("nope", false, true).found && !core.remove("", false, true).found && !core.remove("..", false, true).found && !core.remove("a/b", false, true).found && !core.remove("../lib", false, true).found, "inst: unknown or invalid ids are not found and delete nothing");
  check(fs::exists(root + "/games") && fs::exists(root + "/library.db") && fs::exists(root + "/jail"), "inst: library root, games dir and jail dir survive hostile ids");
  // keep-saves
  GameEntry g1; core.get("inst.g1", &g1);
  auto rk = core.remove("inst.g1", true, true);
  check(rk.removed && !fs::exists(g1.install_path) && fs::exists(root + "/jail/inst.g1/saves/slot1.sav") && !core.get("inst.g1", &g1), "inst: --keep-saves keeps the jail folder but removes the install tree and rows");
  check(render_remove_text("inst.g1", rk).find("kept") != std::string::npos && render_remove_json("inst.g1", rk).find("\"removed\":true") != std::string::npos, "inst: remove renders text and JSON");

  // ---- remove: hostile database rows and symlinks must refuse, delete nothing
  const std::string outside = dir + "/outside";
  spit(outside + "/precious.txt", "KEEP");
  auto hostile = [&](const std::string& id, const std::string& path, const std::string& label) {
    db.add_game({0, id, "H", "1", "pnr-dev"});
    db.set_installation({id, path, "1", "verified", "bin/game"});
    auto o = core.remove(id, false, true);
    check(o.found && o.refused && !o.removed && !o.error.empty() && fs::exists(outside + "/precious.txt") && core.get(id, &e), "inst: " + label + ": refused, nothing deleted, row kept");
    check(render_remove_text(id, o).find("nothing was deleted") != std::string::npos, "inst: " + label + ": text says nothing deleted");
  };
  hostile("h.outside", outside, "install path outside the library");
  hostile("h.parent", dir, "install path is the library's parent");
  hostile("h.root", root, "install path is the library root");
  hostile("h.games", root + "/games", "install path is the games directory itself");
  hostile("h.dotdot", root + "/games/../..", "install path with .. traversal");
  hostile("h.deep", root + "/games/x/../../../outside", "install path escaping via ..");
  fs::create_directories(root + "/games/real");
  fs::create_symlink(outside, root + "/games/linkdir");
  hostile("h.symlink", root + "/games/linkdir", "install dir is a symlink to outside");
  fs::create_directories(root + "/games/nested/child");
  hostile("h.nested", root + "/games/nested/child", "install path two levels below games");
  fs::create_symlink(outside, root + "/jail/h.jail");
  db.add_game({0, "h.jail", "H", "1", "pnr-dev"});
  db.set_installation({"h.jail", root + "/games/real", "1", "verified", "bin/game"});
  auto oj = core.remove("h.jail", false, true);
  check(oj.refused && !oj.removed && fs::exists(outside + "/precious.txt") && fs::exists(root + "/games/real") && core.get("h.jail", &e), "inst: jail folder that is a symlink to outside is refused, nothing deleted");
  // missing install tree: records still go
  db.add_game({0, "h.gone", "G", "1", "pnr-dev"});
  db.set_installation({"h.gone", root + "/games/h.gone-1", "1", "verified", "bin/game"});
  auto og = core.remove("h.gone", false, true);
  check(og.removed && !og.install_exists && !core.get("h.gone", &e) && render_remove_text("h.gone", og).find("already gone") != std::string::npos, "inst: an install tree that is already gone does not block removing the records");
  db.add_game({0, "h.noinst", "N", "1", "pnr-dev"});
  check(core.remove("h.noinst", false, true).removed && !core.get("h.noinst", &e), "inst: a game with no installation record is removable");

  // ---- verify
  check(mk(dir + "/v.pnrgame", "inst.v", "1.0") && core.import_package(dir + "/v.pnrgame").ok, "inst: verify fixture imported");
  check(core.set_compat("inst.v", "Verified Playable", "ok").ok, "inst: verify fixture has a human state");
  auto v0 = core.verify("inst.v");
  check(v0.found && v0.ok && v0.problems.empty() && !v0.marked_failed && render_verify_text("inst.v", v0).find("not re-checked") != std::string::npos && render_verify_json("inst.v", v0).find("\"checksums_rechecked\":false") != std::string::npos, "inst: healthy install verifies, and says checksums are not re-checked");
  check(!core.verify("nope").found && !core.verify("").found && render_verify_text("nope", core.verify("nope")).find("NOT DONE") != std::string::npos, "inst: verify of an unknown id is not found");
  GameEntry v; core.get("inst.v", &v);
  const std::string exe = v.install_path + "/bin/game";
  std::string saved; { std::ifstream f(exe, std::ios::binary); saved.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()); }
  fs::remove(exe);
  auto v1 = core.verify("inst.v");
  check(v1.found && !v1.ok && v1.marked_failed && !v1.problems.empty() && render_verify_text("inst.v", v1).find("VERIFY FAILED") != std::string::npos, "inst: deleted entry point fails verification and marks integrity failed");
  check(core.get("inst.v", &v) && !v.runnable && v.integrity == "failed" && v.compatibility == "Verified Playable" && v.compat_human_set, "inst: failed game is no longer runnable; its human compatibility state is untouched");
  check(!core.launch("inst.v").attempted, "inst: launch of a failed game is refused");
  spit(exe, saved);
  auto v2 = core.verify("inst.v");
  check(v2.ok && !v2.marked_failed && core.get("inst.v", &v) && v.integrity == "failed" && !v.runnable, "inst: verify never restores integrity to verified (checksums cannot be re-checked)");
  spit(exe, saved.substr(0, 20));
  check(!core.verify("inst.v").ok, "inst: truncated ELF fails verification");
  std::string arm = saved; arm[18] = char(0xB7); arm[19] = 0;
  spit(exe, arm);
  auto v3 = core.verify("inst.v");
  check(!v3.ok && v3.problems[0].find("x86-64") != std::string::npos, "inst: non-x86-64 entry fails verification");
  spit(exe, "garbage text");
  check(!core.verify("inst.v").ok, "inst: non-ELF entry fails verification");
  fs::remove_all(v.install_path);
  check(!core.verify("inst.v").ok && core.verify("inst.v").problems[0].find("missing") != std::string::npos, "inst: missing install directory fails verification");
  db.add_game({0, "v.escape", "E", "1", "pnr-dev"});
  fs::create_directories(dir + "/vi"); spit(dir + "/vi/ok", saved);
  db.set_installation({"v.escape", dir + "/vi/in", "1", "verified", "../ok"});
  fs::create_directories(dir + "/vi/in");
  check(!core.verify("v.escape").ok && core.verify("v.escape").problems[0].find("outside") != std::string::npos, "inst: entry point escaping the install directory fails verification");
  db.add_game({0, "v.none", "N", "1", "pnr-dev"});
  auto vn = core.verify("v.none");
  check(vn.found && !vn.ok && !vn.marked_failed && vn.problems[0] == "not installed", "inst: game without an installation fails verification, nothing marked");
  db.add_game({0, "v.noexe", "N", "1", "pnr-dev"});
  fs::create_directories(dir + "/vi/noexe");
  db.set_installation({"v.noexe", dir + "/vi/noexe", "1", "verified", ""});
  check(!core.verify("v.noexe").ok, "inst: installation without an entry point fails verification");

  core.close(); db.close();
  fs::remove_all(dir);
}
