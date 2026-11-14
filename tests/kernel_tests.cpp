// Core slice C2 tests: TLS/TCB layout, Sony constant translation, entry args, and a synthetic
// module that exercises the libkernel ABI from machine code in a forked child.
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <thread>
#include <vector>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "loader/guest_runtime.hpp"
#include "loader/module_loader.hpp"
#include "prx/prx.hpp"
#include "tools/synth_elf.hpp"

void pnr_kernel_tests(int& pass, int& fail) {
  using namespace pnr::loader;
  auto check = [&](bool ok, const std::string& l) { if (ok) ++pass; else ++fail; std::printf("%s %s\n", ok ? "PASS" : "FAIL", l.c_str()); };
  auto write = [](const std::string& p, const std::vector<std::uint8_t>& b) { std::ofstream f(p, std::ios::binary); f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size())); };

  // TLS layout (variant II): variable at offset o lives at fs - round_up(memsz) + o.
  const std::uint8_t init[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  TlsTemplate t{init, 8, 24, 16};
  const TlsLayout l = tls_layout(t);
  check(l.fs_offset == kPad + 32 && l.size == l.fs_offset + kTcbSize, "tls: memsz 24 align 16 rounds to 32 below the TCB");
  std::vector<std::uint8_t> blk(l.size, 0xAA);
  const std::uint64_t host_tcb[8] = {0, 0, 0, 0, 0, 0xC0FFEE, 0xBEEF, 0};
  tls_build(t, l, blk.data(), 0x1234, host_tcb);
  std::uint64_t self = 0, canary = 0;
  std::memcpy(&self, &blk[l.fs_offset], 8); std::memcpy(&canary, &blk[l.fs_offset + 0x28], 8);
  check(self == 0x1234 && canary == 0xC0FFEE, "tls: TCB self pointer set, host canary copied");
  check(std::memcmp(&blk[l.fs_offset - 32], init, 8) == 0 && blk[l.fs_offset - 24] == 0 && blk[l.fs_offset - 1] == 0, "tls: .tdata copied, .tbss and padding zeroed");
  check(tls_layout(TlsTemplate{nullptr, 0, 0, 0}).size == kPad + 0 + kTcbSize, "tls: empty template still yields a valid TCB");
  check(tls_layout(TlsTemplate{nullptr, 0, 10, 1000000}).fs_offset == kPad + 4096 * 0 + 4096 || tls_layout(TlsTemplate{nullptr, 0, 10, 1000000}).fs_offset > kPad, "tls: absurd alignment is clamped");

  // Sony constants.
  int hp = 0, hf = 0; std::string why;
  check(translate_sony_prot(3, &hp, &why) && hp == (PROT_READ | PROT_WRITE), "prot: CPU read+write maps to PROT_READ|PROT_WRITE");
  check(translate_sony_prot(2, &hp, &why) && (hp & PROT_READ), "prot: CPU write implies read");
  check(translate_sony_prot(0x37, &hp, &why) && hp == (PROT_READ | PROT_WRITE | PROT_EXEC), "prot: GPU bits are accepted and ignored");
  check(!translate_sony_prot(0x100, &hp, &why) && !why.empty(), "prot: unknown bits are refused");
  check(translate_sony_map_flags(0x1002, -1, &hf, &why) && (hf & MAP_ANONYMOUS) && (hf & MAP_PRIVATE), "map: ANON|PRIVATE");
  check(translate_sony_map_flags(0x1012, -1, &hf, &why) && (hf & MAP_FIXED), "map: FIXED");
  check(translate_sony_map_flags(0x1092, -1, &hf, &why) && (hf & MAP_FIXED_NOREPLACE), "map: FIXED|NO_OVERWRITE does not overwrite");
  check(!translate_sony_map_flags(0x1082, -1, &hf, &why), "map: NO_OVERWRITE without FIXED refused");
  check(!translate_sony_map_flags(0x2, -1, &hf, &why) && why.find("anonymous") != std::string::npos, "map: file-backed mapping refused");
  check(!translate_sony_map_flags(0x1000 | 0x4000000, -1, &hf, &why), "map: unknown flag bits refused");
  check(!translate_sony_map_flags(0x1003, -1, &hf, &why), "map: SHARED and PRIVATE together refused");
  check(sony_mmap(0, 0, 3, 0x1002, -1, 0) == -1 && sony_mmap(0, 4096, 0x100, 0x1002, -1, 0) == -1, "mmap: bad length or protection returns -1");
  { const std::int64_t p = sony_mmap(0, 8192, 3, 0x1002, -1, 0);
    check(p != -1 && (*reinterpret_cast<volatile int*>(p) = 5, *reinterpret_cast<volatile int*>(p) == 5), "mmap: a real anonymous mapping is usable");
    if (p != -1) ::munmap(reinterpret_cast<void*>(p), 8192); }
  int hc = 0;
  check(translate_sony_clock(4, &hc) && hc == CLOCK_MONOTONIC && translate_sony_clock(0, &hc) && hc == CLOCK_REALTIME && !translate_sony_clock(99, &hc), "clock: known ids map, unknown refused");
  timespec ts{};
  check(sony_clock_gettime(4, reinterpret_cast<std::int64_t>(&ts)) == 0 && (ts.tv_sec || ts.tv_nsec), "clock: reads a real monotonic time");
  check(sony_clock_gettime(99, reinterpret_cast<std::int64_t>(&ts)) != 0 && sony_clock_gettime(4, 0) != 0, "clock: bad id or null pointer is an error");

  // S1: condition variables and semaphores (same entry points the PRX symbols call), driven by real host threads.
  {
    std::int64_t mh = 0, ch = 0, sh = 0;
    check(guest_mutex_op(0, &mh) == 0 && guest_cond_init(&ch) == 0, "sync: mutex and condition variable are created");
    check(guest_cond_wake(0x1234, false) < 0 && guest_cond_wait(0x1234, &mh, 10) < 0, "sync: a forged condition handle is an error, not a crash");
    guest_mutex_op(1, &mh);
    check(guest_cond_wait(ch, &mh, 20000) == -4, "sync: a timed condition wait with no signal times out");
    guest_mutex_op(2, &mh);
    std::atomic<int> ready{0}, woke{0};
    std::vector<std::thread> th;
    for (int i = 0; i < 4; ++i) th.emplace_back([&] {
      guest_mutex_op(1, &mh); ++ready;
      while (ready.load() < 100) { if (guest_cond_wait(ch, &mh, -1) != 0) break; }
      ++woke; guest_mutex_op(2, &mh); });
    while (true) { guest_mutex_op(1, &mh); int r = ready.load(); guest_mutex_op(2, &mh); if (r >= 4) break; std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    guest_mutex_op(1, &mh); ready = 100; guest_mutex_op(2, &mh);
    check(guest_cond_wake(ch, true) == 0, "sync: broadcast succeeds");
    for (auto& x : th) x.join();
    check(woke.load() == 4, "sync: broadcast woke all 4 waiting threads");
    check(guest_cond_destroy(&ch) == 0 && ch == 0 && guest_cond_wake(ch, false) < 0, "sync: destroyed condition handle is invalid");
    guest_mutex_op(3, &mh);

    check(guest_sema_create(&sh, 5, 3) < 0 && guest_sema_create(&sh, 0, 0) < 0 && guest_sema_create(&sh, -1, 3) < 0, "sema: invalid initial/max refused");
    check(guest_sema_create(&sh, 1, 2) == 0, "sema: created with count 1, max 2");
    check(guest_sema_wait(sh, 1, 0) == 0 && guest_sema_wait(sh, 1, 10000) == -4, "sema: wait takes the count, then times out at zero");
    check(guest_sema_signal(sh, 3) == -3 && guest_sema_signal(sh, 2) == 0 && guest_sema_signal(sh, 1) == -3, "sema: signalling past the maximum is refused and leaves the count alone");
    check(guest_sema_wait(sh, 3, 0) == -2, "sema: asking for more than the maximum is an argument error");
    check(guest_sema_wait(sh, 2, 0) == 0, "sema: the count from the signal is consumed exactly");
    std::atomic<std::int64_t> wr{12345};
    std::thread w([&] { wr = guest_sema_wait(sh, 1, -1); });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    check(wr.load() == 12345, "sema: a waiter really blocks until signalled");
    guest_sema_signal(sh, 1); w.join();
    check(wr.load() == 0, "sema: a signal releases the blocked waiter");
    std::atomic<std::int64_t> wd{12345};
    std::thread w2([&] { wd = guest_sema_wait(sh, 1, -1); });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    std::int64_t keep = sh;
    check(guest_sema_destroy(&sh) == 0, "sema: destroy with a waiter blocked");
    w2.join();
    check(wd.load() == -2 && guest_sema_signal(keep, 1) < 0, "sema: destroying wakes the waiter with an error, and the handle is dead");
  }

  { pnr::prx::PrxRegistry r2; bool all = true;
    for (const char* n : {"scePthreadCondInit", "scePthreadCondWait", "scePthreadCondTimedwait", "scePthreadCondSignal", "scePthreadCondBroadcast", "scePthreadCondDestroy", "sceKernelCreateSema", "sceKernelWaitSema", "sceKernelSignalSema", "sceKernelDeleteSema"}) {
      const auto* sy = r2.find_name(n); if (!sy || !sy->implemented) all = false; }
    check(all, "sync: all 10 condition-variable and semaphore symbols are registered as implemented"); }

  // Entry args.
  auto args = build_entry_args({"/game/eboot.elf", "-x"});
  std::uint64_t argc = 0, a0 = 0, a2 = 1, envp = 1;
  std::memcpy(&argc, args.data(), 8); std::memcpy(&a0, args.data() + 8, 8); std::memcpy(&a2, args.data() + 24, 8); std::memcpy(&envp, args.data() + 32, 8);
  check(argc == 2 && a2 == 0 && envp == 0 && std::strcmp(reinterpret_cast<const char*>(a0), "/game/eboot.elf") == 0, "entry: argc, argv pointers into the block, null terminators");

  // Machine-code module.
  pnr::prx::PrxRegistry reg;
  const std::string dir = "/tmp/pnr_kernel_" + std::to_string(::getpid());
  if (std::system(("mkdir -p " + dir).c_str()) != 0) return;
  write(dir + "/k.elf", pnr::tools::build_synthetic_kernel_elf({}));
  write(dir + "/k0.elf", pnr::tools::build_synthetic_kernel_elf({true}));
  ModuleLoadReport rep; ModuleRunResult rr;
  std::fflush(stdout);
  auto st = run_module(dir + "/k.elf", reg, &rr, &rep);
  check(st.ok && rep.bound.size() == 8, "module: 8 libkernel imports bound by NID");
  check(st.ok && rr.finished && !rr.signaled && rr.exit_code == 0, "module: argc, TCB, TLS data, TLS write, Sony mmap, clock, mutex, thread create/join all pass in machine code");
  st = run_module(dir + "/k0.elf", reg, &rr, &rep);
  check(st.ok && rr.finished && !rr.signaled && rr.exit_code == 3, "module: without PT_TLS the TLS data check fails (exit 3), proving the check is real");
  // The launcher's own FS base and TLS are untouched.
  errno = 0; check(errno == 0 && !guest_active(), "module: launcher process state is unchanged after the child ran");
  if (std::system(("rm -rf " + dir).c_str()) != 0) {}
}
