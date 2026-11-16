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


std::vector<std::uint8_t> build_synthetic_libc_elf() {
  std::vector<std::uint8_t> b(0x740, 0);
  const std::uint8_t ident[] = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0};
  std::memcpy(b.data(), ident, 8);
  p16(b, 0x10, 3); p16(b, 0x12, 62); p32(b, 0x14, 1);
  p64(b, 0x18, 0x100); p64(b, 0x20, 0x40);
  p16(b, 0x34, 64); p16(b, 0x36, 56); p16(b, 0x38, 2); p16(b, 0x3A, 64);
  const std::size_t total = b.size();
  p32(b, 0x40, 1); p32(b, 0x44, 7); p64(b, 0x48, 0); p64(b, 0x50, 0); p64(b, 0x58, 0); p64(b, 0x60, total); p64(b, 0x68, total); p64(b, 0x70, 0x1000);
  p32(b, 0x78, 2); p32(b, 0x7C, 6); p64(b, 0x80, 0x6c0); p64(b, 0x88, 0x6c0); p64(b, 0x90, 0x6c0); p64(b, 0x98, 0x80); p64(b, 0xA0, 0x80); p64(b, 0xA8, 8);
  auto str = [&](std::size_t at, const char* s) { std::memcpy(&b[at], s, std::strlen(s) + 1); };
  const std::size_t FMT = 0x400, AB = 0x410, EXP = 0x420, HEX = 0x430, HELLO = 0x440, PF = 0x450, OK = 0x470;
  str(FMT, "%d:%s:%x"); str(AB, "ab"); str(EXP, "-5:ab:ff"); str(HEX, "0x1f"); str(HELLO, "hello"); str(PF, "C3 libc %s %d\n"); str(OK, "ok");
  enum { G_SNPRINTF, G_STRCMP, G_STRTOL, G_CALLOC, G_MEMMOVE, G_STRLEN, G_PRINTF, G_N };
  std::size_t pc = 0x100;
  std::vector<std::size_t> fails;
  auto emit = [&](std::initializer_list<int> bytes) { for (int v : bytes) b[pc++] = static_cast<std::uint8_t>(v); };
  auto imm32 = [&](std::uint32_t v) { p32(b, pc, v); pc += 4; };
  auto call = [&](int g) { emit({0xFF, 0x15}); p32(b, pc, static_cast<std::uint32_t>(0x480 + 8 * g - (pc + 4))); pc += 4; };
  auto rip = [&](std::initializer_list<int> op, std::size_t target) { emit(op); p32(b, pc, static_cast<std::uint32_t>(target - (pc + 4))); pc += 4; };
  auto check = [&](int ok_jcc, int n) { emit({ok_jcc, 10, 0xB8}); imm32(n); emit({0xE9}); fails.push_back(pc); pc += 4; };

  emit({0x53, 0x48, 0x83, 0xEC, 0x60});
  // snprintf(buf, 64, "%d:%s:%x", -5, "ab", 255)
  emit({0x48, 0x8D, 0x7C, 0x24, 0x20, 0xBE}); imm32(64);
  rip({0x48, 0x8D, 0x15}, FMT);
  emit({0x48, 0xC7, 0xC1, 0xFB, 0xFF, 0xFF, 0xFF});
  rip({0x4C, 0x8D, 0x05}, AB);
  emit({0x41, 0xB9}); imm32(255); call(G_SNPRINTF);
  emit({0x83, 0xF8, 0x08}); check(0x74, 1);
  emit({0x48, 0x8D, 0x7C, 0x24, 0x20}); rip({0x48, 0x8D, 0x35}, EXP); call(G_STRCMP);
  emit({0x85, 0xC0}); check(0x74, 2);
  // strtol("0x1f", 0, 0) == 31
  rip({0x48, 0x8D, 0x3D}, HEX); emit({0x31, 0xF6, 0x31, 0xD2}); call(G_STRTOL);
  emit({0x48, 0x83, 0xF8, 0x1F}); check(0x74, 3);
  // calloc(4, 8): non-null and zeroed
  emit({0xBF}); imm32(4); emit({0xBE}); imm32(8); call(G_CALLOC);
  emit({0x48, 0x89, 0xC3, 0x48, 0x85, 0xC0}); check(0x75, 4);
  emit({0x48, 0x83, 0x3B, 0x00}); check(0x74, 4);
  // memmove(p, "hello", 6); strlen(p) == 5
  emit({0x48, 0x89, 0xDF}); rip({0x48, 0x8D, 0x35}, HELLO); emit({0xBA}); imm32(6); call(G_MEMMOVE);
  emit({0x48, 0x89, 0xDF}); call(G_STRLEN);
  emit({0x48, 0x83, 0xF8, 0x05}); check(0x74, 5);
  // printf("C3 libc %s %d\n", "ok", 42)
  rip({0x48, 0x8D, 0x3D}, PF); rip({0x48, 0x8D, 0x35}, OK); emit({0xBA}); imm32(42); call(G_PRINTF);
  emit({0x31, 0xC0});
  const std::size_t epi = pc;
  emit({0x48, 0x83, 0xC4, 0x60, 0x5B, 0xC3});
  for (std::size_t f : fails) p32(b, f, static_cast<std::uint32_t>(epi - (f + 4)));

  const char* names[G_N] = {"snprintf", "strcmp", "strtol", "calloc", "memmove", "strlen", "printf"};
  std::size_t so = 0x580 + 1;
  for (int i = 0; i < G_N; ++i) {
    const std::string nm = pnr::loader::nid::compute_nid(names[i], "") + "#A#A";
    const std::size_t sy = 0x4c0 + 24 * (i + 1);
    p32(b, sy, static_cast<std::uint32_t>(so - 0x580)); b[sy + 4] = 0x12; p16(b, sy + 6, 0);
    std::memcpy(&b[so], nm.c_str(), nm.size() + 1); so += nm.size() + 1;
  }
  const std::size_t strsz = so - 0x580;
  for (int i = 0; i < G_N; ++i) {
    const std::size_t r = 0x600 + 24 * i;
    p64(b, r, 0x480 + 8 * i); p64(b, r + 8, (static_cast<std::uint64_t>(i + 1) << 32) | 6); p64(b, r + 16, 0);
  }
  std::size_t dy = 0x6c0;
  auto dyn = [&](std::int64_t tag, std::uint64_t v) { p64(b, dy, static_cast<std::uint64_t>(tag)); p64(b, dy + 8, v); dy += 16; };
  dyn(6, 0x4c0); dyn(5, 0x580); dyn(10, strsz); dyn(11, 24); dyn(7, 0x600); dyn(8, 24 * G_N); dyn(9, 24); dyn(0, 0);
  return b;
}

