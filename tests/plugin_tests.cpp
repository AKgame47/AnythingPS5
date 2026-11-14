// Plugin slice P1 tests: versions, manifests, manager. SYNTHETIC plugins written to /tmp.
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "plugin/manager.hpp"
#include "plugin/analyze.hpp"
#include "plugin/archive.hpp"
#include "library/game_db.hpp"
#include "plugin/fetch.hpp"
#include "plugin/host.hpp"
#include "store/store.hpp"
#include "plugin/manifest.hpp"
#include "plugin/runner.hpp"
#include "sandbox/sandbox.hpp"
#include "plugin/semver.hpp"
#include "plugin/signing.hpp"
#include "psn/http.hpp"

namespace fs = std::filesystem;
using namespace pnr::plugin;

namespace {
// P3b fixtures: tar.gz made with Python's tarfile+gzip (GitHub style: pax global header, top folder, a .gitignore).
static const unsigned char kGoodTgz[] = {31,139,8,0,0,0,0,0,2,255,237,152,207,107,219,48,20,199,125,238,95,33,116,217,165,81,36,59,73,71,105,199,160,135,245,176,67,239,99,12,197,122,115,212,217,146,145,228,53,161,228,127,223,83,204,24,4,50,54,104,82,218,190,15,6,73,79,95,255,128,167,175,252,144,152,138,233,199,59,189,190,5,109,32,20,71,65,142,28,106,165,44,203,63,253,28,87,82,45,22,5,107,138,19,48,196,164,3,190,190,120,155,168,247,172,246,93,7,46,93,235,101,173,202,234,172,32,222,16,186,238,96,82,251,159,16,226,100,92,0,211,163,248,255,98,62,63,236,255,253,190,82,10,183,4,54,39,255,63,71,254,123,23,38,125,59,52,214,137,251,232,221,211,228,127,49,155,29,204,127,53,223,207,255,76,73,220,255,37,229,255,232,60,114,107,248,37,111,86,98,92,4,252,156,59,221,1,134,110,126,143,115,99,189,195,144,18,165,144,24,49,16,235,96,251,52,70,13,70,244,144,86,62,224,64,227,160,181,53,184,152,159,241,233,238,243,100,188,5,23,213,55,221,91,140,125,184,86,66,10,201,174,202,220,224,212,15,235,76,228,151,95,120,7,73,27,157,52,255,138,122,8,157,141,249,189,121,42,7,90,157,190,251,208,237,148,173,117,195,58,203,12,244,224,12,184,218,2,78,60,110,207,57,254,201,194,6,187,60,109,250,252,9,187,7,110,183,180,211,255,179,255,157,79,16,69,90,167,167,220,255,255,226,255,170,154,237,251,191,194,139,252,127,10,118,46,103,58,36,182,75,59,67,139,177,180,2,54,68,8,239,34,243,15,142,181,118,25,116,216,8,70,90,210,146,150,180,164,37,237,75,209,82,133,247,223,245,159,104,108,178,141,243,1,78,83,255,229,211,129,253,250,79,97,136,234,191,19,176,28,108,107,166,116,232,71,16,4,65,16,4,65,16,4,65,16,4,241,90,249,5,224,51,168,148,0,40,0,0};
static const unsigned char kSymTgz[] = {31,139,8,0,0,0,0,0,2,255,237,213,65,75,195,48,20,7,240,158,253,20,37,103,219,38,221,58,69,156,8,30,244,224,193,187,136,196,38,206,104,155,150,36,211,137,236,187,251,98,17,97,178,225,97,211,131,255,223,37,201,75,214,14,94,222,107,94,228,197,233,149,92,92,104,169,180,75,118,130,15,214,141,156,151,229,215,60,198,5,23,147,73,146,206,146,95,48,247,65,58,122,125,242,63,137,195,180,238,218,86,219,48,149,119,181,40,71,123,9,252,35,178,110,117,86,119,207,218,249,108,184,0,197,78,234,255,160,170,214,215,255,234,92,8,65,45,33,173,80,255,127,145,255,222,186,172,111,230,51,99,243,71,223,217,237,228,127,50,30,175,205,255,168,90,205,255,88,112,234,255,28,249,223,185,55,102,20,59,98,179,135,124,184,4,108,159,89,217,106,10,157,125,174,227,96,58,75,33,145,151,57,167,136,210,190,118,166,15,67,84,81,68,206,195,67,231,104,33,105,209,152,90,91,31,159,113,126,117,153,13,63,161,75,117,43,123,67,177,147,169,200,121,206,211,227,50,14,180,245,100,172,242,236,232,154,181,58,72,37,131,100,55,116,94,187,214,248,248,222,184,21,3,141,12,247,157,107,63,78,54,198,206,23,241,152,210,189,182,74,219,218,104,218,120,91,238,51,250,146,185,87,154,178,240,218,199,191,240,241,192,229,18,157,254,199,245,175,159,77,179,245,254,191,169,254,191,247,255,49,167,150,144,150,133,14,117,209,75,239,95,20,234,31,0,0,0,0,0,0,0,0,0,0,0,0,0,96,179,119,239,74,146,216,0,40,0,0};
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

