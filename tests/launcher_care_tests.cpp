// Launcher diagnostics and saves backup tests (headless): doctor, clean,
// export, restore-saves. Decoy files, symlinks, hostile archive names,
// truncated and oversize archives. Real packages and files.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "launcher/launcher_core.hpp"
#include "package/importer.hpp"
#include "package/tar.hpp"

namespace {
namespace fs = std::filesystem;
void spit(const std::string& p, const std::string& s) {
  fs::create_directories(fs::path(p).parent_path());
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f << s;
}
std::string slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
// Sorted "relative:kind:size" listing, symlinks not followed.
std::string tree(const std::string& dir) {
  std::vector<std::string> v;
  std::error_code ec;
  if (!fs::exists(fs::symlink_status(dir, ec))) return "<absent>";
  for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const auto st = it->symlink_status();
    std::string k = fs::is_symlink(st) ? "L" : fs::is_directory(st) ? "D" : "F";
    v.push_back(fs::relative(it->path(), dir).string() + ":" + k + ":" + (k == "F" ? std::to_string(fs::file_size(it->path())) : "0"));
  }
  std::sort(v.begin(), v.end());
  std::string s;
  for (auto& x : v) s += x + "|";
  return s;
}
bool has_name(const std::vector<pnr::launcher::DoctorOrphan>& v, const std::string& n, const std::string& kind) {
  for (auto& o : v) if (o.name == n && o.kind == kind) return true;
  return false;
}
bool has_issue(const pnr::launcher::DoctorReport& r, const std::string& kind, const std::string& id) {
  for (auto& i : r.issues) if (i.kind == kind && i.subject == id) return true;
  return false;
}
bool write_tar(const std::string& out, const std::vector<std::pair<std::string, std::string>>& files) {
  std::vector<pnr::pkg::TarEntry> es;
  for (auto& f : files) es.push_back({f.first, std::vector<uint8_t>(f.second.begin(), f.second.end())});
  return pnr::pkg::tar_write(out, es).ok;
}
}  // namespace

