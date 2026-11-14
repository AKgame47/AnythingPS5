#include "tools/synth_elf.hpp"

#include <cstring>

#include "loader/nid.hpp"

namespace pnr::tools {

const char* const kSynthMessage = "hello from a NID-bound module\n";

namespace {
void p16(std::vector<std::uint8_t>& b, std::size_t o, std::uint16_t v) { b[o] = v & 0xff; b[o + 1] = v >> 8; }
void p32(std::vector<std::uint8_t>& b, std::size_t o, std::uint32_t v) { for (int i = 0; i < 4; ++i) b[o + i] = (v >> (8 * i)) & 0xff; }
void p64(std::vector<std::uint8_t>& b, std::size_t o, std::uint64_t v) { for (int i = 0; i < 8; ++i) b[o + i] = (v >> (8 * i)) & 0xff; }
}  // namespace

std::vector<std::uint8_t> build_synthetic_ps_elf(const SynthSpec& spec) {
  std::vector<std::uint8_t> b(0x340, 0);
  const std::uint8_t ident[] = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0};
  std::memcpy(b.data(), ident, 8);
  p16(b, 0x10, 3);       // ET_DYN
  p16(b, 0x12, 62);      // x86-64
  p32(b, 0x14, 1);
  p64(b, 0x18, 0x100);   // entry
  p64(b, 0x20, 0x40);    // phoff
  p16(b, 0x34, 64); p16(b, 0x36, 56); p16(b, 0x38, 2); p16(b, 0x3A, 64);
  // PT_LOAD (RWX, whole file) and PT_DYNAMIC
  const std::size_t total = b.size();
  p32(b, 0x40, 1); p32(b, 0x44, 7); p64(b, 0x48, 0); p64(b, 0x50, 0); p64(b, 0x58, 0); p64(b, 0x60, total); p64(b, 0x68, total); p64(b, 0x70, 0x1000);
  p32(b, 0x78, 2); p32(b, 0x7C, 6); p64(b, 0x80, 0x2c0); p64(b, 0x88, 0x2c0); p64(b, 0x90, 0x2c0); p64(b, 0x98, 0x80); p64(b, 0xA0, 0x80); p64(b, 0xA8, 8);

  // text at 0x100
  const std::uint8_t sub[] = {0x48, 0x83, 0xEC, 0x08};
  std::memcpy(&b[0x100], sub, 4);
  auto movrdi = [&](std::size_t at, std::size_t target) { b[at] = 0x48; b[at + 1] = 0x8B; b[at + 2] = 0x3D; p32(b, at + 3, static_cast<std::uint32_t>(target - (at + 7))); };
  auto callmem = [&](std::size_t at, std::size_t target) { b[at] = 0xFF; b[at + 1] = 0x15; p32(b, at + 2, static_cast<std::uint32_t>(target - (at + 6))); };
  movrdi(0x104, 0x160); callmem(0x10B, 0x168); movrdi(0x111, 0x160); callmem(0x118, 0x170);
  const std::uint8_t tail[] = {0x48, 0x83, 0xC4, 0x08, 0xC3};
  std::memcpy(&b[0x11E], tail, 5);
  // data
  std::memcpy(&b[0x140], kSynthMessage, std::strlen(kSynthMessage) + 1);
  // 0x160 pointer (RELATIVE), 0x168/0x170/0x178 GOT slots
  // dynsym at 0x180 (4 entries), dynstr at 0x1e0
  std::vector<std::string> names = {pnr::loader::nid::compute_nid("printf", "") + "#A#A", pnr::loader::nid::compute_nid("strlen", "") + "#A#A",
                                    pnr::loader::nid::compute_nid("sceSyntheticDoesNotExist", "") + "#A#A"};
  std::size_t so = 0x1e0 + 1;
  std::uint32_t off[3];
  const int nimp = spec.include_missing_import ? 3 : 2;
  for (int i = 0; i < nimp; ++i) { off[i] = static_cast<std::uint32_t>(so - 0x1e0); std::memcpy(&b[so], names[i].c_str(), names[i].size() + 1); so += names[i].size() + 1; }
  const std::size_t strsz = so - 0x1e0;
  for (int i = 0; i < nimp; ++i) { const std::size_t s = 0x180 + 24 * (i + 1); p32(b, s, off[i]); b[s + 4] = 0x12; /* GLOBAL FUNC */ p16(b, s + 6, 0); }
  // rela at 0x240
  std::size_t r = 0x240;
  auto rela = [&](std::uint64_t o, std::uint64_t info, std::int64_t addend) { p64(b, r, o); p64(b, r + 8, info); p64(b, r + 16, static_cast<std::uint64_t>(addend)); r += 24; };
  rela(0x160, 8, 0x140);                     // RELATIVE -> message
  rela(0x168, (1ull << 32) | 6, 0);          // GLOB_DAT printf
  rela(0x170, (2ull << 32) | 7, 0);          // JUMP_SLOT strlen
  if (spec.include_missing_import) rela(0x178, (3ull << 32) | 6, 0);
  const std::uint64_t relasz = r - 0x240;
  // dynamic at 0x2c0
  std::size_t dy = 0x2c0;
  auto dyn = [&](std::int64_t tag, std::uint64_t v) { p64(b, dy, static_cast<std::uint64_t>(tag)); p64(b, dy + 8, v); dy += 16; };
  dyn(6, 0x180); dyn(5, 0x1e0); dyn(10, strsz); dyn(11, 24); dyn(7, 0x240); dyn(8, relasz); dyn(9, 24); dyn(0, 0);
  return b;
}

}  // namespace pnr::tools