  // ---- P2: isolated execution + broker (needs a usable sandbox; says so when it is not) ----
  {
    PluginManager rm(root + "/rhome");
    auto mk = [&](const std::string& id, const std::string& perms, const std::string& script) {
      fs::path d = fs::path(root) / "rsrc" / id;
      write(d / kManifestFile, manifest_json(id, "1.0.0", "", perms, "{}", "{\"type\":\"process\",\"path\":\"bin/run.sh\"}"));
      write(d / "bin" / "run.sh", script);
      fs::permissions(d / "bin" / "run.sh", fs::perms::owner_all);
      return rm.install_from_dir(d.string());
    };
    InstalledPlugin dp;
    check(mk("run.data", "[]", "#!/bin/sh\n").ok && !run_plugin([&] { InstalledPlugin q; rm.get("run.data", &q); return q; }(), root + "/rhome").started, "run: a plugin that is not enabled is not started");
    pnr::sandbox::Probe sp = pnr::sandbox::probe();
    if (!sp.usable()) {
      std::printf("SKIP plugin sandbox run tests: %s\n", sp.notes.c_str());
      rm.enable("run.data", {});
      rm.get("run.data", &dp);
      auto rr = run_plugin(dp, root + "/rhome");
      check(!rr.started && !rr.error.empty(), "run: with no usable sandbox the plugin is NOT started and the reason is given");
    } else {
      const char* script =
        "#!/bin/sh\n"
        "echo '{\"call\":\"log\",\"msg\":\"hello from the sandbox\"}'; read a\n"
        "echo '{\"call\":\"data_put\",\"name\":\"k\",\"value\":\"v1\"}'; read b\n"
        "echo '{\"call\":\"library_list\"}'; read c\n"
        "b=$(echo \"$b\" | tr -d '\"{}'); c=$(echo \"$c\" | tr -d '\"{}')\n"
        "echo '{\"call\":\"result\",\"value\":\"b='\"$b\"' c='\"$c\"'\"}'\n";
      (void)mk("run.nop", "[]", script);
      rm.enable("run.nop", {});
      InstalledPlugin np; rm.get("run.nop", &np);
      auto r1 = run_plugin(np, root + "/rhome");
      check(r1.started && r1.completed && r1.logs.size() == 1 && r1.logs[0] == "hello from the sandbox", "run: an enabled process plugin runs in the sandbox and its log reaches the host");
      check(r1.denied == 2 && r1.result.find("permission fs_write_plugin_data was not granted") != std::string::npos, "broker: data_put without the grant is refused");
      check(r1.result.find("library_read was not granted") != std::string::npos, "broker: a library call without the grant is refused");
      check(!fs::exists(root + "/rhome/plugins/data/run.nop/k"), "broker: a refused write left no file");
      (void)mk("run.gr", "[\"fs_write_plugin_data\"]", script);
      rm.enable("run.gr", {"fs_write_plugin_data"});
      InstalledPlugin gp; rm.get("run.gr", &gp);
      auto r2 = run_plugin(gp, root + "/rhome");
      std::string got; { std::ifstream in(root + "/rhome/plugins/data/run.gr/k"); std::getline(in, got); }
      check(r2.completed && r2.denied == 1 && got == "v1", "broker: with the grant data_put is served and the file lands in the plugin data dir");
      // escape attempts: reading the host, writing outside, the network
      const char* esc =
        "#!/bin/sh\n"
        "r=''\n"
        "[ -r /etc/passwd ] && r=\"$r etc-visible\" || r=\"$r etc-hidden\"\n"
        "[ -d /home ] && r=\"$r home-visible\" || r=\"$r home-hidden\"\n"
        "( echo x > \"$0.pwn\" ) 2>/dev/null && r=\"$r wrote-own-dir\" || r=\"$r own-dir-readonly\"\n"
        "echo '{\"call\":\"result\",\"value\":\"'\"$r\"'\"}'\n";
      (void)mk("run.esc", "[]", esc);
      rm.enable("run.esc", {});
      InstalledPlugin ep; rm.get("run.esc", &ep);
      auto r3 = run_plugin(ep, root + "/rhome");
      check(r3.completed && r3.result.find("etc-hidden") != std::string::npos && r3.result.find("home-hidden") != std::string::npos && r3.result.find("own-dir-readonly") != std::string::npos, "sandbox: the plugin cannot see the host /etc or /home and cannot write its own code dir");
      (void)mk("run.hang", "[]", "#!/bin/sh\nsleep 30\n");
      rm.enable("run.hang", {});
      InstalledPlugin hp; rm.get("run.hang", &hp);
      RunOptions fast; fast.timeout_ms = 600;
      auto r4 = run_plugin(hp, root + "/rhome", fast);
      check(r4.started && !r4.completed && r4.timed_out, "limits: a plugin that hangs is killed at the timeout");
      (void)mk("run.spam", "[]", "#!/bin/sh\nwhile true; do echo '{\"call\":\"log\",\"msg\":\"x\"}'; read a; done\n");
      rm.enable("run.spam", {});
      InstalledPlugin xp; rm.get("run.spam", &xp);
      RunOptions few; few.max_requests = 20; few.timeout_ms = 5000;
      auto r5 = run_plugin(xp, root + "/rhome", few);
      check(!r5.completed && r5.error.find("request limit") != std::string::npos, "limits: a plugin that floods requests is stopped");
    }
    InstalledPlugin dd;  // data-only plugin
    check(rm.get("run.data", &dd) && !run_plugin(dd, root + "/rhome").started, "run: a data-only or disabled plugin never starts a process");
  }

