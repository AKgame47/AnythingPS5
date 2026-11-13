#include "loader/elf_parser.hpp"

#include <cstring>
#include <fstream>

namespace pnr::loader {
namespace {

constexpr uint16_t EM_X86_64 = 62;
constexpr uint16_t ET_EXEC = 2;
constexpr uint16_t ET_DYN = 3;

uint16_t rd16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
uint32_t rd32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t rd64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

}  // namespace

bool is_x86_64(const ElfInfo& info) { return info.machine == EM_X86_64; }

bool is_executable_type(const ElfInfo& info) {
  return info.elf_type == ET_EXEC || info.elf_type == ET_DYN;
}

ParseResult parse_elf(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {false, "cannot open file: " + path, {}};
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
  return parse_elf_bytes(bytes);
}

ParseResult parse_elf_bytes(const std::vector<uint8_t>& b) {
  ParseResult r;
  constexpr size_t EHDR64 = 64;   // sizeof(Elf64_Ehdr)
  constexpr size_t PHDR64 = 56;   // sizeof(Elf64_Phdr)
  if (b.size() < 4 || !(b[0] == 0x7f && b[1] == 'E' && b[2] == 'L' &&
                         b[3] == 'F')) {
    r.error = "not an ELF file (bad magic); unrecognized/proprietary formats "
              "are reported unsupported, never guessed";
    return r;
  }
  if (b.size() < EHDR64) {
    r.error = "file too small to contain an ELF header (" +
              std::to_string(b.size()) + " bytes)";
    return r;
  }
  r.info.is_elf = true;
  if (b[4] != 2) {
    r.error = "not a 64-bit ELF (EI_CLASS=" + std::to_string(b[4]) + ")";
    return r;
  }
  r.info.is_64bit = true;
  if (b[5] != 1) {
    r.error = "not a little-endian ELF (EI_DATA=" + std::to_string(b[5]) + ")";
    return r;
  }
  r.info.little_endian = true;

  r.info.elf_type = rd16(&b[16]);
  r.info.machine = rd16(&b[18]);
  r.info.entry = rd64(&b[24]);
  const uint64_t phoff = rd64(&b[32]);
  const uint16_t phentsize = rd16(&b[54]);
  const uint16_t phnum = rd16(&b[56]);

  if (phnum > 0) {
    if (phentsize < PHDR64) {
      r.error = "program header entry size " + std::to_string(phentsize) +
                " is smaller than the ELF64 minimum " +
                std::to_string(PHDR64);
      return r;
    }
    if (phoff > b.size() ||
        static_cast<uint64_t>(phnum) * phentsize > b.size() - phoff) {
      r.error = "program header table extends past end of file (corrupt or "
                "truncated)";
      return r;
    }
    for (uint16_t i = 0; i < phnum; ++i) {
      const uint8_t* p = &b[phoff + static_cast<uint64_t>(i) * phentsize];
      ElfSegment seg;
      seg.type = rd32(p + 0);
      seg.flags = rd32(p + 4);
      seg.offset = rd64(p + 8);
      seg.vaddr = rd64(p + 16);
      seg.filesz = rd64(p + 32);
      seg.memsz = rd64(p + 40);
      if (seg.filesz > 0 &&
          (seg.offset > b.size() || seg.filesz > b.size() - seg.offset)) {
        r.error = "segment " + std::to_string(i) +
                  " file range extends past end of file (corrupt)";
        return r;
      }
      r.info.segments.push_back(seg);
    }
  }
  r.ok = true;
  return r;
}

}  // namespace pnr::loader
