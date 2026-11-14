// Plugin slice P1 tests: versions, manifests, manager. SYNTHETIC plugins written to /tmp.
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "plugin/manager.hpp"
#include "plugin/manifest.hpp"
#include "plugin/semver.hpp"

namespace fs = std::filesystem;
using namespace pnr::plugin;

namespace {
std::string manifest_json(const std::string& id, const std::string& ver, const std::string& extra = "", const std::string& perms = "[]",
                          const std::string& deps = "{}", const std::string& entry = "{\"type\":\"data\"}", const std::string& kinds = "[\"catalog\"]") {
  return "{\"id\":\"" + id + "\",\"name\":\"N " + id + "\",\"version\":\"" + ver + "\",\"author\":\"t\",\"license\":\"GPL-2.0\",\"pnr_api\":\">=1.0.0 <2.0.0\","
         "\"kinds\":" + kinds + ",\"permissions\":" + perms + ",\"platforms\":[\"linux\",\"windows\"],\"dependencies\":" + deps + ",\"entry\":" + entry + extra + "}";
}
void write(const fs::path& p, const std::string& s) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << s; }
}  // namespace

void pnr_plugin_tests(int& pass, int& fail) {
  auto check = [&](bool ok, const std::string& l) { if (ok) ++pass; else ++fail; std::printf("%s %s\n", ok ? "PASS" : "FAIL", l.c_str()); };

  // ---- versions ----
  Version v;
  check(parse_version("1.2.3", &v) && v.major == 1 && v.minor == 2 && v.patch == 3 && v.str() == "1.2.3", "semver: parses MAJOR.MINOR.PATCH");
  check(!parse_version("1.2", &v) && !parse_version("1.2.3.4", &v) && !parse_version("1.2.x", &v) && !parse_version("1.2.3-beta", &v) && !parse_version("", &v) && !parse_version("1..3", &v) && !parse_version("1234567890.0.0", &v), "semver: short, long, non-digit, pre-release, empty and oversized versions are refused");
  Version a{1, 2, 3}, b{1, 10, 0};
  check(a < b && !(b < a) && Version{2, 0, 0} == Version{2, 0, 0}, "semver: compares numerically (1.2.3 < 1.10.0)");
  auto sat = [&](const char* c, const char* ver) { Constraint k; if (!k.parse(c).empty()) return false; Version x; parse_version(ver, &x); return k.satisfied_by(x); };
  check(sat(">=1.0.0 <2.0.0", "1.9.9") && !sat(">=1.0.0 <2.0.0", "2.0.0") && !sat(">=1.0.0 <2.0.0", "0.9.9"), "constraint: range of two comparators");
  check(sat("^1.2.3", "1.9.0") && !sat("^1.2.3", "2.0.0") && !sat("^1.2.3", "1.2.2"), "constraint: caret keeps the major");
  check(sat("~1.2.3", "1.2.9") && !sat("~1.2.3", "1.3.0"), "constraint: tilde keeps major and minor");
  check(sat("1.2.3", "1.2.3") && !sat("1.2.3", "1.2.4") && sat("*", "9.9.9") && sat(">1.0.0", "1.0.1") && sat("<=1.0.0", "1.0.0"), "constraint: exact, star, greater, less-or-equal");
  Constraint bad;
  check(!bad.parse("").empty() && !bad.parse(">=").empty() && !bad.parse("* >=1.0.0").empty() && !bad.parse("foo").empty(), "constraint: empty, dangling, mixed star and junk are errors");

  // ---- manifest ----
  auto ok = parse_manifest(manifest_json("demo.catalog", "1.0.0", "", "[\"net:example.org\",\"fs_write_plugin_data\"]", "{\"other.plugin\":\"^1.0.0\"}"));
  check(ok.ok && ok.manifest.id == "demo.catalog" && ok.manifest.permissions.size() == 2 && ok.manifest.kinds.size() == 1 && ok.manifest.dependencies.size() == 1, "manifest: a good manifest parses");
  auto kinds = parse_manifest(manifest_json("demo.all", "1.0.0", "", "[]", "{}", "{\"type\":\"data\"}", "[\"game_library\",\"store_source\",\"catalog\",\"download_provider\",\"package_extractor\",\"installer\",\"native_runtime\",\"compatibility\",\"performance\",\"metadata\"]"));
  check(kinds.ok && kinds.manifest.kinds.size() == 10, "manifest: all ten plugin kinds are accepted");
  check(!parse_manifest("not json").ok && !parse_manifest("[]").ok && !parse_manifest(std::string(300 * 1024, ' ')).ok, "manifest: bad JSON, wrong top-level type and oversize are refused");
  auto bad_all = parse_manifest("{\"id\":\"X\",\"version\":\"1\",\"kinds\":[\"warez\"],\"permissions\":[\"net:*\",\"root\"],\"platforms\":[\"amiga\"]}");
  check(!bad_all.ok && bad_all.errors.size() >= 7, "manifest: every problem is listed, not just the first (" + std::to_string(bad_all.errors.size()) + " found)");
  check(!parse_manifest(manifest_json("demo.x", "1.0.0", "", "[\"net:*\"]")).ok && !parse_manifest(manifest_json("demo.x", "1.0.0", "", "[\"net:\"]")).ok && !parse_manifest(manifest_json("demo.x", "1.0.0", "", "[\"net:Evil.COM\"]")).ok && !parse_manifest(manifest_json("demo.x", "1.0.0", "", "[\"all\"]")).ok, "manifest: wildcard, empty, upper-case host and unknown permission names are refused");
  check(!parse_manifest(manifest_json("../evil", "1.0.0")).ok && !parse_manifest(manifest_json("ab", "1.0.0")).ok && !parse_manifest(manifest_json("Demo.Cat", "1.0.0")).ok, "manifest: bad plugin ids are refused");
  auto selfdep = parse_manifest(manifest_json("demo.self", "1.0.0", "", "[]", "{\"demo.self\":\"*\"}"));
  check(!selfdep.ok, "manifest: a plugin cannot depend on itself");
  auto elf = parse_manifest(manifest_json("demo.elf", "1.0.0", "", "[]", "{}", "{\"type\":\"self\"}"));
  check(!elf.ok && elf.errors[0].find("console binary") != std::string::npos, "manifest: a console binary entry is refused with a plain reason");
  check(!parse_manifest(manifest_json("demo.p", "1.0.0", "", "[]", "{}", "{\"type\":\"process\",\"path\":\"../x\"}")).ok && !parse_manifest(manifest_json("demo.p", "1.0.0", "", "[]", "{}", "{\"type\":\"process\",\"path\":\"/bin/sh\"}")).ok && parse_manifest(manifest_json("demo.p", "1.0.0", "", "[]", "{}", "{\"type\":\"process\",\"path\":\"bin/run\"}")).ok, "manifest: process entry path must stay inside the plugin");
  check(!parse_manifest(manifest_json("demo.rt", "1.0.0", "", "[\"runtime_hook\"]", "{}", "{\"type\":\"process\",\"path\":\"bin/run\"}")).ok && parse_manifest(manifest_json("demo.rt", "1.0.0", "", "[\"runtime_hook\"]", "{}", "{\"type\":\"process\",\"path\":\"bin/run\"}", "[\"native_runtime\"]")).ok, "manifest: runtime_hook is only for native_runtime plugins");
  auto old_api = parse_manifest("{\"id\":\"demo.old\",\"name\":\"n\",\"version\":\"1.0.0\",\"author\":\"t\",\"license\":\"MIT\",\"pnr_api\":\">=2.0.0\",\"kinds\":[\"catalog\"],\"platforms\":[\"linux\"]}");
  check(old_api.ok && !check_compatible(old_api.manifest).empty(), "compat: a plugin needing a newer plugin API is reported incompatible");
  auto win_only = parse_manifest("{\"id\":\"demo.win\",\"name\":\"n\",\"version\":\"1.0.0\",\"author\":\"t\",\"license\":\"MIT\",\"pnr_api\":\"^1.0.0\",\"kinds\":[\"catalog\"],\"platforms\":[\"windows\"]}");
  check(win_only.ok && !check_compatible(win_only.manifest).empty(), "compat: a plugin that does not list this platform is reported incompatible");
  check(valid_relative_path("a/b.txt") && !valid_relative_path("a//b") && !valid_relative_path("./a") && !valid_relative_path("a/../b") && !valid_relative_path("a\\b") && !valid_relative_path(""), "path: relative path rules");

  // ---- manager ----
  const std::string root = "/tmp/pnr_plugin_" + std::to_string(::getpid());
  fs::remove_all(root);
  const fs::path src = fs::path(root) / "src";
  PluginManager pm(root + "/home");
  write(src / "base" / kManifestFile, manifest_json("demo.base", "1.0.0", "", "[\"fs_write_plugin_data\"]"));
  write(src / "base" / "data" / "a.txt", "hello");
  write(src / "child" / kManifestFile, manifest_json("demo.child", "1.0.0", "", "[]", "{\"demo.base\":\"^1.0.0\"}"));
  auto ib = pm.install_from_dir((src / "base").string());
  check(ib.ok && !ib.updated && pm.list().size() == 1 && fs::exists(root + "/home/plugins/demo.base/data/a.txt"), "manager: install copies the plugin tree");
  InstalledPlugin ip;
  check(pm.get("demo.base", &ip) && !ip.enabled && ip.granted.empty(), "manager: a fresh install is disabled and holds no permissions");
  auto e1 = pm.enable("demo.base", {});
  check(!e1.ok && e1.error.message.find("fs_write_plugin_data") != std::string::npos, "manager: enabling without granting every declared permission is refused, naming it");
  check(pm.enable("demo.base", {"fs_write_plugin_data"}).ok && pm.get("demo.base", &ip) && ip.enabled && ip.granted.size() == 1, "manager: enable with the grant works and records it");
  check(pm.install_from_dir((src / "child").string()).ok, "manager: second plugin installs");
  check(pm.enable("demo.child", {}).ok, "manager: child enables once its dependency is installed, in range and enabled");
  check(!pm.disable("demo.base").ok && !pm.remove("demo.base").ok, "manager: base cannot be disabled or removed while an enabled/installed dependent exists");
  check(pm.disable("demo.child").ok && pm.disable("demo.base").ok && !pm.remove("demo.base").ok, "manager: disabled dependents still block remove");
  check(!pm.enable("demo.child", {}).ok, "manager: child cannot enable while its dependency is disabled");
  check(pm.remove("demo.child").ok && pm.remove("demo.base").ok && pm.list().empty() && !fs::exists(root + "/home/plugins/demo.base"), "manager: removal in dependency order deletes the files");
  check(!pm.remove("demo.base").ok && !pm.enable("nope.nothing", {}).ok, "manager: unknown plugin ids are errors");

  // dependency range and missing dependency
  write(src / "needs" / kManifestFile, manifest_json("demo.needs", "1.0.0", "", "[]", "{\"demo.base\":\">=2.0.0\"}"));
  pm.install_from_dir((src / "base").string());
  pm.install_from_dir((src / "needs").string());
  auto en = pm.enable("demo.needs", {});
  check(!en.ok && en.error.message.find("1.0.0") != std::string::npos, "manager: a dependency outside its version range blocks enable, naming the installed version");
  pm.remove("demo.needs");
  write(src / "orphan" / kManifestFile, manifest_json("demo.orphan", "1.0.0", "", "[]", "{\"missing.one\":\"*\"}"));
  pm.install_from_dir((src / "orphan").string());
  check(!pm.enable("demo.orphan", {}).ok, "manager: a missing dependency blocks enable");
  pm.remove("demo.orphan");

  // cycle
  write(src / "cyca" / kManifestFile, manifest_json("cyc.a", "1.0.0", "", "[]", "{\"cyc.b\":\"*\"}"));
  write(src / "cycb" / kManifestFile, manifest_json("cyc.b", "1.0.0", "", "[]", "{\"cyc.a\":\"*\"}"));
  check(pm.install_from_dir((src / "cyca").string()).ok && !pm.install_from_dir((src / "cycb").string()).ok, "manager: an install that would close a dependency cycle is refused");
  pm.remove("cyc.a");

  // updates
  write(src / "up2" / kManifestFile, manifest_json("demo.base", "1.1.0", "", "[\"fs_write_plugin_data\"]"));
  write(src / "up3" / kManifestFile, manifest_json("demo.base", "1.2.0", "", "[\"fs_write_plugin_data\",\"library_read\"]"));
  pm.enable("demo.base", {"fs_write_plugin_data"});
  auto u1 = pm.install_from_dir((src / "base").string());
  check(!u1.ok, "update: the same version is not installed again");
  auto u2 = pm.install_from_dir((src / "up2").string());
  check(u2.ok && u2.updated && !u2.disabled_by_update && pm.get("demo.base", &ip) && ip.enabled && ip.manifest.version.str() == "1.1.0", "update: a newer version with the same permissions stays enabled");
  auto u3 = pm.install_from_dir((src / "up3").string());
  check(u3.ok && u3.updated && u3.disabled_by_update && pm.get("demo.base", &ip) && !ip.enabled && ip.granted.empty(), "update: a new permission request installs the plugin DISABLED with the grants cleared");
  write(src / "down" / kManifestFile, manifest_json("demo.base", "1.0.5"));
  check(!pm.install_from_dir((src / "down").string()).ok && pm.get("demo.base", &ip) && ip.manifest.version.str() == "1.2.0", "update: a downgrade is refused and leaves the installed plugin alone");

  // hostile source trees
  write(src / "sym" / kManifestFile, manifest_json("demo.sym", "1.0.0"));
  std::error_code ec; fs::create_symlink("/etc/passwd", src / "sym" / "link", ec);
  check(!pm.install_from_dir((src / "sym").string()).ok && !fs::exists(root + "/home/plugins/demo.sym"), "install: a plugin containing a symbolic link is refused and nothing is placed");
  write(src / "hid" / kManifestFile, manifest_json("demo.hid", "1.0.0")); write(src / "hid" / ".secret", "x");
  check(!pm.install_from_dir((src / "hid").string()).ok, "install: a hidden file is refused");
  write(src / "noent" / kManifestFile, manifest_json("demo.noent", "1.0.0", "", "[]", "{}", "{\"type\":\"process\",\"path\":\"bin/run\"}"));
  check(!pm.install_from_dir((src / "noent").string()).ok, "install: a process entry that is not in the plugin is refused");
  write(src / "badm" / kManifestFile, "{}");
  auto bm = pm.install_from_dir((src / "badm").string());
  check(!bm.ok && bm.errors.size() >= 4, "install: a bad manifest fails with all its problems");
  check(!pm.install_from_dir(root + "/does-not-exist").ok && !pm.install_from_dir((src / "base" / kManifestFile).string()).ok, "install: a missing path or a file instead of a directory is an error");

  // state survives a new manager instance
  PluginManager again(root + "/home");
  pm.enable("demo.base", {"fs_write_plugin_data", "library_read"});
  check(again.get("demo.base", &ip) && ip.enabled && again.enabled_of_kind(Kind::Catalog).size() == 1 && again.enabled_of_kind(Kind::Installer).empty(), "manager: state persists and enabled_of_kind filters");
  fs::remove_all(root);
  // end of pnr_plugin_tests
           }
