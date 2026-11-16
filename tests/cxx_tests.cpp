// Slice S34 tests: the C++ ABI surface (__cxa_atexit, __cxa_thread_atexit_impl, qsort,
// __cxa_guard_*, __cxa_pure_virtual). All inputs are SYNTHETIC modules from tools/synth_elf.
// No PlayStation file is used.
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <string>
#include <thread>

#include "loader/guest_runtime.hpp"
#include "loader/module_loader.hpp"
#include "prx/prx.hpp"
#include "tools/synth_elf.hpp"

void pnr_cxx_tests(int& pass, int& fail) {
  using namespace pnr::loader;
  auto check = [&](bool ok, const std::string& l) { if (ok) ++pass; else ++fail; std::printf("%s %s\n", ok ? "PASS" : "FAIL", l.c_str()); };
  auto write = [](const std::string& p, const std::vector<std::uint8_t>& b) { std::ofstream f(p, std::ios::binary); f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size())); };

  pnr::prx::PrxRegistry reg;
  const std::string dir = "/tmp/pnr_cxx_" + std::to_string(::getpid());
  std::string cmd = "mkdir -p " + dir;
  if (std::system(cmd.c_str()) != 0) return;

  // Load: the C++ ABI imports bind like any other NID-bound symbol.
  write(dir + "/cxx.elf", pnr::tools::build_synthetic_cxx_elf());
  std::unique_ptr<ModuleImage> img;
  ModuleLoadReport rep;
  auto st = load_module(dir + "/cxx.elf", reg, &img, &rep);
  check(st.ok && img, "cxx: module importing the C++ ABI surface loads");
  img.reset();

  // Registration without a running guest module is an error, not a crash.
  check(guest_cxa_atexit_register(0x1234, 0x55) == -2, "cxx: __cxa_atexit with no active guest is refused");
  check(guest_cxa_thread_atexit_register(0x1234, 0x51) == -2, "cxx: __cxa_thread_atexit_impl with no active guest is refused");

  // Symbols exist and are implemented.
  const char* names[] = {"__cxa_atexit", "__cxa_thread_atexit_impl", "qsort", "__cxa_guard_acquire",
                         "__cxa_guard_release", "__cxa_guard_abort", "__cxa_pure_virtual"};
  for (const char* n : names) {
    const pnr::prx::PrxSymbol* s = reg.find_name(n);
    check(s && s->implemented, std::string("cxx: ") + n + " is implemented");
  }
  auto call = [&](const char* name, std::vector<std::int64_t> a) -> std::int64_t {
    const pnr::prx::PrxSymbol* s = reg.find_name(name);
    if (!s || !s->implemented) return -999999;
    return s->invoke(a);
  };
  auto P = [](const void* p) { return reinterpret_cast<std::int64_t>(p); };

  // __cxa_guard_*: first acquire takes it; a concurrent acquire BLOCKS until the initializer
  // releases (one static is never initialized twice); abort clears and re-arms.
  { std::uint8_t g[8] = {};
    check(call("__cxa_guard_acquire", {P(g)}) == 1, "cxx: guard acquire on an uninitialized guard takes it");
    std::atomic<int> other{-1};
    std::thread t([&] { other.store(static_cast<int>(call("__cxa_guard_acquire", {P(g)}))); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    check(other.load() == -1, "cxx: guard acquire during an in-progress init blocks");
    check(call("__cxa_guard_release", {P(g)}) == 0 && g[0] == 1, "cxx: guard release marks the guard initialized");
    for (int i = 0; i < 100 && other.load() == -1; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const int woke = other.load();
    if (woke == -1) t.detach(); else t.join();
    check(woke == 0, "cxx: blocked acquirer wakes after release and does not take it");
    check(call("__cxa_guard_acquire", {P(g)}) == 0, "cxx: guard acquire after release does not take it");
    std::uint8_t h[8] = {};
    check(call("__cxa_guard_acquire", {P(h)}) == 1, "cxx: second guard is independent");
    check(call("__cxa_guard_abort", {P(h)}) == 0 && h[0] == 0 && h[1] == 0, "cxx: guard abort clears initialized and pending");
    check(call("__cxa_guard_acquire", {P(h)}) == 1, "cxx: guard acquire after abort takes it again");
    check(call("__cxa_guard_release", {P(h)}) == 0 && h[0] == 1, "cxx: re-armed guard releases normally");
    check(call("__cxa_guard_acquire", {0}) == pnr::prx::SCE_KERNEL_ERROR_EINVAL, "cxx: guard acquire with a null guard is EINVAL"); }

  // qsort argument validation (the comparator path itself is exercised by the module run below:
  // it needs a live guest FS, so it cannot run from this unit-test process).
  { int arr[4] = {5, 3, 8, 1};
    check(call("qsort", {P(arr), 4, 4, 0}) == pnr::prx::SCE_KERNEL_ERROR_EINVAL, "cxx: qsort with a null comparator is EINVAL");
    check(call("qsort", {0, 4, 4, P(arr)}) == pnr::prx::SCE_KERNEL_ERROR_EINVAL, "cxx: qsort with a null base and nonzero count is EINVAL");
    check(call("qsort", {0, 0, 4, P(arr)}) == 0 && arr[0] == 5 && arr[3] == 1, "cxx: qsort with zero elements is a no-op"); }

  // Run for real: guard, qsort with a guest comparator that makes a NESTED host call on its
  // first invocation ('n'), and the glibc exit order (thread destructors before atexit
  // handlers). The last destructor hard-verifies (UD2 -> signaled run) that the nested call
  // happened exactly once, that the thread dtor ran first with its registered argument, and
  // that its own argument arrived intact - then branches on that argument and makes two more
  // host calls. Expected order "EGnQQbB" on stdout, exit 55.
  fflush(stdout);
  ModuleRunResult rr;
  st = run_module(dir + "/cxx.elf", reg, &rr, &rep);
  check(st.ok && rr.finished && !rr.signaled && rr.exit_code == pnr::tools::kCxxExitReturn,
        "cxx: guard + nested-callback qsort + dtors (branching, multi-call, arg-checked) run in order EGnQQbB, exit 55");

  cmd = "rm -rf " + dir;
  if (std::system(cmd.c_str()) != 0) {}
}
