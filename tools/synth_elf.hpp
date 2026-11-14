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

extern const char* const kSynthMessage;
constexpr int kSynthExitCode = 30;

}  // namespace pnr::tools
