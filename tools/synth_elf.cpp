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


std::vector<std::uint8_t> build_synthetic_kernel_elf(const KernelSpec& spec) {
  std::vector<std::uint8_t> b(0x760, 0);
  const std::uint8_t ident[] = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0};
  std::memcpy(b.data(), ident, 8);
  p16(b, 0x10, 3); p16(b, 0x12, 62); p32(b, 0x14, 1);
  p64(b, 0x18, 0x100); p64(b, 0x20, 0x40);
  p16(b, 0x34, 64); p16(b, 0x36, 56); p16(b, 0x38, 3); p16(b, 0x3A, 64);
  const std::size_t total = b.size();
  p32(b, 0x40, 1); p32(b, 0x44, 7); p64(b, 0x48, 0); p64(b, 0x50, 0); p64(b, 0x58, 0); p64(b, 0x60, total); p64(b, 0x68, total); p64(b, 0x70, 0x1000);
  p32(b, 0x78, 2); p32(b, 0x7C, 6); p64(b, 0x80, 0x6c0); p64(b, 0x88, 0x6c0); p64(b, 0x90, 0x6c0); p64(b, 0x98, 0x80); p64(b, 0xA0, 0x80); p64(b, 0xA8, 8);
  // PT_TLS (type 7) or a harmless PT_NOTE (type 4) when breaking TLS
  p32(b, 0xB0, spec.break_tls ? 4 : 7); p32(b, 0xB4, 4); p64(b, 0xB8, 0x400); p64(b, 0xC0, 0x400); p64(b, 0xC8, 0x400); p64(b, 0xD0, 8); p64(b, 0xD8, 16); p64(b, 0xE0, 16);
  p64(b, 0x400, 0x1122334455667788ull);

  // GOT slots at 0x420 + 8*i
  enum { G_MMAP, G_CLOCK, G_MINIT, G_MLOCK, G_MUNLOCK, G_MDESTROY, G_TCREATE, G_TJOIN, G_N };
  std::size_t pc = 0x100;
  std::vector<std::size_t> fails;  // positions of rel32 to patch -> epilogue
  auto emit = [&](std::initializer_list<int> bytes) { for (int v : bytes) b[pc++] = static_cast<std::uint8_t>(v); };
  auto imm32 = [&](std::uint32_t v) { p32(b, pc, v); pc += 4; };
  auto imm64 = [&](std::uint64_t v) { p64(b, pc, v); pc += 8; };
  auto call = [&](int g) { emit({0xFF, 0x15}); p32(b, pc, static_cast<std::uint32_t>(0x420 + 8 * g - (pc + 4))); pc += 4; };
  // after flags are set: jump over a "mov eax,N; jmp epilogue" when `ok_jcc` holds
  auto check = [&](int ok_jcc, int n) { emit({ok_jcc, 10, 0xB8}); imm32(n); emit({0xE9}); fails.push_back(pc); pc += 4; };

  emit({0x53, 0x41, 0x54, 0x41, 0x55});            // push rbx; push r12; push r13
  emit({0x48, 0x83, 0xEC, 0x40});                  // sub rsp,0x40
  emit({0x48, 0x89, 0xFB});                        // mov rbx,rdi
  emit({0x48, 0x83, 0x3B, 0x01}); check(0x74, 1);  // argc == 1
  emit({0x64, 0x48, 0x8B, 0x04, 0x25, 0, 0, 0, 0});  // mov rax, fs:[0]
  emit({0x48, 0x8B, 0x08, 0x48, 0x39, 0xC1}); check(0x74, 2);  // [rax] == rax
  emit({0x64, 0x48, 0x8B, 0x04, 0x25, 0xF0, 0xFF, 0xFF, 0xFF});  // mov rax, fs:[-16]
  emit({0x48, 0xB9}); imm64(0x1122334455667788ull); emit({0x48, 0x39, 0xC8}); check(0x74, 3);
  emit({0x64, 0x48, 0x8B, 0x04, 0x25, 0xF8, 0xFF, 0xFF, 0xFF, 0x48, 0x85, 0xC0}); check(0x74, 4);  // fs:[-8] == 0
  emit({0x64, 0x48, 0xC7, 0x04, 0x25, 0xF8, 0xFF, 0xFF, 0xFF, 7, 0, 0, 0});  // mov qword fs:[-8],7
  emit({0x64, 0x48, 0x8B, 0x04, 0x25, 0xF8, 0xFF, 0xFF, 0xFF, 0x48, 0x83, 0xF8, 0x07}); check(0x74, 5);
  // mmap(0, 4096, CPU_READ|CPU_WRITE, ANON|PRIVATE, -1, 0)
  emit({0x31, 0xFF, 0xBE}); imm32(4096); emit({0xBA}); imm32(3); emit({0xB9}); imm32(0x1002);
  emit({0x49, 0xC7, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF, 0x45, 0x31, 0xC9}); call(G_MMAP);
  emit({0x49, 0x89, 0xC4, 0x48, 0x83, 0xF8, 0xFF}); check(0x75, 6);   // r12 = rax; rax != -1
  emit({0x49, 0xC7, 0x04, 0x24, 0x55, 0, 0, 0, 0x49, 0x8B, 0x04, 0x24, 0x48, 0x83, 0xF8, 0x55}); check(0x74, 7);
  // clock_gettime(MONOTONIC=4, &ts at rsp+0x20)
  emit({0xBF}); imm32(4); emit({0x48, 0x8D, 0x74, 0x24, 0x20}); call(G_CLOCK);
  emit({0x85, 0xC0}); check(0x74, 8);
  emit({0x48, 0x8B, 0x44, 0x24, 0x20, 0x48, 0x0B, 0x44, 0x24, 0x28}); check(0x75, 8);
  // mutex handle at [rsp+0x10]
  emit({0x48, 0x8D, 0x7C, 0x24, 0x10, 0x31, 0xF6, 0x31, 0xD2}); call(G_MINIT);   emit({0x85, 0xC0}); check(0x74, 9);
  emit({0x48, 0x8D, 0x7C, 0x24, 0x10}); call(G_MLOCK);    emit({0x85, 0xC0}); check(0x74, 10);
  emit({0x48, 0x8D, 0x7C, 0x24, 0x10}); call(G_MUNLOCK);  emit({0x85, 0xC0}); check(0x74, 11);
  emit({0x48, 0x8D, 0x7C, 0x24, 0x10}); call(G_MDESTROY); emit({0x85, 0xC0}); check(0x74, 12);
  // thread: result cell at [rsp+8], thread handle at [rsp+0x18], thread fn address in rdx
  emit({0x48, 0xC7, 0x44, 0x24, 0x08, 0, 0, 0, 0});
  emit({0x48, 0x8D, 0x7C, 0x24, 0x18, 0x31, 0xF6, 0x48, 0x8D, 0x15});
  const std::size_t thr_disp = pc; pc += 4;  // patched below
  emit({0x48, 0x8D, 0x4C, 0x24, 0x08, 0x45, 0x31, 0xC0}); call(G_TCREATE);
  emit({0x85, 0xC0}); check(0x74, 13);
  emit({0x48, 0x8B, 0x7C, 0x24, 0x18, 0x48, 0x8D, 0x74, 0x24, 0x30}); call(G_TJOIN);
  emit({0x48, 0x8B, 0x44, 0x24, 0x08, 0x48, 0x83, 0xF8, 0x42}); check(0x74, 14);
  emit({0x31, 0xC0});  // success
  const std::size_t epi = pc;
  emit({0x48, 0x83, 0xC4, 0x40, 0x41, 0x5D, 0x41, 0x5C, 0x5B, 0xC3});
  for (std::size_t f : fails) p32(b, f, static_cast<std::uint32_t>(epi - (f + 4)));
  // thread function: store 0x42 into *rdi only when its own TCB self pointer is intact
  const std::size_t thr = pc;
  p32(b, thr_disp, static_cast<std::uint32_t>(thr - (thr_disp + 4)));
  emit({0x64, 0x48, 0x8B, 0x04, 0x25, 0, 0, 0, 0, 0x48, 0x8B, 0x08, 0x48, 0x39, 0xC1, 0x75, 0x09});
  emit({0x48, 0xC7, 0x07, 0x42, 0, 0, 0, 0xEB, 0x07, 0x48, 0xC7, 0x07, 0x99, 0, 0, 0, 0x31, 0xC0, 0xC3});

  // dynamic symbols: 8 imports
  const char* names[G_N] = {"mmap", "sceKernelClockGettime", "scePthreadMutexInit", "scePthreadMutexLock", "scePthreadMutexUnlock",
                            "scePthreadMutexDestroy", "scePthreadCreate", "scePthreadJoin"};
  std::size_t so = 0x560 + 1;
  for (int i = 0; i < G_N; ++i) {
    const std::string nm = pnr::loader::nid::compute_nid(names[i], "") + "#A#A";
    const std::size_t sy = 0x480 + 24 * (i + 1);
    p32(b, sy, static_cast<std::uint32_t>(so - 0x560)); b[sy + 4] = 0x12; p16(b, sy + 6, 0);
    std::memcpy(&b[so], nm.c_str(), nm.size() + 1); so += nm.size() + 1;
  }
  const std::size_t strsz = so - 0x560;
  for (int i = 0; i < G_N; ++i) {
    const std::size_t r = 0x600 + 24 * i;
    p64(b, r, 0x420 + 8 * i); p64(b, r + 8, (static_cast<std::uint64_t>(i + 1) << 32) | 6); p64(b, r + 16, 0);
  }
  std::size_t dy = 0x6c0;
  auto dyn = [&](std::int64_t tag, std::uint64_t v) { p64(b, dy, static_cast<std::uint64_t>(tag)); p64(b, dy + 8, v); dy += 16; };
  dyn(6, 0x480); dyn(5, 0x560); dyn(10, strsz); dyn(11, 24); dyn(7, 0x600); dyn(8, 24 * G_N); dyn(9, 24); dyn(0, 0);
  return b;
}

}  // namespace pnr::tools
