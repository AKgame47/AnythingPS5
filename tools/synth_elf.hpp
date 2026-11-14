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

extern const char* const kSynthMessage;
constexpr int kSynthExitCode = 30;

}  // namespace pnr::tools
