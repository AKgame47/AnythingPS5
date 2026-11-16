// Slice S33 tests: dependent modules (DT_NEEDED files). All inputs are SYNTHETIC modules from
// tools/synth_elf. No PlayStation file is used.
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "loader/module_loader.hpp"
#include "prx/prx.hpp"
#include "tools/synth_elf.hpp"

void pnr_depmod_tests(int& pass, int& fail) {
  using namespace pnr::loader;
  auto check = [&](bool ok, const std::string& l) { if (ok) ++pass; else ++fail; std::printf("%s %s\n", ok ? "PASS" : "FAIL", l.c_str()); };
  auto write = [](const std::string& p, const std::vector<std::uint8_t>& b) { std::ofstream f(p, std::ios::binary); f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size())); };

  pnr::prx::PrxRegistry reg;
  const std::string dir = "/tmp/pnr_depmod_" + std::to_string(::getpid());
  std::string cmd = "mkdir -p " + dir + "/sce_module";
  if (std::system(cmd.c_str()) != 0) return;

  // Basic tree: main needs libdep.prx; the dep is found in sce_module and bound first.
  write(dir + "/sce_module/libdep.prx", pnr::tools::build_synthetic_dep_elf({}));
  write(dir + "/main.elf", pnr::tools::build_synthetic_depmain_elf({}));
  ModuleLoadReport rep;
  std::unique_ptr<ModuleTree> tree;
  auto st = load_module_tree(dir + "/main.elf", {}, reg, &tree, &rep);
  check(st.ok && tree && tree->images.size() == 2, "depmod: main + one dependency loaded");
  check(st.ok && tree->images[0]->soname == "libdep.prx" && tree->images[1].get() == tree->main(),
        "depmod: dependency loads before the main module and carries its DT_SONAME");
  bool guest_bound = false;
  for (const auto& s : rep.bound) if (s.find("[guest module export]") != std::string::npos) guest_bound = true;
  check(st.ok && guest_bound, "depmod: imports bound against the guest module's exports");
  tree.reset();
  fflush(stdout);
  ModuleRunResult rr;
  st = run_module_tree(dir + "/main.elf", {}, reg, &rr, &rep);
  check(st.ok && rr.finished && !rr.signaled && rr.exit_code == pnr::tools::kDepMainExitReturn,
        "depmod: imported function and data read through the dependency (exit 55, order ghIABE21yxFvw on stdout)");

  // Transitive tree: main -> libdepa.prx -> libdepb.prx; dep_a's export calls dep_b's export.
  cmd = "mkdir -p " + dir + "/chain/sce_module";
  if (std::system(cmd.c_str()) != 0) return;
  pnr::tools::DepSpec db; db.soname = "libdepb.prx"; db.export_fn = "dep_b_value"; db.value = 1;
  db.letters[0] = 't'; db.letters[1] = 'u'; db.letters[2] = 'q'; db.letters[3] = 'r';
  pnr::tools::DepSpec da; da.soname = "libdepa.prx"; da.needed = "libdepb.prx"; da.import_fn = "dep_b_value"; da.value = 40;
  write(dir + "/chain/sce_module/libdepb.prx", pnr::tools::build_synthetic_dep_elf(db));
  write(dir + "/chain/sce_module/libdepa.prx", pnr::tools::build_synthetic_dep_elf(da));
  write(dir + "/chain/main.elf", pnr::tools::build_synthetic_depmain_elf({"libdepa.prx", "dep_value", 41, 80}));
  st = load_module_tree(dir + "/chain/main.elf", {}, reg, &tree, &rep);
  check(st.ok && tree->images.size() == 3 && tree->images[0]->soname == "libdepb.prx" && tree->images[1]->soname == "libdepa.prx",
        "depmod: transitive dependency loads deepest-first");
  tree.reset();
  fflush(stdout);
  st = run_module_tree(dir + "/chain/main.elf", {}, reg, &rr, &rep);
  check(st.ok && rr.finished && !rr.signaled && rr.exit_code == 55,
        "depmod: cross-module call chain returns 41 (order tughIABE21yxFvwqr on stdout)");

  // Bundled module overrides the host PRX registry: the dep exports strlen's NID returning 7.
  cmd = "mkdir -p " + dir + "/over/sce_module";
  if (std::system(cmd.c_str()) != 0) return;
  pnr::tools::DepSpec over; over.soname = "libover.prx"; over.export_fn = "strlen"; over.value = 7;
  write(dir + "/over/sce_module/libover.prx", pnr::tools::build_synthetic_dep_elf(over));
  write(dir + "/over/main.elf", pnr::tools::build_synthetic_depmain_elf({"libover.prx", "strlen", 7, 14}));
  fflush(stdout);
  st = run_module_tree(dir + "/over/main.elf", {}, reg, &rr, &rep);
  check(st.ok && rr.finished && !rr.signaled && rr.exit_code == 55,
        "depmod: bundled module takes precedence over the host PRX registry (strlen -> 7)");

  // A needed name with no matching file is firmware-provided: host binding still applies.
  write(dir + "/fw.elf", pnr::tools::build_synthetic_depmain_elf({"libSceNothingHere.prx", "strlen", 7, 14}));
  // No file matches libSceNothingHere.prx; imports putchar/atexit/strlen/dep_counter must come
  // from the host registry, and dep_counter is unknown -> the load must fail honestly.
  st = load_module_tree(dir + "/fw.elf", {}, reg, &tree, &rep);
  check(!st.ok && st.error.message.find("cannot bind") != std::string::npos,
        "depmod: firmware-needed name with an unimplemented import fails with the symbol named");

  // Cyclic dependencies are refused with a named reason.
  cmd = "mkdir -p " + dir + "/cyc/sce_module";
  if (std::system(cmd.c_str()) != 0) return;
  pnr::tools::DepSpec ca; ca.soname = "libca.prx"; ca.needed = "libcb.prx";
  pnr::tools::DepSpec cb; cb.soname = "libcb.prx"; cb.needed = "libca.prx";
  write(dir + "/cyc/sce_module/libca.prx", pnr::tools::build_synthetic_dep_elf(ca));
  write(dir + "/cyc/sce_module/libcb.prx", pnr::tools::build_synthetic_dep_elf(cb));
  write(dir + "/cyc/main.elf", pnr::tools::build_synthetic_depmain_elf({"libca.prx", "dep_value", 41, 82}));
  st = load_module_tree(dir + "/cyc/main.elf", {}, reg, &tree, &rep);
  check(!st.ok && st.error.message.find("cyclic") != std::string::npos, "depmod: cyclic dependency is refused");

  // A dependent module with PT_TLS is refused (only the main module's TLS is set up).
  cmd = "mkdir -p " + dir + "/tls/sce_module";
  if (std::system(cmd.c_str()) != 0) return;
  pnr::tools::DepSpec wt; wt.with_tls = true;
  write(dir + "/tls/sce_module/libdep.prx", pnr::tools::build_synthetic_dep_elf(wt));
  write(dir + "/tls/main.elf", pnr::tools::build_synthetic_depmain_elf({}));
  st = load_module_tree(dir + "/tls/main.elf", {}, reg, &tree, &rep);
  check(!st.ok && st.error.message.find("PT_TLS") != std::string::npos, "depmod: dependent module with PT_TLS is refused");

  // Ambiguity: the same needed name matches files in two search directories.
  cmd = "mkdir -p " + dir + "/amb/sce_module";
  if (std::system(cmd.c_str()) != 0) return;
  write(dir + "/amb/libdep.prx", pnr::tools::build_synthetic_dep_elf({}));
  write(dir + "/amb/sce_module/libdep.prx", pnr::tools::build_synthetic_dep_elf({}));
  write(dir + "/amb/main.elf", pnr::tools::build_synthetic_depmain_elf({}));
  st = load_module_tree(dir + "/amb/main.elf", {}, reg, &tree, &rep);
  check(!st.ok && st.error.message.find("ambiguous") != std::string::npos,
        "depmod: a needed name matching two files is an ambiguity error");

  cmd = "rm -rf " + dir;
  if (std::system(cmd.c_str()) != 0) {}
}