  // ---- P3: static analysis + signatures (offline, nothing is executed) ----
  {
    std::string w;
    check(is_console_binary_magic(std::string("\x4f\x15\x3d\x1d\0\0\0\0", 8), &w) && w.find("SELF") != std::string::npos, "analyze: a Sony SELF container is recognised and refused");
    check(is_console_binary_magic(std::string("\x7f" "ELF\x02\x01\x01\x09", 8), &w), "analyze: a FreeBSD-ABI ELF (console payload style) is refused");
    check(!is_console_binary_magic(std::string("\x7f" "ELF\x02\x01\x01\x00", 8), &w) && !is_console_binary_magic("hello", &w), "analyze: an ordinary Linux ELF or text is not called a console binary");
    check(!piracy_findings("a.json", "{\"url\":\"https://mega.nz/file/abc\"}").empty() && !piracy_findings("a.txt", "magnet:?xt=urn:btih:abc").empty(), "analyze: file-locker hosts and magnet links are findings");
    check(!piracy_findings("list.json", "fpkg dump list http://x.example/a.pkg").empty() && !piracy_findings("n.md", "installs kstuff after the jailbreak").empty(), "analyze: dump catalogs with links and exploit payload loaders are findings");
    check(piracy_findings("notes.md", "Metadata for the user's own games. Reads a local JSON file and shows covers.").empty(), "analyze: ordinary plugin text has no findings");
    check(is_refused_host("mega.nz") && is_refused_host("www.mediafire.com") && !is_refused_host("notmega.nz") && !is_refused_host("example.org"), "analyze: refused hosts match the domain and subdomains, not look-alikes");

    PluginManager am(root + "/ahome");
    auto mkd = [&](const std::string& id, const std::string& perms) {
      fs::path d = fs::path(root) / "asrc" / id;
      write(d / kManifestFile, manifest_json(id, "1.0.0", "", perms));
      return d;
    };
    fs::path d1 = mkd("an.piracy", "[]"); write(d1 / "catalog.json", "{\"games\":[{\"download\":\"https://www.mediafire.com/file/x\"}]}");
    auto r1 = am.install_from_dir(d1.string());
    check(!r1.ok && r1.errors.size() >= 2 && !fs::exists(root + "/ahome/plugins/an.piracy"), "install: a download-provider payload for copyrighted games is refused and nothing is placed");
    fs::path d2 = mkd("an.self", "[]"); write(d2 / "bin" / "game.bin", std::string("\x4f\x15\x3d\x1d", 4) + std::string(60, 'x'));
    check(!am.install_from_dir(d2.string()).ok, "install: a plugin carrying a SELF container is refused");
    check(!am.install_from_dir(mkd("an.net", "[\"net:mega.nz\"]").string()).ok, "install: a permission for a file-locker host is refused");
    fs::path d3 = mkd("an.clean", "[]"); write(d3 / "notes.txt", "Shows covers for the user's own library.");
    auto r3 = am.install_from_dir(d3.string());
    check(r3.ok && r3.signature == "unsigned", "install: a clean plugin installs and is shown as unsigned");

    // signatures
    const std::string seed(64, '1');  // 32 bytes of 0x11 as hex
    fs::path s1 = mkd("sg.one", "[]"); write(s1 / "data.txt", "hello");
    std::string pub, err;
    check(sign_dir(s1.string(), seed, &pub, &err) && pub.size() == 64 && !sign_dir(s1.string(), "zz", &pub, &err), "sign: a seed is needed and must be 64 hex characters");
    auto u = am.install_from_dir(s1.string());
    check(u.ok && u.signature.find("untrusted") != std::string::npos, "signature: a valid signature from an unpinned key proves nothing and says so");
    fs::path s2 = mkd("sg.two", "[]"); write(s2 / "data.txt", "hello"); sign_dir(s2.string(), seed, &pub, &err);
    check(!am.pin_key("bad name!", pub).ok && !am.pin_key("alice", "abcd").ok && am.pin_key("alice", pub).ok && !am.pin_key("alice", pub).ok, "pin: bad names, short keys and duplicate names are refused");
    check(am.pinned_keys().size() == 1 && am.pinned_keys()[0].name == "alice", "pin: the pinned key persists");
    auto v = am.install_from_dir(s2.string());
    check(v.ok && v.signature.find("pinned key 'alice'") != std::string::npos, "signature: a valid signature from a pinned key is reported with the key name");
    fs::path s3 = mkd("sg.three", "[]"); write(s3 / "data.txt", "hello"); sign_dir(s3.string(), seed, &pub, &err);
    write(s3 / "data.txt", "hello, changed");
    check(!am.install_from_dir(s3.string()).ok && !fs::exists(root + "/ahome/plugins/sg.three"), "signature: a changed file after signing is refused");
    fs::path s4 = mkd("sg.four", "[]"); write(s4 / "data.txt", "hello"); sign_dir(s4.string(), seed, &pub, &err);
    write(s4 / "extra.txt", "added later");
    check(!am.install_from_dir(s4.string()).ok, "signature: a file added after signing is refused");
    fs::path s5 = mkd("sg.five", "[]"); write(s5 / "data.txt", "hello"); write(s5 / kSigFile, "{\"key\":\"00\",\"signature\":\"00\"}");
    check(!am.install_from_dir(s5.string()).ok, "signature: a malformed signature file is refused");
    am.require_signed = true;
    check(!am.install_from_dir(mkd("sg.six", "[]").string()).ok, "require-signed: an unsigned plugin is refused");
    fs::path s7 = mkd("sg.seven", "[]"); write(s7 / "data.txt", "hello"); sign_dir(s7.string(), std::string(64, '2'), &pub, &err);
    check(!am.install_from_dir(s7.string()).ok, "require-signed: a valid signature from an unpinned key is refused");
    fs::path s8 = mkd("sg.eight", "[]"); write(s8 / "data.txt", "hello"); sign_dir(s8.string(), seed, &pub, &err);
    check(am.install_from_dir(s8.string()).ok, "require-signed: a valid signature from a pinned key installs");
    check(am.unpin_key("alice").ok && !am.unpin_key("alice").ok && am.pinned_keys().empty(), "pin: unpin removes the key, and an unknown name is an error");
  }

