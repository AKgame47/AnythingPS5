// Core slice C1 tests: Sony-style symbol names, in-process module loading, NID binding.
// All inputs are SYNTHETIC modules from tools/synth_elf. No PlayStation file is used.
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "loader/module_loader.hpp"
#include "loader/nid.hpp"
#include "loader/sony_symbols.hpp"
#include "prx/prx.hpp"
#include "tools/synth_elf.hpp"

void pnr_module_tests(int& pass, int& fail) {
  using namespace pnr::loader;
  auto check = [&](bool ok, const std::string& l) { if (ok) ++pass; else ++fail; std::printf("%s %s\n", ok ? "PASS" : "FAIL", l.c_str()); };
  auto has = [](const std::string& h, const std::string& n) { return h.find(n) != std::string::npos; };
  auto write = [](const std::string& p, const std::vector<std::uint8_t>& b) { std::ofstream f(p, std::ios::binary); f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size())); };

  // Name form.
  SonySymbol s;
  check(parse_sony_symbol("bzQExy189ZI#a#b", &s) && s.nid == "bzQExy189ZI" && s.lib_id == 26 && s.mod_id == 27, "sony: NID#lib#mod parses and decodes ids");
  check(!parse_sony_symbol("printf", nullptr), "sony: a plain name is not NID form");
  check(!parse_sony_symbol("bzQExy189ZI#a", nullptr), "sony: missing module part rejected");
  check(!parse_sony_symbol("bzQExy189Z!#a#b", nullptr), "sony: bad NID character rejected");
  check(!parse_sony_symbol("bzQExy189ZI#a#bbbb", nullptr), "sony: over-long id rejected");
  check(decode_sony_id("A") == 0 && decode_sony_id("BA") == 64 && decode_sony_id("-") == 63 && decode_sony_id("") == -1, "sony: id decoding");
  check(pnr::loader::nid::compute_nid("printf", "").size() == 11, "nid: 11 characters");

  pnr::prx::PrxRegistry reg;
  const std::string dir = "/tmp/pnr_module_" + std::to_string(::getpid());
  std::string cmd = "mkdir -p " + dir;
  if (std::system(cmd.c_str()) != 0) return;

  // Import listing without section headers.
  const auto good = pnr::tools::build_synthetic_ps_elf({});
  write(dir + "/good.elf", good);
  ImportResolution ir;
  auto st = resolve_imports(dir + "/good.elf", reg, &ir);
  check(st.ok && ir.imports.size() == 2 && ir.prx_implemented == 2 && ir.imports[0].sony_form && ir.imports[0].resolved_name == "printf", "imports: NID-form imports are resolved by NID (no section headers needed)");
  const auto miss = pnr::tools::build_synthetic_ps_elf({true});
  write(dir + "/miss.elf", miss);
  ImportResolution ir2;
  st = resolve_imports(dir + "/miss.elf", reg, &ir2);
  check(st.ok && ir2.imports.size() == 3 && ir2.unknown == 1 && ir2.imports[2].klass == ImportClass::Unknown, "imports: an unknown NID is reported as unknown");

  // Load, bind, run for real.
  std::unique_ptr<ModuleImage> img;
  ModuleLoadReport rep;
  st = load_module(dir + "/good.elf", reg, &img, &rep);
  check(st.ok && rep.bound.size() == 2 && rep.relocations == 3, "load: relocations applied and both imports bound by NID");
  check(st.ok && has(rep.bound[0], "printf"), "load: bound list names the symbol and NID");
  img.reset();
  fflush(stdout);
  ModuleRunResult rr;
  st = run_module(dir + "/good.elf", reg, &rr, &rep);
  check(st.ok && rr.finished && !rr.signaled && rr.exit_code == pnr::tools::kSynthExitCode, "run: the generated machine code ran natively and returned strlen() through two NID-bound calls");

  // Unknown NID fails loudly and does not run.
  st = run_module(dir + "/miss.elf", reg, &rr, &rep);
  check(!st.ok && !rr.started && has(st.error.message, "cannot bind 1 import") && has(st.error.message, "[unknown]"), "load: unknown NID refuses to load, names the import, never runs");

  // Malformed inputs.
  { auto b = good; b[0x2c0 + 16 * 4 + 8] = 0xff; b[0x2c0 + 16 * 4 + 9] = 0xff; write(dir + "/bad1.elf", b);  // DT_RELA to a bad address
    check(!load_module(dir + "/bad1.elf", reg, &img, &rep).ok, "load: relocation table outside the image is rejected"); }
  { auto b = good; b[0x240 + 24 + 8] = 99; write(dir + "/bad2.elf", b);  // reloc type 99
    st = load_module(dir + "/bad2.elf", reg, &img, &rep);
    check(!st.ok && has(st.error.message, "unsupported relocation type 99"), "load: unsupported relocation type is refused"); }
  { auto b = good; b[0x240 + 24 + 12] = 77; write(dir + "/bad3.elf", b);  // symbol index 77
    check(!load_module(dir + "/bad3.elf", reg, &img, &rep).ok, "load: bad symbol index is rejected"); }
  { auto b = good; b[0x18] = 0; b[0x19] = 0x90; write(dir + "/bad4.elf", b);  // entry far outside
    st = load_module(dir + "/bad4.elf", reg, &img, &rep);
    check(!st.ok && has(st.error.message, "entry point"), "load: entry point outside the image is rejected"); }
  { auto b = good; b.resize(0x90); write(dir + "/bad5.elf", b);
    check(!load_module(dir + "/bad5.elf", reg, &img, &rep).ok, "load: truncated file is rejected"); }
  { std::vector<std::uint8_t> self = {0x4F, 0x15, 0x3D, 0x1D, 0, 0, 0, 0, 0, 0, 0, 0}; self.resize(256); write(dir + "/x.self", self);
    st = load_module(dir + "/x.self", reg, &img, &rep);
    check(!st.ok && has(st.error.message, "SELF container") && has(st.error.message, "does not decrypt"), "load: a SELF container is refused with a clear message (no decryption)"); }
  check(!load_module(dir + "/nope.elf", reg, &img, &rep).ok, "load: missing file is an error");
  cmd = "rm -rf " + dir;
  if (std::system(cmd.c_str()) != 0) {}
}