std::vector<std::uint8_t> build_synthetic_lifecycle_elf(const LifecycleSpec& spec) {
  std::vector<std::uint8_t> b(0x4c0, 0);
  const std::uint8_t ident[] = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0};
  std::memcpy(b.data(), ident, 8);
  p16(b, 0x10, 3); p16(b, 0x12, 62); p32(b, 0x14, 1);
  p64(b, 0x18, 0x100); p64(b, 0x20, 0x40);
  p16(b, 0x34, 64); p16(b, 0x36, 56); p16(b, 0x38, 2); p16(b, 0x3A, 64);
  const std::size_t total = b.size();
  p32(b, 0x40, 1); p32(b, 0x44, 7); p64(b, 0x48, 0); p64(b, 0x50, 0); p64(b, 0x58, 0); p64(b, 0x60, total); p64(b, 0x68, total); p64(b, 0x70, 0x1000);
  p32(b, 0x78, 2); p32(b, 0x7C, 6); p64(b, 0x80, 0x3c0); p64(b, 0x88, 0x3c0); p64(b, 0x90, 0x3c0); p64(b, 0x98, 0xe0); p64(b, 0xA0, 0xe0); p64(b, 0xA8, 8);

  enum { G_PUTCHAR, G_ATEXIT, G_EXIT, G_N };
  const std::size_t GOT = 0x220;
  auto callgot = [&](std::size_t pc, int g) { b[pc] = 0xFF; b[pc + 1] = 0x15; p32(b, pc + 2, static_cast<std::uint32_t>(GOT + 8 * g - (pc + 6))); };
  // One-letter lifecycle function: putchar(X) through the bound import. 14 bytes, 0x10 slots.
  auto letter_fn = [&](std::size_t at, std::uint8_t letter) {
    b[at] = 0x50;                          // push rax (aligns rsp for the call)
    b[at + 1] = 0xBF; p32(b, at + 2, letter);  // mov edi, letter
    callgot(at + 6, G_PUTCHAR);
    b[at + 12] = 0x59;                     // pop rcx
    b[at + 13] = 0xC3;                     // ret
  };
  const std::size_t F_INIT = 0x180, F_IA1 = 0x190, F_IA2 = 0x1a0, F_AH1 = 0x1b0, F_AH2 = 0x1c0,
                    F_FA1 = 0x1d0, F_FA2 = 0x1e0, F_FINI = 0x1f0;
  letter_fn(F_INIT, 'I'); letter_fn(F_IA1, 'A'); letter_fn(F_IA2, 'B');
  letter_fn(F_AH1, '1'); letter_fn(F_AH2, '2');
  letter_fn(F_FA1, 'x'); letter_fn(F_FA2, 'y'); letter_fn(F_FINI, 'F');

  // Entry at 0x100: print 'E', register AH1 then AH2 through atexit (each must return 0),
  // then return 55, or call exit(42) when spec.use_exit.
  std::size_t pc = 0x100;
  auto emit = [&](std::initializer_list<int> bytes) { for (int v : bytes) b[pc++] = static_cast<std::uint8_t>(v); };
  auto imm32 = [&](std::uint32_t v) { p32(b, pc, v); pc += 4; };
  auto lea = [&](std::size_t target) { emit({0x48, 0x8D, 0x3D}); p32(b, pc, static_cast<std::uint32_t>(target - (pc + 4))); pc += 4; };
  std::vector<std::size_t> fails;
  emit({0x53});                            // push rbx
  emit({0xBF}); imm32('E');                // mov edi, 'E'
  callgot(pc, G_PUTCHAR); pc += 6;
  lea(F_AH1); callgot(pc, G_ATEXIT); pc += 6;
  emit({0x85, 0xC0, 0x75, 0x00}); fails.push_back(pc - 1);   // test eax,eax; jnz fail1
  lea(F_AH2); callgot(pc, G_ATEXIT); pc += 6;
  emit({0x85, 0xC0, 0x75, 0x00}); fails.push_back(pc - 1);   // test eax,eax; jnz fail2
  if (spec.use_exit) {
    emit({0xBF}); imm32(kLifecycleExitExit);   // mov edi, 42
    callgot(pc, G_EXIT); pc += 6;              // exit(42): noreturn
    emit({0x0F, 0x0B});                        // ud2 (unreachable)
  } else {
    emit({0xB8}); imm32(kLifecycleExitReturn); // mov eax, 55
    emit({0x5B, 0xC3});                        // pop rbx; ret
  }
  const std::size_t fail1 = pc;
  emit({0xB8}); imm32(61); emit({0x5B, 0xC3});
  const std::size_t fail2 = pc;
  emit({0xB8}); imm32(62); emit({0x5B, 0xC3});
  b[fails[0]] = static_cast<std::uint8_t>(fail1 - (fails[0] + 1));  // jnz rel8, from the next insn
  b[fails[1]] = static_cast<std::uint8_t>(fail2 - (fails[1] + 1));

  // init/fini array DATA: file holds vaddrs, RELATIVE relocs below make them absolute.
  p64(b, 0x200, F_IA1); p64(b, 0x208, F_IA2);
  p64(b, 0x210, F_FA1); p64(b, 0x218, F_FA2);

  // dynamic symbols: 3 imports
  const char* names[G_N] = {"putchar", "atexit", "exit"};
  std::size_t so = 0x380 + 1;
  for (int i = 0; i < G_N; ++i) {
    const std::string nm = pnr::loader::nid::compute_nid(names[i], "") + "#A#A";
    const std::size_t sy = 0x300 + 24 * (i + 1);
    p32(b, sy, static_cast<std::uint32_t>(so - 0x380)); b[sy + 4] = 0x12; p16(b, sy + 6, 0);
    std::memcpy(&b[so], nm.c_str(), nm.size() + 1); so += nm.size() + 1;
  }
  const std::size_t strsz = so - 0x380;
  // relocations at 0x240: 4 RELATIVE (array contents) + 3 GLOB_DAT (GOT)
  std::size_t r = 0x240;
  auto rela = [&](std::uint64_t o, std::uint64_t info, std::int64_t addend) { p64(b, r, o); p64(b, r + 8, info); p64(b, r + 16, static_cast<std::uint64_t>(addend)); r += 24; };
  rela(0x200, 8, F_IA1); rela(0x208, 8, F_IA2); rela(0x210, 8, F_FA1); rela(0x218, 8, F_FA2);
  for (int i = 0; i < G_N; ++i) rela(GOT + 8 * i, (static_cast<std::uint64_t>(i + 1) << 32) | 6, 0);
  const std::uint64_t relasz = r - 0x240;
  // dynamic at 0x3c0 (14 entries)
  std::size_t dy = 0x3c0;
  auto dyn = [&](std::int64_t tag, std::uint64_t v) { p64(b, dy, static_cast<std::uint64_t>(tag)); p64(b, dy + 8, v); dy += 16; };
  dyn(6, 0x300); dyn(5, 0x380); dyn(10, strsz); dyn(11, 24); dyn(7, 0x240); dyn(8, relasz); dyn(9, 24);
  dyn(12, F_INIT); dyn(25, 0x200); dyn(27, 16); dyn(26, 0x210); dyn(28, 16); dyn(13, F_FINI); dyn(0, 0);
  return b;
}


