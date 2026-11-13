// PNR Loader - Binary Parser (part A of MODULE 02)
// Real ELF64 parser + validator for x86-64 executables.
// Protected PS5 formats are NOT parsed here; they are recognized as
// unsupported and rejected with an explicit error (never fabricated).
// Status: Implemented (unit-tested).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pnr::loader {

struct ElfSegment {
  uint32_t type = 0;    // PT_* value (PT_LOAD == 1)
  uint32_t flags = 0;   // PF_X=1, PF_W=2, PF_R=4
  uint64_t offset = 0;
  uint64_t vaddr = 0;
  uint64_t filesz = 0;
  uint64_t memsz = 0;
};

struct ElfInfo {
  bool is_elf = false;
  bool is_64bit = false;
  bool little_endian = false;
  uint16_t elf_type = 0;    // ET_EXEC=2, ET_DYN=3
  uint16_t machine = 0;     // EM_X86_64=62
  uint64_t entry = 0;
  std::vector<ElfSegment> segments;
};

struct ParseResult {
  bool ok = false;
  std::string error;  // human-readable reason when ok==false
  ElfInfo info;
};

// Parses and validates an ELF file from disk. Bounds-checked; a malformed
// or truncated file fails with a specific error instead of a crash.
ParseResult parse_elf(const std::string& path);
ParseResult parse_elf_bytes(const std::vector<uint8_t>& bytes);

bool is_x86_64(const ElfInfo& info);
bool is_executable_type(const ElfInfo& info);

}  // namespace pnr::loader
