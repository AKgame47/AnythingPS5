// Builds SYNTHETIC PlayStation-convention ELF modules for tests (Core slice C1), GPL-2.0.
// The output is a real, hand-assembled x86-64 ET_DYN file with NID-named imports and no
// section headers. It is NOT a PlayStation executable and contains no Sony code.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pnr::tools {

struct SynthSpec {
  bool include_missing_import = false;  // adds an import whose NID is not in any table
};

// The program calls printf("hello from a NID-bound module\n") then strlen() of that text
// through NID-named imports, and returns the length (30) as its exit code.
std::vector<std::uint8_t> build_synthetic_ps_elf(const SynthSpec& spec);

// Slice C2: a module that exercises the libkernel ABI from machine code. It returns 0 when every
// check passes, otherwise the number of the first failing check (1..14):
//  1 argc==1   2 TCB self pointer   3 .tdata initial value   4 .tbss zeroed   5 TLS write/read
//  6 mmap with Sony flags   7 mapped memory usable   8 clock_gettime   9-12 mutex init/lock/unlock/destroy
//  13 thread create   14 thread ran with its own TCB and joined
// `break_tls` drops PT_TLS so check 3 must fail (negative test).
struct KernelSpec { bool break_tls = false; };
std::vector<std::uint8_t> build_synthetic_kernel_elf(const KernelSpec& spec);

// Slice C3: a module that calls libc functions through NID-bound imports from machine code.
// Returns 0 when all checks pass, else the number of the first failing check (1..5), and prints
// "C3 libc ok 42" through the bound printf.
std::vector<std::uint8_t> build_synthetic_libc_elf();

// Slice S35 (review follow-up): a module calling fixed-signature FP functions through NID-bound
// imports. Returns 0 when all checks pass, else the failing check number (1..4), and prints
// "1.0 2.0 3.0 4.0 5.0 6.0 7.0 8.0" through printf (8 xmm argument lanes live).
std::vector<std::uint8_t> build_synthetic_fp_elf();

// Slice S36: a module calling printf with both register pools exhausted and the stack overflow
// interleaved across classes (i6, i7, i8, then the 9th double, in argument order). Returns 0
// only when the host consumes the spill window correctly; prints "1 11 2 12 ... 8 18 9".
std::vector<std::uint8_t> build_synthetic_stkovf_elf();

// Slice S37: a module reading an asset through the jailed file calls: opens "/app0/asset.txt",
// reads 12 bytes, byte-compares "pnr-asset-42", prints "asset ok", exits 0 on success (1..5
// otherwise). Needs a jail with asset.txt seeded; module-run uses PNR_JAIL_ROOT.
std::vector<std::uint8_t> build_synthetic_file_elf();
std::vector<std::uint8_t> build_synthetic_filew_elf();
std::vector<std::uint8_t> build_synthetic_filepos_elf();
std::vector<std::uint8_t> build_synthetic_abi7_elf();

// Slice S32: a module with DT_INIT, DT_INIT_ARRAY (2), DT_FINI_ARRAY (2), DT_FINI and two
// atexit handlers registered from its entry point. Every lifecycle function prints one letter
// through the bound putchar; the exact stdout order "IABE21yxF" proves the call order:
//   I = DT_INIT, A B = init array, E = entry body, 2 1 = atexit LIFO, y x = fini array reversed,
//   F = DT_FINI. Entry returns kLifecycleExitReturn (55), or calls exit(kLifecycleExitExit=42)
//   when `use_exit` is set; the atexit/fini sequence must run on BOTH paths.
struct LifecycleSpec { bool use_exit = false; };
std::vector<std::uint8_t> build_synthetic_lifecycle_elf(const LifecycleSpec& spec);
constexpr int kLifecycleExitReturn = 55, kLifecycleExitExit = 42;
constexpr const char* kLifecycleOrder = "IABE21yxF";

// Slice S33: dependent modules.
// build_synthetic_dep_elf makes a .prx-style module: DT_SONAME, an exported function
// (returns `value`), an exported data symbol dep_counter (holds 2*value), one DT_INIT and one
// DT_INIT_ARRAY entry printing letters[0]/letters[1] through putchar, and DT_FINI_ARRAY +
// DT_FINI printing letters[2]/letters[3]. With `import_fn` set, the exported function instead
// returns import_fn() + value (import resolved from another module, proving cross-module
// binding). `needed` adds a DT_NEEDED entry by that name. `with_tls` adds a PT_TLS segment
// (the loader must refuse dependent modules that have one).
struct DepSpec {
  std::string soname = "libdep.prx";
  std::string export_fn = "dep_value";   // plain name; exported as NID#A#A like real PS modules
  int value = 41;
  std::string needed;                    // optional DT_NEEDED name
  std::string import_fn;                 // optional import (plain name) called by export_fn
  char letters[4] = {'g', 'h', 'v', 'w'};  // init, init-array, fini-array, fini
  bool with_tls = false;
};
std::vector<std::uint8_t> build_synthetic_dep_elf(const DepSpec& spec);

// Slice S33: a main module with a DT_NEEDED entry. Imports putchar+atexit from the host and
// import_fn + dep_counter from the dependent module. Entry prints 'E', fails with exit 61/62
// when the imported function/data does not match, registers two atexit handlers ('1','2') and
// returns 55. Lifecycle letters are the S32 ones, so a one-dependency run prints
// "ghIABE21yxFvw": dep init first, dep fini last.
struct DepMainSpec {
  std::string needed = "libdep.prx";
  std::string import_fn = "dep_value";
  int expect_fn = 41;
  int expect_data = 82;
};
std::vector<std::uint8_t> build_synthetic_depmain_elf(const DepMainSpec& spec);
constexpr int kDepMainExitReturn = 55;
constexpr const char* kDepOrder = "ghIABE21yxFvw";

// Slice S34: a C++-style module exercising the C++ ABI surface from machine code.
// Entry prints 'E'; __cxa_guard_acquire on a guard byte must return 1 then 0 ('G'); qsort
// sorts {5,3,8,1} descending through a guest comparator ('Q'); it registers
// __cxa_thread_atexit_impl(dtor_t, 0x51) and __cxa_atexit(dtor_c, 0x42) and returns 55.
// Each destructor prints the LOW BYTE of its argument: the exit sequence MUST print
// 'Q' before 'B' (thread destructors run before atexit handlers, glibc order) with the
// exact bytes proving both handlers received their registered arguments (0x51, 0x42).
// Failures exit 61 (guard/registration) or 62 (qsort result).
std::vector<std::uint8_t> build_synthetic_cxx_elf();
constexpr int kCxxExitReturn = 55;
constexpr const char* kCxxOrder = "EGQQB";

extern const char* const kSynthMessage;
constexpr int kSynthExitCode = 30;

}  // namespace pnr::tools