void pnr_launcher_care_tests(int& pass, int& fail) {
  using namespace pnr::launcher;
  auto check = [&](bool ok, const std::string& label) { if (ok) ++pass; else ++fail; std::printf("%s %s\n", ok ? "PASS" : "FAIL", label.c_str()); };
  const std::string dir = "/tmp/pnr_launcher_care_" + std::to_string(::getpid());
  fs::remove_all(dir); fs::create_directories(dir);
  std::string demo = "/bin/true";
#ifdef PNR_PS_DEMO_PATH
  demo = PNR_PS_DEMO_PATH;
#endif
  const std::string root = dir + "/lib", outside = dir + "/outside";
  auto mk = [&](const std::string& out, const std::string& id, const std::string& ver) {
    pnr::pkg::Manifest m; m.id = id; m.title = id; m.version = ver; m.executable = "bin/game";
    return pnr::pkg::build_package(out, m, {{"bin/game", demo}}).ok;
  };

  // ---- archive names
  for (const char* ok : {"a", "dir/file.sav", "a/b/c", "x.y", "..a", "a..", ".hidden"}) check(safe_archive_name(ok), std::string("care: safe name accepted: ") + ok);
  for (const char* bad : {"", "/abs", "../x", "a/../b", "a/..", "./a", "a/./b", "a//b", "a/", "..", ".", "a\\b", "/"}) check(!safe_archive_name(bad), std::string("care: unsafe name rejected: [") + bad + "]");
  check(!safe_archive_name(std::string("a\0b", 3)) && !safe_archive_name(std::string(101, 'a')) && safe_archive_name(std::string(100, 'a')), "care: NUL and over-100-byte names rejected, 100 accepted");

  LauncherCore core;
  check(!core.doctor().ok && !core.clean_orphans(true).ok && !core.export_saves("a", dir + "/x.tar").ok && !core.restore_saves("a", dir + "/x.tar", false).ok, "care: every command refuses without an open library");
  check(core.open(root + "/library.db").ok, "care: library opens");
  check(mk(dir + "/a1.pnrgame", "care.a", "1.0") && mk(dir + "/a2.pnrgame", "care.a", "2.0") && mk(dir + "/b.pnrgame", "care.b", "1.0"), "care: fixture packages built");
  check(core.import_package(dir + "/a1.pnrgame").ok && core.import_package(dir + "/b.pnrgame").ok, "care: two games imported");

  // ---- doctor on a healthy library
  auto d0 = core.doctor();
  check(d0.ok && d0.games == 2 && d0.issues.empty() && d0.orphan_installs.empty() && d0.orphan_jails.empty(), "care: healthy library has no issues and no orphans");
  check(d0.usage.size() == 2 && d0.usage[0].game_id == "care.a" && d0.usage[0].install_bytes > 0 && d0.usage[0].saves_bytes == 0, "care: usage lists installs with sizes, sorted by id");
  // a row path in other spellings must not look like an orphan
  pnr::library::GameDb db; db.open(root + "/library.db");
  GameEntry ea; core.get("care.b", &ea);
  auto orig = *db.get_installation("care.b");
  auto alt = orig; alt.path = root + "/games/../games/care.b-1.0/";
  db.set_installation(alt);
  check(core.doctor().orphan_installs.empty() && core.doctor().issues.empty(), "care: a row path with .. and a trailing slash is still recognized, not an orphan");
  check(core.clean_orphans(true).items.empty() && fs::exists(root + "/games/care.b-1.0/bin/game"), "care: clean does not delete an install whose row uses an odd path spelling");
  db.set_installation(orig);

  // ---- decoys
  check(core.import_package(dir + "/a2.pnrgame").ok, "care: second version of care.a (leaves the old tree behind)");
  spit(root + "/games/decoy-dir/payload.bin", std::string(1000, 'x'));
  spit(root + "/games/stray.txt", "hello");
  spit(outside + "/precious.txt", "KEEP");
  fs::create_symlink(outside, root + "/games/evil-link");
  spit(root + "/jail/ghost/saves/s1.sav", "GHOST");
  const std::string before_all = tree(root);
  auto d1 = core.doctor();
  check(d1.ok && has_name(d1.orphan_installs, "care.a-1.0", "dir") && has_name(d1.orphan_installs, "decoy-dir", "dir") && has_name(d1.orphan_installs, "stray.txt", "file") && has_name(d1.orphan_installs, "evil-link", "symlink"), "care: doctor finds the old-version tree and the decoys, with their kinds");
  check(!has_name(d1.orphan_installs, "care.a-2.0", "dir") && !has_name(d1.orphan_installs, "care.b-1.0", "dir") && d1.orphan_installs.size() == 4, "care: doctor does not list installed trees as orphans");
  bool link_bytes_zero = false, decoy_bytes = false;
  for (auto& o : d1.orphan_installs) { if (o.name == "evil-link") link_bytes_zero = o.bytes == 0; if (o.name == "decoy-dir") decoy_bytes = o.bytes == 1000; }
  check(link_bytes_zero && decoy_bytes, "care: a symlink's target is not counted; real sizes are");
  check(has_name(d1.orphan_jails, "ghost", "dir") && d1.orphan_jails.size() == 1, "care: doctor finds the orphan saves folder");
  check(tree(root) == before_all, "care: doctor changed nothing on disk");
  check(render_doctor_text(d1).find("read-only") != std::string::npos && render_doctor_json(d1).find("\"orphan_installs\"") != std::string::npos && render_doctor_json(d1).find('\n') == std::string::npos, "care: doctor renders text and one-line JSON");

  // ---- doctor row issues
  db.add_game({0, "care.gone", "G", "1", "pnr-dev"});
  db.set_installation({"care.gone", root + "/games/care.gone-1", "1", "verified", "bin/game"});
  db.add_game({0, "care.out", "O", "1", "pnr-dev"});
  db.set_installation({"care.out", outside, "1", "verified", "bin/game"});
  db.add_game({0, "care.fail", "F", "1", "pnr-dev"});
  db.set_installation({"care.fail", root + "/games/care.b-1.0", "1", "failed", "bin/game"});
  auto d2 = core.doctor();
  check(has_issue(d2, "install-missing", "care.gone") && has_issue(d2, "install-outside-library", "care.out") && has_issue(d2, "integrity-failed", "care.fail"), "care: doctor reports missing, outside-library and failed-integrity rows");
  check(fs::exists(outside + "/precious.txt"), "care: doctor never touched the outside path");
  db.remove_game("care.gone"); db.remove_game("care.out"); db.remove_game("care.fail");

  // ---- clean
  check(core.clean_orphans(true).ok, "care: clean runs");
  // (the call above already deleted; rebuild the decoys for the dry-run checks)
  spit(root + "/games/decoy-dir/payload.bin", std::string(1000, 'x'));
  fs::create_directories(root + "/games/care.a-1.0/bin");
  const std::string before_dry = tree(root);
  auto c0 = core.clean_orphans(false);
  check(c0.ok && c0.needs_confirmation && !c0.items.empty() && tree(root) == before_dry, "care: clean without --yes is a dry run that deletes nothing");
  check(render_clean_text(c0).find("add --yes") != std::string::npos, "care: dry run says what to add");
  auto c1 = core.clean_orphans(true);
  bool all_removed = !c1.items.empty();
  for (auto& i : c1.items) all_removed = all_removed && i.removed && i.error.empty();
  check(c1.ok && all_removed && !fs::exists(root + "/games/decoy-dir") && !fs::exists(root + "/games/care.a-1.0"), "care: confirmed clean deletes the orphan directories");
  check(fs::exists(root + "/games/stray.txt") && fs::is_symlink(root + "/games/evil-link") && c1.left_alone == 2, "care: orphan files and symlinks are left alone (2 reported)");
  check(fs::exists(outside + "/precious.txt") && slurp(root + "/jail/ghost/saves/s1.sav") == "GHOST", "care: the symlink target and orphan saves are untouched");
  GameEntry g;
  check(core.get("care.a", &g) && g.runnable && core.get("care.b", &g) && g.runnable && fs::exists(root + "/games/care.a-2.0/bin/game") && fs::exists(root + "/games/care.b-1.0/bin/game"), "care: installed games and their rows are untouched and still runnable");
  auto c2 = core.clean_orphans(true);
  check(c2.ok && c2.items.empty() && !c2.needs_confirmation && render_clean_text(c2).find("nothing to clean") != std::string::npos, "care: a second clean finds nothing");
  check(render_clean_json(c1).find("\"removed\":true") != std::string::npos && render_clean_json(c1).find('\n') == std::string::npos, "care: clean renders JSON");
  // kept saves of a removed game show up as orphan saves
  spit(root + "/jail/care.b/saves/k.sav", "K");
  check(core.remove("care.b", true, true).removed && has_name(core.doctor().orphan_jails, "care.b", "dir"), "care: --keep-saves leftovers are reported as orphan saves, not deleted");
  check(slurp(root + "/jail/care.b/saves/k.sav") == "K", "care: clean never touches saves");
  core.clean_orphans(true);
  check(slurp(root + "/jail/care.b/saves/k.sav") == "K" && fs::exists(root + "/jail/ghost"), "care: clean leaves every jail folder alone");

  // ---- export
  const std::string out1 = dir + "/saves_a.tar";
  check(!core.export_saves("care.a", out1).ok && !fs::exists(out1), "care: export of a game with no saves folder is refused, no file written");
  spit(root + "/jail/care.a/slot1.sav", "SLOT-ONE");
  spit(root + "/jail/care.a/profile/p.dat", std::string(5000, 'p'));
  spit(root + "/jail/care.a/profile/empty.dat", "");
  spit(outside + "/secret.txt", "SECRET");
  fs::create_symlink(outside + "/secret.txt", root + "/jail/care.a/sneaky-file");
  fs::create_symlink(outside, root + "/jail/care.a/sneaky-dir");
  const std::string saves_before = tree(root + "/jail/care.a");
  auto e1 = core.export_saves("care.a", out1);
  check(e1.ok && e1.files == 3 && e1.bytes == 8 + 5000 && e1.skipped == 2, "care: export archives the 3 regular files and skips the 2 symlinks");
  std::vector<pnr::pkg::TarEntry> back;
  check(pnr::pkg::tar_read(out1, back).ok && back.size() == 3, "care: the archive is a readable ustar with 3 entries");
  bool leaked = false, names_ok = true;
  for (auto& b : back) { std::string d(b.data.begin(), b.data.end()); if (d.find("SECRET") != std::string::npos) leaked = true; if (!safe_archive_name(b.name)) names_ok = false; }
  check(!leaked && names_ok, "care: nothing from outside the saves folder leaked in; every name is safe");
  check(tree(root + "/jail/care.a") == saves_before, "care: export did not modify the saves folder");
  auto e2 = core.export_saves("care.a", out1);
  check(!e2.ok && e2.error.find("overwrite") != std::string::npos, "care: export refuses to overwrite an existing file");
  check(!core.export_saves("care.a", root + "/jail/care.a/self.tar").ok && !fs::exists(root + "/jail/care.a/self.tar"), "care: export refuses an output path inside the saves folder");
  check(!core.export_saves("care.a", dir + "/no_such_dir/x.tar").ok && !core.export_saves("care.a", "").ok, "care: export refuses a missing output directory and an empty path");
  check(!core.export_saves("nope", dir + "/n.tar").ok && !core.export_saves("..", dir + "/n.tar").ok && !core.export_saves("a/b", dir + "/n.tar").ok && !fs::exists(dir + "/n.tar"), "care: export refuses unknown and invalid ids");
  spit(root + "/jail/care.a/" + std::string(60, 'd') + "/" + std::string(60, 'f') + ".sav", "LONG");
  check(!core.export_saves("care.a", dir + "/long.tar").ok && !fs::exists(dir + "/long.tar"), "care: a path over the tar 100-byte name limit refuses the whole export (no silent loss)");
  fs::remove_all(root + "/jail/care.a/" + std::string(60, 'd'));
  {
    const std::string big = root + "/jail/care.a/big.bin";
    spit(big, "");
    fs::resize_file(big, kSavesMaxBytes + 1);
    check(!core.export_saves("care.a", dir + "/big.tar").ok && !fs::exists(dir + "/big.tar"), "care: export over 64 MiB is refused before reading it");
    fs::remove(big);
  }
  {
    fs::create_directories(root + "/jail/care.many");
    db.add_game({0, "care.many", "M", "1", "pnr-dev"});
    for (size_t i = 0; i < kSavesMaxFiles + 1; ++i) spit(root + "/jail/care.many/f" + std::to_string(i), "x");
    check(!core.export_saves("care.many", dir + "/many.tar").ok && !fs::exists(dir + "/many.tar"), "care: export of more than 10000 files is refused");
    fs::remove_all(root + "/jail/care.many"); db.remove_game("care.many");
  }
  fs::create_directories(outside + "/jaildir");
  db.add_game({0, "care.linkjail", "L", "1", "pnr-dev"});
  fs::create_symlink(outside + "/jaildir", root + "/jail/care.linkjail");
  check(!core.export_saves("care.linkjail", dir + "/lj.tar").ok && !fs::exists(dir + "/lj.tar"), "care: a saves folder that is a symlink is refused");
  check(!core.restore_saves("care.linkjail", out1, true).ok && tree(outside + "/jaildir") == "", "care: restore into a symlinked saves folder is refused, outside untouched");
  fs::remove(root + "/jail/care.linkjail"); db.remove_game("care.linkjail");

  // ---- restore: round trip, skip vs overwrite
  fs::remove_all(root + "/jail/care.a");
  auto r1 = core.restore_saves("care.a", out1, false);
  check(r1.ok && r1.files == 3 && r1.skipped == 0 && slurp(root + "/jail/care.a/slot1.sav") == "SLOT-ONE" && slurp(root + "/jail/care.a/profile/p.dat") == std::string(5000, 'p') && fs::exists(root + "/jail/care.a/profile/empty.dat"), "care: restore rebuilds the saves (folder recreated, content identical)");
  spit(root + "/jail/care.a/slot1.sav", "CHANGED");
  auto r2 = core.restore_saves("care.a", out1, false);
  check(r2.ok && r2.files == 0 && r2.skipped == 3 && slurp(root + "/jail/care.a/slot1.sav") == "CHANGED", "care: restore without --overwrite leaves existing files alone");
  auto r3 = core.restore_saves("care.a", out1, true);
  check(r3.ok && r3.files == 3 && slurp(root + "/jail/care.a/slot1.sav") == "SLOT-ONE", "care: restore with --overwrite replaces them");
  check(render_saves_text("restore", r2).find("left alone") != std::string::npos && render_saves_json("export", e1).find("\"files\":3") != std::string::npos, "care: saves commands render text and JSON");

  // ---- restore: hostile archives write nothing
  const std::string jail_before = tree(root + "/jail");
  const std::string out_before = tree(outside);
  int hostile_ok = 0, hostile_n = 0;
  for (const char* bad : {"../evil.txt", "/tmp/abs_evil.txt", "a/../../evil.txt", "a//b", "./x", "dir/", "back\\slash", "sub/../../../evil"}) {
    ++hostile_n;
    // a good file first, then the bad one: nothing may be written at all
    write_tar(dir + "/h.tar", {{"good.sav", "G"}, {bad, "EVIL"}});
    auto r = core.restore_saves("care.a", dir + "/h.tar", true);
    if (!r.ok && !r.error.empty()) ++hostile_ok;
  }
  check(hostile_ok == hostile_n, "care: every hostile archive name is refused");
  check(tree(root + "/jail") == jail_before && tree(outside) == out_before && !fs::exists(dir + "/evil.txt") && !fs::exists("/tmp/abs_evil.txt") && !fs::exists(root + "/evil.txt"), "care: refused archives wrote nothing anywhere, not even the good file before the bad one");
  write_tar(dir + "/dup.tar", {{"same.sav", "1"}, {"same.sav", "2"}});
  check(!core.restore_saves("care.a", dir + "/dup.tar", true).ok, "care: duplicate names in an archive are refused");
  write_tar(dir + "/fd.tar", {{"x", "file"}, {"x/y", "under"}});
  check(!core.restore_saves("care.a", dir + "/fd.tar", true).ok && tree(root + "/jail") == jail_before, "care: a name used as both file and folder is refused");
  spit(dir + "/garbage.tar", "this is not a tar archive at all");
  spit(dir + "/zero.tar", "");
  { std::string t = slurp(out1); spit(dir + "/trunc.tar", t.substr(0, t.size() / 2)); }
  for (const char* f : {"garbage.tar", "zero.tar", "trunc.tar", "missing.tar"})
    check(!core.restore_saves("care.a", dir + "/" + f, true).ok && tree(root + "/jail") == jail_before, std::string("care: restore refuses bad archive: ") + f);
  check(!core.restore_saves("care.a", dir, true).ok && !core.restore_saves("care.a", "", true).ok, "care: restore refuses a directory and an empty path");
  check(!core.restore_saves("nope", out1, true).ok && !core.restore_saves("..", out1, true).ok && !core.restore_saves("a/b", out1, true).ok, "care: restore refuses unknown and invalid ids");
  // tar bomb: a tiny file whose header claims a 3 GB entry must fail fast
  {
    write_tar(dir + "/bomb.tar", {{"x.sav", "tiny"}});
    std::string t = slurp(dir + "/bomb.tar");
    std::snprintf(&t[124], 12, "%011o", 3000000000u);       // size field, octal
    std::memset(&t[148], ' ', 8);
    unsigned sum = 0; for (int i = 0; i < 512; ++i) sum += static_cast<unsigned char>(t[i]);
    std::snprintf(&t[148], 8, "%06o", sum); t[154] = 0; t[155] = ' ';
    spit(dir + "/bomb.tar", t);
    std::vector<pnr::pkg::TarEntry> bomb;
    auto st = pnr::pkg::tar_read(dir + "/bomb.tar", bomb);
    check(!st.ok && st.error.message.find("exceeds the archive") != std::string::npos && bomb.empty(), "care: a header claiming 3 GB in a tiny file is rejected without allocating");
    check(!core.restore_saves("care.a", dir + "/bomb.tar", true).ok && tree(root + "/jail") == jail_before, "care: restore refuses the tar bomb");
  }
  {
    spit(dir + "/huge.tar", "");
    fs::resize_file(dir + "/huge.tar", 80ull * 1024 * 1024);
    check(!core.restore_saves("care.a", dir + "/huge.tar", true).ok, "care: an 80 MiB archive file is over the restore limit");
    fs::remove(dir + "/huge.tar");
    std::vector<pnr::pkg::TarEntry> one; one.push_back({"big.sav", std::vector<uint8_t>(kSavesMaxBytes + 1, 1)});
    check(pnr::pkg::tar_write(dir + "/over.tar", one).ok, "care: 64 MiB + 1 byte archive built");
    check(!core.restore_saves("care.a", dir + "/over.tar", true).ok && tree(root + "/jail") == jail_before, "care: contents over 64 MiB are refused before anything is written");
    fs::remove(dir + "/over.tar");
  }
  // symlinks in the way
  fs::create_symlink(outside, root + "/jail/care.a/linkdir");
  write_tar(dir + "/viaLink.tar", {{"linkdir/pwn.txt", "PWN"}});
  check(!core.restore_saves("care.a", dir + "/viaLink.tar", true).ok && !fs::exists(outside + "/pwn.txt"), "care: restore will not write through a symlinked folder");
  fs::create_symlink(outside + "/secret.txt", root + "/jail/care.a/linkfile");
  write_tar(dir + "/viaFile.tar", {{"linkfile", "PWN"}});
  check(!core.restore_saves("care.a", dir + "/viaFile.tar", true).ok && slurp(outside + "/secret.txt") == "SECRET", "care: restore will not overwrite through a symlinked file");
  fs::remove(root + "/jail/care.a/linkdir"); fs::remove(root + "/jail/care.a/linkfile");
  write_tar(dir + "/onfile.tar", {{"slot1.sav/inner", "X"}});
  check(!core.restore_saves("care.a", dir + "/onfile.tar", true).ok && slurp(root + "/jail/care.a/slot1.sav") == "SLOT-ONE", "care: a file in the way of a folder is refused");
  fs::create_directories(root + "/jail/care.a/adir");
  write_tar(dir + "/ondir.tar", {{"adir", "X"}});
  check(!core.restore_saves("care.a", dir + "/ondir.tar", true).ok && fs::is_directory(root + "/jail/care.a/adir"), "care: a folder in the way of a file is refused");
  // a game with no jail yet gets one
  check(core.import_package(dir + "/b.pnrgame").ok, "care: care.b reinstalled");
  fs::remove_all(root + "/jail/care.b");
  check(core.restore_saves("care.b", out1, false).ok && slurp(root + "/jail/care.b/slot1.sav") == "SLOT-ONE", "care: restoring into a game with no saves folder creates it");

  core.close(); db.close();
  fs::remove_all(dir);
}
