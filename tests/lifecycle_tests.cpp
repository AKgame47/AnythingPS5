// Slice S32 tests: DT_INIT/DT_INIT_ARRAY/DT_FINI_ARRAY/DT_FINI and libc atexit.
// All inputs are SYNTHETIC modules from tools/synth_elf. No PlayStation file is used.
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "loader/module_loader.hpp"
#include "prx/prx.hpp"
#include "tools/synth_elf.hpp"

void pnr_lifecycle_tests(int& pass, int& fail) {
  using namespace pnr::loader;
  auto check = [&](bool ok, const std::string& l) { if (ok) ++pass; else ++fail; std::printf("%s %s\n", ok ? "PASS" : "FAIL", l.c_str()); };
  auto write = [](const std::string& p, const std::vector<std::uint8_t>& b) { std::ofstream f(p, std::ios::binary); f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size())); };

  pnr::prx::PrxRegistry reg;
  const std::string dir = "/tmp/pnr_lifecycle_" + std::to_string(::getpid());
  std::string cmd = "mkdir -p " + dir;
  if (std::system(cmd.c_str()) != 0) return;

  const auto lc = pnr::tools::build_synthetic_lifecycle_elf({});
  write(dir + "/lc.elf", lc);

  // Load: lifecycle addresses are parsed, relocated and recorded in call order.
  std::unique_ptr<ModuleImage> img;
  ModuleLoadReport rep;
  auto st = load_module(dir + "/lc.elf", reg, &img, &rep);
  check(st.ok && img, "lifecycle: module with init/fini dynamic tags loads");
  check(st.ok && img->init_fn != 0 && img->fini_fn != 0, "lifecycle: DT_INIT and DT_FINI recorded as absolute addresses");
  check(st.ok && img->init_array.size() == 2 && img->fini_array.size() == 2, "lifecycle: both array entries read after relocation");
  check(st.ok && img->init_array[0] != img->init_array[1] && img->init_array[0] >= reinterpret_cast<std::uint64_t>(img->base) &&
            img->init_array[0] < reinterpret_cast<std::uint64_t>(img->base) + img->span,
        "lifecycle: init array entries are distinct absolute pointers into the image (RELATIVE-relocated)");
  img.reset();

  // Malformed lifecycle tags are refused, never run.
  { auto b = lc; b[0x3c0 + 16 * 9 + 8] = 3; write(dir + "/bad_sz.elf", b);  // DT_INIT_ARRAYSZ = 3
    st = load_module(dir + "/bad_sz.elf", reg, &img, &rep);
    check(!st.ok && st.error.message.find("multiple of 8") != std::string::npos, "lifecycle: array size not a multiple of 8 is refused"); }
  { auto b = lc; b[0x3c0 + 16 * 8 + 8] = 0xf0; b[0x3c0 + 16 * 8 + 9] = 0xff; write(dir + "/bad_arr.elf", b);  // DT_INIT_ARRAY far outside
    st = load_module(dir + "/bad_arr.elf", reg, &img, &rep);
    check(!st.ok && st.error.message.find("DT_INIT_ARRAY") != std::string::npos, "lifecycle: DT_INIT_ARRAY outside the image is refused"); }
  { auto b = lc; b[0x3c0 + 16 * 7 + 8] = 0xf0; b[0x3c0 + 16 * 7 + 9] = 0xff; write(dir + "/bad_init.elf", b);  // DT_INIT far outside
    st = load_module(dir + "/bad_init.elf", reg, &img, &rep);
    check(!st.ok && st.error.message.find("DT_INIT/DT_FINI") != std::string::npos, "lifecycle: DT_INIT outside the image is refused"); }

  // atexit without a running guest module is an error, not a crash.
  check(guest_atexit_register(0x1234) == -2, "lifecycle: atexit with no active guest is refused");

  // Run for real, entry-returns path: exit code 55.
  fflush(stdout);
  ModuleRunResult rr;
  st = run_module(dir + "/lc.elf", reg, &rr, &rep);
  check(st.ok && rr.finished && !rr.signaled && rr.exit_code == pnr::tools::kLifecycleExitReturn,
        "lifecycle: init ran before entry and atexit+fini ran after it returned (order IABE21yxF on stdout)");
  // Run for real, guest-calls-exit path: exit code 42, same sequence.
  write(dir + "/lcx.elf", pnr::tools::build_synthetic_lifecycle_elf({true}));
  fflush(stdout);
  st = run_module(dir + "/lcx.elf", reg, &rr, &rep);
  check(st.ok && rr.finished && !rr.signaled && rr.exit_code == pnr::tools::kLifecycleExitExit,
        "lifecycle: guest exit() runs atexit handlers LIFO then fini (order IABE21yxF on stdout)");

  cmd = "rm -rf " + dir;
  if (std::system(cmd.c_str()) != 0) {}
}