std::vector<std::uint8_t> build_synthetic_dep_elf(const DepSpec& spec) {
  std::vector<std::uint8_t> b(0x500, 0);
  const std::uint8_t ident[] = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0};
  std::memcpy(b.data(), ident, 8);
  p16(b, 0x10, 3); p16(b, 0x12, 62); p32(b, 0x14, 1);
  p64(b, 0x18, 0x140); p64(b, 0x20, 0x40);  // entry (unused for a dependency) at a ret
  p16(b, 0x34, 64); p16(b, 0x36, 56); p16(b, 0x38, spec.with_tls ? 3 : 2); p16(b, 0x3A, 64);
  const std::size_t total = b.size();
  p32(b, 0x40, 1); p32(b, 0x44, 7); p64(b, 0x48, 0); p64(b, 0x50, 0); p64(b, 0x58, 0); p64(b, 0x60, total); p64(b, 0x68, total); p64(b, 0x70, 0x1000);
  p32(b, 0x78, 2); p32(b, 0x7C, 6); p64(b, 0x80, 0x400); p64(b, 0x88, 0x400); p64(b, 0x90, 0x400); p64(b, 0x98, 0x100); p64(b, 0xA0, 0x100); p64(b, 0xA8, 8);
  if (spec.with_tls) {  // PT_TLS at 0x1c0: 8 file bytes, 16 bss
    p32(b, 0xB0, 7); p32(b, 0xB4, 4); p64(b, 0xB8, 0x1c0); p64(b, 0xC0, 0x1c0); p64(b, 0xC8, 0x1c0); p64(b, 0xD0, 8); p64(b, 0xD8, 16); p64(b, 0xE0, 8);
  }

  const std::size_t GOT = 0x220;
  auto callgot = [&](std::size_t pc, int g) { b[pc] = 0xFF; b[pc + 1] = 0x15; p32(b, pc + 2, static_cast<std::uint32_t>(GOT + 8 * g - (pc + 6))); };
  // Exported function at 0x100: returns `value`, or import_fn() + value when import_fn is set.
  std::size_t pc = 0x100;
  if (spec.import_fn.empty()) {
    b[pc] = 0xB8; p32(b, pc + 1, static_cast<std::uint32_t>(spec.value));  // mov eax, value
    b[pc + 5] = 0xC3;                                                      // ret
  } else {
    b[pc] = 0x50;                                                          // push rax (align)
    callgot(pc + 1, 1);                                                    // call [GOT import]
    b[pc + 7] = 0x05; p32(b, pc + 8, static_cast<std::uint32_t>(spec.value));  // add eax, value
    b[pc + 12] = 0x59;                                                     // pop rcx
    b[pc + 13] = 0xC3;                                                     // ret
  }
  b[0x140] = 0xC3;  // entry: ret
  // Letter functions: putchar(X) through GOT[0].
  auto letter_fn = [&](std::size_t at, std::uint8_t letter) {
    b[at] = 0x50;
    b[at + 1] = 0xBF; p32(b, at + 2, letter);
    callgot(at + 6, 0);
    b[at + 12] = 0x59;
    b[at + 13] = 0xC3;
  };
  const std::size_t F_INIT = 0x180, F_IA = 0x190, F_FA = 0x1a0, F_FINI = 0x1b0;
  letter_fn(F_INIT, static_cast<std::uint8_t>(spec.letters[0]));
  letter_fn(F_IA, static_cast<std::uint8_t>(spec.letters[1]));
  letter_fn(F_FA, static_cast<std::uint8_t>(spec.letters[2]));
  letter_fn(F_FINI, static_cast<std::uint8_t>(spec.letters[3]));

  p64(b, 0x200, F_IA);   // init array data (RELATIVE below)
  p64(b, 0x208, F_FA);   // fini array data
  p32(b, 0x210, static_cast<std::uint32_t>(2 * spec.value));  // dep_counter

  // dynsym at 0x300: 1 putchar import, [2 import_fn], then export_fn + dep_counter (defined).
  const bool imp = !spec.import_fn.empty();
  const std::string nid_putchar = pnr::loader::nid::compute_nid("putchar", "") + "#A#A";
  const std::string nid_export = pnr::loader::nid::compute_nid(spec.export_fn, "") + "#A#A";
  const std::string nid_counter = pnr::loader::nid::compute_nid("dep_counter", "") + "#A#A";
  const std::string nid_import = imp ? pnr::loader::nid::compute_nid(spec.import_fn, "") + "#A#A" : "";
  std::size_t so = 0x380 + 1;
  auto name_off = [&](const std::string& s) { const std::uint32_t o = static_cast<std::uint32_t>(so - 0x380); std::memcpy(&b[so], s.c_str(), s.size() + 1); so += s.size() + 1; return o; };
  auto sym = [&](int i, std::uint32_t nm, std::uint8_t info, std::uint16_t shndx, std::uint64_t value) {
    const std::size_t s = 0x300 + 24 * i;
    p32(b, s, nm); b[s + 4] = info; p16(b, s + 6, shndx); p64(b, s + 8, value);
  };
  sym(1, name_off(nid_putchar), 0x12, 0, 0);
  if (imp) sym(2, name_off(nid_import), 0x12, 0, 0);
  sym(imp ? 3 : 2, name_off(nid_export), 0x12, 1, 0x100);
  sym(imp ? 4 : 3, name_off(nid_counter), 0x11, 1, 0x210);
  const std::uint32_t soname_off = name_off(spec.soname);
  std::uint32_t needed_off = 0;
  if (!spec.needed.empty()) needed_off = name_off(spec.needed);
  const std::size_t strsz = so - 0x380;

  // relocations at 0x240
  std::size_t r = 0x240;
  auto rela = [&](std::uint64_t o, std::uint64_t info, std::int64_t addend) { p64(b, r, o); p64(b, r + 8, info); p64(b, r + 16, static_cast<std::uint64_t>(addend)); r += 24; };
  rela(0x200, 8, F_IA); rela(0x208, 8, F_FA);
  rela(GOT, (1ull << 32) | 6, 0);                       // putchar
  if (imp) rela(GOT + 8, (2ull << 32) | 6, 0);          // import_fn
  const std::uint64_t relasz = r - 0x240;

  // dynamic at 0x400
  std::size_t dy = 0x400;
  auto dyn = [&](std::int64_t tag, std::uint64_t v) { p64(b, dy, static_cast<std::uint64_t>(tag)); p64(b, dy + 8, v); dy += 16; };
  dyn(6, 0x300); dyn(5, 0x380); dyn(10, strsz); dyn(11, 24); dyn(7, 0x240); dyn(8, relasz); dyn(9, 24);
  dyn(14, soname_off);
  if (!spec.needed.empty()) dyn(1, needed_off);
  dyn(12, F_INIT); dyn(25, 0x200); dyn(27, 8); dyn(26, 0x208); dyn(28, 8); dyn(13, F_FINI); dyn(0, 0);
  return b;
}