  // ---- P3b: package files and GitHub URLs (fixtures and a fake HTTP client only; nothing contacts GitHub) ----
  {
    GithubRef g; std::string why;
    check(parse_github_url("https://github.com/acme/covers", &g, &why) && g.owner == "acme" && g.repo == "covers" && g.ref == "HEAD", "github: a plain repo URL parses with ref HEAD");
    check(parse_github_url("https://github.com/acme/covers.git/", &g, &why) && g.repo == "covers" && github_archive_url(g) == "https://codeload.github.com/acme/covers/tar.gz/HEAD", "github: .git and a trailing slash are tolerated; the archive URL is on codeload.github.com");
    check(parse_github_url("https://github.com/acme/covers/tree/v1.2.0", &g, &why) && g.ref == "v1.2.0", "github: /tree/<ref> selects the ref");
    check(!parse_github_url("http://github.com/acme/covers", &g, &why) && !parse_github_url("https://evil.example/acme/covers", &g, &why) && !parse_github_url("https://github.com.evil.example/a/b", &g, &why) && !parse_github_url("https://user@github.com/a/b", &g, &why) && !parse_github_url("https://github.com/a/b?x=1", &g, &why) && !parse_github_url("https://github.com/a", &g, &why) && !parse_github_url("https://github.com/a/b/blob/main/x", &g, &why) && !parse_github_url("https://github.com/a/../b", &g, &why), "github: other schemes, hosts, look-alikes, user-info, queries and odd paths are refused");

    std::string gz((const char*)kGoodTgz, sizeof kGoodTgz), plain, e;
    check(gunzip(gz, &plain, 1 << 20, &e) && plain.size() % 512 == 0 && plain.size() > 1000, "gunzip: a real gzip stream inflates to a whole number of tar blocks");
    std::string bad = gz; bad[bad.size() - 6] ^= 0x55;
    check(!gunzip(bad, &plain, 1 << 20, &e) && !gunzip(gz.substr(0, gz.size() / 2), &plain, 1 << 20, &e) && !gunzip("not gzip at all, really not gzip", &plain, 1 << 20, &e), "gunzip: a bad checksum, a truncated stream and non-gzip input are errors");
    check(!gunzip(gz, &plain, 100, &e) && e.find("limit") != std::string::npos, "gunzip: output past the limit is refused (zip-bomb guard)");

    PluginManager gm(root + "/ghome");
    std::string base = root + "/gpk"; fs::create_directories(base);
    { std::ofstream(base + "/covers.tar.gz", std::ios::binary).write((const char*)kGoodTgz, sizeof kGoodTgz); }
    auto ri = install_from_archive_file(gm, base + "/covers.tar.gz");
    InstalledPlugin gp;
    check(ri.ok && ri.manifest.id == "gh.covers" && gm.get("gh.covers", &gp) && !gp.enabled, "package: a GitHub-style tar.gz installs (top folder stripped), disabled");
    check(fs::exists(root + "/ghome/plugins/gh.covers/notes.txt") && !fs::exists(root + "/ghome/plugins/gh.covers/.gitignore"), "package: .gitignore and other hidden files are dropped, the rest is installed");
    check(!fs::exists(root + "/ghome/plugins/.stage-0") && [&] { for (auto& d : fs::directory_iterator(root + "/ghome/plugins")) if (d.path().filename().string().rfind(".stage-", 0) == 0) return false; return true; }(), "package: the staging directory is cleaned up");
    check(gunzip(gz, &plain, 1 << 20, &e), "gunzip: the same stream inflates again");
    { std::ofstream(base + "/covers.tar", std::ios::binary) << plain; }
    gm.remove("gh.covers");
    check(install_from_archive_file(gm, base + "/covers.tar").ok, "package: a plain .tar works too");
    { std::ofstream(base + "/sym.tgz", std::ios::binary).write((const char*)kSymTgz, sizeof kSymTgz); }
    auto rs = install_from_archive_file(gm, base + "/sym.tgz");
    check(!rs.ok && !rs.errors.empty() && rs.errors[0].find("symlinks") != std::string::npos, "package: an archive containing a symlink is refused");
    auto tarhdr = [](const std::string& name, std::size_t size, char type) {
      std::string h(512, '\0'); std::memcpy(&h[0], name.data(), std::min<std::size_t>(name.size(), 100));
      std::snprintf(&h[100], 8, "%07o", 0644); std::snprintf(&h[124], 12, "%011o", (unsigned)size); h[156] = type; std::memcpy(&h[257], "ustar", 5);
      std::memset(&h[148], ' ', 8); unsigned sum = 0; for (unsigned char c : h) sum += c; std::snprintf(&h[148], 7, "%06o", sum); h[154] = 0; h[155] = ' ';
      return h;
    };
    auto mktar = [&](const std::string& name, const std::string& data) { std::string t = tarhdr(name, data.size(), '0') + data + std::string((512 - data.size() % 512) % 512, '\0') + std::string(1024, '\0'); return t; };
    std::string ex;
    check(!extract_archive(mktar("../escape.txt", "x"), root + "/gx1", ArchiveLimits{}, &ex) && !fs::exists(root + "/gx1") && !fs::exists(root + "/escape.txt"), "archive: a '..' path is refused and nothing is written");
    check(!extract_archive(mktar("/abs/file", "x"), root + "/gx2", ArchiveLimits{}, &ex) && !fs::exists(root + "/gx2"), "archive: an absolute path is refused");
    std::string two = tarhdr("a.txt", 1, '0') + "x" + std::string(511, '\0') + tarhdr("a.txt", 1, '0') + "y" + std::string(511, '\0') + std::string(1024, '\0');
    check(!extract_archive(two, root + "/gx3", ArchiveLimits{}, &ex), "archive: a path listed twice is refused");
    ArchiveLimits tiny; tiny.max_files = 1;
    check(!extract_archive(two, root + "/gx4", tiny, &ex) && ex.find("too many") != std::string::npos, "archive: the file-count limit is enforced");
    check(!extract_archive(mktar("a.txt", "x").substr(0, 700), root + "/gx5", ArchiveLimits{}, &ex), "archive: a truncated tar is an error");
    std::string hdrbad = mktar("a.txt", "x"); hdrbad[10] ^= 1;
    check(!extract_archive(hdrbad, root + "/gx6", ArchiveLimits{}, &ex) && ex.find("checksum") != std::string::npos, "archive: a corrupted tar header is refused");
    check(!extract_archive(mktar("pnr-plugin.json", "{}"), root + "/ghome", ArchiveLimits{}, &ex), "archive: an existing destination is never overwritten");

    // analysis still applies to package installs
    std::string piracy = mktar("pnr-plugin.json", manifest_json("gh.dl", "1.0.0"));
    piracy.resize(piracy.size() - 1024);
    std::string cat = "{\"u\":\"https://mega.nz/file/abc\"}";
    piracy += tarhdr("catalog.json", cat.size(), '0') + cat + std::string((512 - cat.size() % 512) % 512, '\0') + std::string(1024, '\0');
    { std::ofstream(base + "/pir.tar", std::ios::binary) << piracy; }
    auto rp = install_from_archive_file(gm, base + "/pir.tar");
    check(!rp.ok && !fs::exists(root + "/ghome/plugins/gh.dl"), "package: the same analysis refuses a download-provider payload inside a package");

    // GitHub install through a fake client: checks the exact URL, status handling and size cap
    struct Fake : pnr::psn::HttpClient {
      std::string url, body; int status = 200; int calls = 0;
      pnr::Status perform(const pnr::psn::HttpRequest&, pnr::psn::HttpResponse*) override { return pnr::Status::Fail(1, "t", "unused"); }
      pnr::Status download(const pnr::psn::HttpRequest& r, const pnr::psn::BodySink& sink, pnr::psn::HttpResponse* res) override {
        ++calls; url = r.url; res->status = status;
        if (status == 200) { std::size_t off = 0; while (off < body.size()) { std::size_t n = std::min<std::size_t>(100, body.size() - off); if (!sink(*res, body.data() + off, n)) return pnr::Status::Fail(2, "t", "aborted"); off += n; } }
        return pnr::Status::Ok();
      }
    } fake;
    fake.body = gz;
    PluginManager hm(root + "/hhome");
    auto rg = install_from_github(hm, fake, "https://github.com/acme/covers/tree/v1.2.0");
    check(rg.ok && fake.url == "https://codeload.github.com/acme/covers/tar.gz/v1.2.0" && hm.get("gh.covers", &gp) && !gp.enabled, "github: installs from the codeload archive URL, disabled, after analysis");
    PluginManager hm2(root + "/hhome2"); fake.status = 404;
    auto r404 = install_from_github(hm2, fake, "https://github.com/acme/covers");
    check(!r404.ok && r404.errors[0].find("404") != std::string::npos && hm2.list().empty(), "github: an HTTP error installs nothing and says why");
    fake.status = 200;
    auto rbig = install_from_github(hm2, fake, "https://github.com/acme/covers", 200);
    check(!rbig.ok && rbig.errors[0].find("larger") != std::string::npos && hm2.list().empty(), "github: a download past the size cap is aborted and nothing is installed");
    int before = fake.calls;
    auto rbad = install_from_github(hm2, fake, "https://evil.example/acme/covers");
    check(!rbad.ok && fake.calls == before, "github: a non-GitHub URL never reaches the network");
  }