std::vector<std::uint8_t> build_synthetic_depmain_elf(const DepMainSpec& spec) {
  std::vector<std::uint8_t> b(0x500, 0);
  const std::uint8_t ident[] = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0};
  std::memcpy(b.data(), ident, 8);
  p16(b, 0x10, 3); p16(b, 0x12, 62); p32(b, 0x14, 1);
  p64(b, 0x18, 0x100); p64(b, 0x20, 0x40);
  p16(b, 0x34, 64); p16(b, 0x36, 56); p16(b, 0x38, 2); p16(b, 0x3A, 64);
  const std::size_t total = b.size();
  p32(b, 0x40, 1); p32(b, 0x44, 7); p64(b, 0x48, 0); p64(b, 0x50, 0); p64(b, 0x58, 0); p64(b, 0x60, total); p64(b, 0x68, total); p64(b, 0x70, 0x1000);
  p32(b, 0x78, 2); p32(b, 0x7C, 6); p64(b, 0x80, 0x400); p64(b, 0x88, 0x400); p64(b, 0x90, 0x400); p64(b, 0x98, 0x100); p64(b, 0xA0, 0x100); p64(b, 0xA8, 8);

  enum { G_PUTCHAR, G_ATEXIT, G_FN, G_COUNTER, G_N };
  const std::size_t GOT = 0x220;
  auto callgot = [&](std::size_t pc, int g) { b[pc] = 0xFF; b[pc + 1] = 0x15; p32(b, pc + 2, static_cast<std::uint32_t>(GOT + 8 * g - (pc + 6))); };
  auto letter_fn = [&](std::size_t at, std::uint8_t letter) {
    b[at] = 0x50;
    b[at + 1] = 0xBF; p32(b, at + 2, letter);
    callgot(at + 6, G_PUTCHAR);
    b[at + 12] = 0x59;
    b[at + 13] = 0xC3;
  };
  const std::size_t F_INIT = 0x180, F_IA1 = 0x190, F_IA2 = 0x1a0, F_AH1 = 0x1b0, F_AH2 = 0x1c0, F_FA1 = 0x1d0, F_FA2 = 0x1e0, F_FINI = 0x1f0;
  letter_fn(F_INIT, 'I'); letter_fn(F_IA1, 'A'); letter_fn(F_IA2, 'B');
  letter_fn(F_AH1, '1'); letter_fn(F_AH2, '2');
  letter_fn(F_FA1, 'x'); letter_fn(F_FA2, 'y'); letter_fn(F_FINI, 'F');

  // Entry: print 'E', check import_fn() == expect_fn and *dep_counter == expect_data,
  // register both atexit handlers (must return 0), return 55. Failures exit 61..64.
  std::size_t pc = 0x100;
  auto emit = [&](std::initializer_list<int> bytes) { for (int v : bytes) b[pc++] = static_cast<std::uint8_t>(v); };
  auto imm32 = [&](std::uint32_t v) { p32(b, pc, v); pc += 4; };
  auto lea = [&](std::size_t target) { emit({0x48, 0x8D, 0x3D}); p32(b, pc, static_cast<std::uint32_t>(target - (pc + 4))); pc += 4; };
  std::vector<std::pair<std::size_t, std::size_t>> jcc;  // (rel32 field pos, fail target index)
  emit({0x53});                            // push rbx
  emit({0xBF}); imm32('E');                // mov edi, 'E'
  callgot(pc, G_PUTCHAR); pc += 6;
  callgot(pc, G_FN); pc += 6;              // eax = import_fn()
  emit({0x3D}); imm32(static_cast<std::uint32_t>(spec.expect_fn));  // cmp eax, expect_fn
  emit({0x0F, 0x85}); jcc.push_back({pc, 0}); pc += 4;              // jne fail1
  emit({0x48, 0x8B, 0x05}); p32(b, pc, static_cast<std::uint32_t>(GOT + 8 * G_COUNTER - (pc + 4))); pc += 4;  // mov rax, [GOT counter]
  emit({0x81, 0x38}); imm32(static_cast<std::uint32_t>(spec.expect_data));  // cmp dword [rax], expect_data
  emit({0x0F, 0x85}); jcc.push_back({pc, 1}); pc += 4;              // jne fail2
  lea(F_AH1); callgot(pc, G_ATEXIT); pc += 6;
  emit({0x85, 0xC0, 0x0F, 0x85}); jcc.push_back({pc, 2}); pc += 4;  // test eax,eax; jnz fail3
  lea(F_AH2); callgot(pc, G_ATEXIT); pc += 6;
  emit({0x85, 0xC0, 0x0F, 0x85}); jcc.push_back({pc, 3}); pc += 4;  // test eax,eax; jnz fail4
  emit({0xB8}); imm32(kDepMainExitReturn); // mov eax, 55
  emit({0x5B, 0xC3});                      // pop rbx; ret
  for (int i = 0; i < 4; ++i) {
    const std::size_t fail = pc;
    emit({0xB8}); imm32(static_cast<std::uint32_t>(61 + i));
    emit({0x5B, 0xC3});
    p32(b, jcc[i].first, static_cast<std::uint32_t>(fail - (jcc[i].first + 4)));
  }

  // init/fini array data (RELATIVE below)
  p64(b, 0x200, F_IA1); p64(b, 0x208, F_IA2);
  p64(b, 0x210, F_FA1); p64(b, 0x218, F_FA2);

  // dynsym at 0x300: putchar, atexit, import_fn, dep_counter - all imports.
  const char* plain[G_N] = {"putchar", "atexit", spec.import_fn.c_str(), "dep_counter"};
  std::size_t so = 0x380 + 1;
  std::uint32_t name_offs[G_N];
  for (int i = 0; i < G_N; ++i) {
    const std::string nm = pnr::loader::nid::compute_nid(plain[i], "") + "#A#A";
    name_offs[i] = static_cast<std::uint32_t>(so - 0x380);
    std::memcpy(&b[so], nm.c_str(), nm.size() + 1); so += nm.size() + 1;
  }
  const std::uint32_t needed_off = static_cast<std::uint32_t>(so - 0x380);
  std::memcpy(&b[so], spec.needed.c_str(), spec.needed.size() + 1); so += spec.needed.size() + 1;
  const std::size_t strsz = so - 0x380;
  for (int i = 0; i < G_N; ++i) {
    const std::size_t s = 0x300 + 24 * (i + 1);
    p32(b, s, name_offs[i]); b[s + 4] = (i == G_COUNTER) ? 0x11 : 0x12; p16(b, s + 6, 0);
  }
  // relocations at 0x240: 4 RELATIVE (arrays) + 4 GLOB_DAT (GOT)
  std::size_t r = 0x240;
  auto rela = [&](std::uint64_t o, std::uint64_t info, std::int64_t addend) { p64(b, r, o); p64(b, r + 8, info); p64(b, r + 16, static_cast<std::uint64_t>(addend)); r += 24; };
  rela(0x200, 8, F_IA1); rela(0x208, 8, F_IA2); rela(0x210, 8, F_FA1); rela(0x218, 8, F_FA2);
  for (int i = 0; i < G_N; ++i) rela(GOT + 8 * i, (static_cast<std::uint64_t>(i + 1) << 32) | 6, 0);
  const std::uint64_t relasz = r - 0x240;
  // dynamic at 0x400
  std::size_t dy = 0x400;
  auto dyn = [&](std::int64_t tag, std::uint64_t v) { p64(b, dy, static_cast<std::uint64_t>(tag)); p64(b, dy + 8, v); dy += 16; };
  dyn(6, 0x300); dyn(5, 0x380); dyn(10, strsz); dyn(11, 24); dyn(7, 0x240); dyn(8, relasz); dyn(9, 24);
  dyn(1, needed_off);
  dyn(12, F_INIT); dyn(25, 0x200); dyn(27, 16); dyn(26, 0x210); dyn(28, 16); dyn(13, F_FINI); dyn(0, 0);
  return b;
}

}  // namespace pnr::tools