  // ---- P4: broker calls for library, store and download (fake host for gating; live host on temp files) ----
  {
    struct FakeHost : BrokerHost {
      int lib = 0, src = 0, dl = 0; bool fail = false;
      bool library_list(std::string* j, std::string*) override { ++lib; *j = "[{\"id\":\"g1\",\"title\":\"Game One\"}]"; return true; }
      bool store_add_source(const std::string&, const std::string&, const std::string&, std::string* e) override { ++src; if (fail) { *e = "nope"; return false; } return true; }
      bool download_request(const std::string&, const std::string&, std::string* e) override { ++dl; if (fail) { *e = "nope"; return false; } return true; }
    } fh;
    pnr::sandbox::Probe sp4 = pnr::sandbox::probe();
    if (!sp4.usable()) std::printf("SKIP P4 broker run tests: %s\n", sp4.notes.c_str());
    else {
      PluginManager bm(root + "/bhome");
      auto mkb = [&](const std::string& id, const std::string& perms, const std::string& script) {
        fs::path d = fs::path(root) / "bsrc" / id;
        write(d / kManifestFile, manifest_json(id, "1.0.0", "", perms, "{}", "{\"type\":\"process\",\"path\":\"bin/run.sh\"}"));
        write(d / "bin" / "run.sh", script); fs::permissions(d / "bin" / "run.sh", fs::perms::owner_all);
        bm.install_from_dir(d.string());
        InstalledPlugin q; return q;
      };
      const char* sc =
        "#!/bin/sh\n"
        "echo '{\"call\":\"library_list\"}'; read a\n"
        "echo '{\"call\":\"store_add_source\",\"id\":\"s1\",\"url\":\"https://example.org/i.json\",\"rights\":\"mine\"}'; read b\n"
        "echo '{\"call\":\"download_request\",\"source\":\"s1\",\"pkg\":\"p1\"}'; read c\n"
        "a=$(echo \"$a\" | tr -d '\"{}\\\\'); b=$(echo \"$b\" | tr -d '\"{}'); c=$(echo \"$c\" | tr -d '\"{}')\n"
        "echo '{\"call\":\"result\",\"value\":\"A='\"$a\"' B='\"$b\"' C='\"$c\"'\"}'\n";
      (void)mkb("b.none", "[]", sc); bm.enable("b.none", {});
      InstalledPlugin pn; bm.get("b.none", &pn);
      RunOptions ro; ro.host = &fh;
      auto n1 = run_plugin(pn, root + "/bhome", ro);
      check(n1.completed && n1.denied == 3 && fh.lib == 0 && fh.src == 0 && fh.dl == 0, "broker: with no grants none of the library, store or download calls reach the host");
      check(n1.result.find("library_read was not granted") != std::string::npos && n1.result.find("store_add_source was not granted") != std::string::npos && n1.result.find("download_request was not granted") != std::string::npos, "broker: each refusal names the permission that was missing");
      (void)mkb("b.lib", "[\"library_read\"]", sc); bm.enable("b.lib", {"library_read"});
      InstalledPlugin pl; bm.get("b.lib", &pl);
      auto n2 = run_plugin(pl, root + "/bhome", ro);
      check(n2.completed && n2.served == 2 && fh.lib == 1 && fh.src == 0 && fh.dl == 0 && n2.result.find("Game One") != std::string::npos, "broker: library_read alone allows only library_list, which returns the games");
      (void)mkb("b.all", "[\"library_read\",\"store_add_source\",\"download_request\"]", sc); bm.enable("b.all", {"library_read", "store_add_source", "download_request"});
      InstalledPlugin pa; bm.get("b.all", &pa);
      auto n3 = run_plugin(pa, root + "/bhome", ro);
      check(n3.completed && n3.denied == 0 && fh.lib == 2 && fh.src == 1 && fh.dl == 1, "broker: with all three grants each call reaches the host once");
      fh.fail = true;
      auto n4 = run_plugin(pa, root + "/bhome", ro);
      check(n4.completed && n4.denied == 2 && n4.result.find("nope") != std::string::npos, "broker: a host refusal is passed back as an error and counted as denied");
      auto n5 = run_plugin(pa, root + "/bhome");
      check(n5.completed && n5.denied == 3 && n5.result.find("not available") != std::string::npos, "broker: with no host connected the calls are refused as not available");
    }
    // live host on temp files
    std::string db = root + "/live/library.db";
    fs::create_directories(root + "/live");
    LiveBrokerHost lh(db, "demo.plugin");
    std::string js, er;
    check(lh.library_list(&js, &er) && js == "[]" && !fs::exists(db), "live host: an empty or missing library lists nothing and is never created for a plugin");
    { pnr::library::GameDb gdb; gdb.open(db); pnr::library::GameRecord g; g.game_id = "g1"; g.title = "Game \"One\""; g.version = "1"; gdb.add_game(g); gdb.close(); }
    check(lh.library_list(&js, &er) && js.find("\"id\":\"g1\"") != std::string::npos && js.find("Game \\\"One\\\"") != std::string::npos, "live host: games come back with their title, quotes escaped");
    check(!lh.store_add_source("s1", "https://example.org/i.json", "", &er), "live host: a source needs a rights statement");
    check(!lh.store_add_source("s1", "http://example.org/i.json", "mine", &er) && !lh.store_add_source("BAD ID", "https://example.org/i.json", "mine", &er), "live host: a non-https URL or a bad source id is refused");
    check(lh.store_add_source("s1", "https://example.org/i.json", "my own files", &er), "live host: a valid source is added");
    pnr::store::Registry lreg(pnr::store::store_dir_for(db)); lreg.load();
    const pnr::store::Source* ls = lreg.find("s1");
    check(ls && ls->rights.find("declared by plugin demo.plugin") != std::string::npos && ls->rights.find("my own files") != std::string::npos && ls->pubkey.empty(), "live host: the rights text is marked as declared by the plugin, not by the user");
    check(!lh.store_add_source("s1", "https://example.org/i.json", "again", &er), "live host: a duplicate source id is refused");
    check(!lh.download_request("s1", "nothing", &er) && !lh.download_request("zzz", "p", &er), "live host: queuing a package that is not in a refreshed index, or from an unknown source, is refused");
  }
  fs::remove_all(root); }
