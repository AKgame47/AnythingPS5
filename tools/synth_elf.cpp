#include "tools/synth_elf.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "loader/nid.hpp"

namespace pnr::tools {

const char* const kSynthMessage = "hello from a NID-bound module\n";

namespace {
void p16(std::vector<std::uint8_t>& b, std::size_t o, std::uint16_t v) { b[o] = v & 0xff; b[o + 1] = v >> 8; }
void p32(std::vector<std::uint8_t>& b, std::size_t o, std::uint32_t v) { for (int i = 0; i < 4; ++i) b[o + i] = (v >> (8 * i)) & 0xff; }
void p64(std::vector<std::uint8_t>& b, std::size_t o, std::uint64_t v) { for (int i = 0; i < 8; ++i) b[o + i] = (v >> (8 * i)) & 0xff; }
}  // namespace

// ---- Slice S34: C++ ABI module ----------------------------------------------------
// Layout: helper code after the program headers (0xb0 comparator, 0xb8/0xd8 dtors),
// entry 0x100, ptr slots 0x200/0x208 (RELATIVE), GOT 0x210 (6), guard byte 0x240,
// int[4] {5,3,8,1} at 0x248, RELA 0x260 (8 entries), dynsym 0x320 (7), dynstr 0x3c8,
// dynamic at 0x440 (8 entries). Failures exit 61 (guard/registration) or 62 (qsort result).
std::vector<std::uint8_t> build_synthetic_cxx_elf() {
  std::vector<std::uint8_t> b(0x560, 0);
  const std::uint8_t ident[] = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0};
  std::memcpy(b.data(), ident, 8);
  p16(b, 0x10, 3);       // ET_DYN
  p16(b, 0x12, 62);      // x86-64
  p32(b, 0x14, 1);
  p64(b, 0x18, 0x100);   // entry
  p64(b, 0x20, 0x40);    // phoff
  p16(b, 0x34, 64); p16(b, 0x36, 56); p16(b, 0x38, 2); p16(b, 0x3A, 64);
  const std::size_t total = b.size();
  p32(b, 0x40, 1); p32(b, 0x44, 7); p64(b, 0x48, 0); p64(b, 0x50, 0); p64(b, 0x58, 0); p64(b, 0x60, total); p64(b, 0x68, total); p64(b, 0x70, 0x1000);
  p32(b, 0x78, 2); p32(b, 0x7C, 6); p64(b, 0x80, 0x440); p64(b, 0x88, 0x440); p64(b, 0x90, 0x440); p64(b, 0x98, 0x80); p64(b, 0xA0, 0x80); p64(b, 0xA8, 8);

  constexpr std::uint64_t G_PUTCHAR = 0x210, G_CXA_ATEXIT = 0x218, G_TLS_ATEXIT = 0x220;
  constexpr std::uint64_t G_QSORT = 0x228, G_GUARD_ACQ = 0x230, G_GUARD_REL = 0x238;
  std::size_t at = 0xb0;
  auto ins = [&](std::initializer_list<int> bs) { for (int x : bs) b[at++] = static_cast<std::uint8_t>(x); };
  auto d32 = [&](std::int64_t disp) { p32(b, at, static_cast<std::uint32_t>(disp)); at += 4; };
  auto callgot = [&](std::uint64_t got) { ins({0xff, 0x15}); d32(static_cast<std::int64_t>(got) - static_cast<std::int64_t>(at + 4)); };
  // dtor_t at 0xb8: records the low byte of its argument at 0x259, then prints it ('Q' when
  // arg==0x51). dtor_c (at 0x500) later checks that record, proving thread dtors ran first.
  at = 0xb8;
  ins({0x50});                    // push rax (call alignment)
  ins({0x40, 0x88, 0x3d}); d32(0x259 - (at + 4));  // mov [rip->0x259], dil (REX for dil)
  callgot(G_PUTCHAR);
  ins({0x59, 0xc3});              // pop rcx; ret
  // NOTE: the comparator lives at 0x4c0 and dtor_c at 0x500, past PT_DYNAMIC; both are still
  // inside PT_LOAD (filesz/memsz cover the whole file). The 0xd8..0x100 region is padding.
  // entry, assembled with labels so every RIP-relative displacement is computed, never typed.
  {
    std::vector<std::uint8_t> code;
    auto e8 = [&](int x) { code.push_back(static_cast<std::uint8_t>(x)); };
    auto e32 = [&](std::uint32_t v) { for (int i = 0; i < 4; ++i) e8((v >> (8 * i)) & 0xff); };
    struct Patch { std::size_t at; std::uint64_t target; std::size_t next; };
    std::vector<Patch> patches;
    auto rel32 = [&](std::uint64_t target) { patches.push_back({code.size(), target, code.size() + 4}); e32(0); };
    auto cg = [&](std::uint64_t got) { e8(0xff); e8(0x15); rel32(got); };          // call [rip+rel32]
    auto movrdi = [&](std::uint64_t slot) { e8(0x48); e8(0x8b); e8(0x3d); rel32(slot); };
    auto movrsi = [&](std::uint64_t slot) { e8(0x48); e8(0x8b); e8(0x35); rel32(slot); };
    auto leardi = [&](std::uint64_t t) { e8(0x48); e8(0x8d); e8(0x3d); rel32(t); };
    auto learcx = [&](std::uint64_t t) { e8(0x48); e8(0x8d); e8(0x0d); rel32(t); };
    std::vector<std::size_t> fail_a;  // patch indices jumping to the guard/registration stub
    std::vector<std::size_t> fail_b;  // patch indices jumping to the qsort-result stub
    auto jcc = [&](std::vector<std::size_t>& which) { e8(0x0f); e8(0x85); which.push_back(patches.size()); patches.push_back({code.size(), 0, code.size() + 4}); e32(0); };
    for (int x : {0x53, 0xbf, 0x45, 0x00, 0x00, 0x00}) e8(x);   // push rbx; mov edi,'E'
    cg(G_PUTCHAR);
    movrdi(0x200); cg(G_GUARD_ACQ);                             // acquire must take it
    for (int x : {0x83, 0xf8, 0x01}) e8(x);                     // cmp eax,1
    jcc(fail_a);
    movrdi(0x200); cg(G_GUARD_REL);                             // release marks it initialized
    e8(0x85); e8(0xc0);                                         // test eax,eax
    jcc(fail_a);
    movrdi(0x200); cg(G_GUARD_ACQ);                             // acquire now declines
    e8(0x85); e8(0xc0);
    jcc(fail_a);
    for (int x : {0xbf, 0x47, 0x00, 0x00, 0x00}) e8(x);         // mov edi,'G'
    cg(G_PUTCHAR);
    movrdi(0x208);
    for (int x : {0xbe, 0x04, 0x00, 0x00, 0x00}) e8(x);         // mov esi,4
    for (int x : {0xba, 0x04, 0x00, 0x00, 0x00}) e8(x);         // mov edx,4
    learcx(0x4c0);                                              // comparator
    cg(G_QSORT);
    movrsi(0x208);
    for (int x : {0x83, 0x3e, 0x08}) e8(x);                     // cmp [rsi],8
    jcc(fail_b);
    for (int x : {0x83, 0x7e, 0x04, 0x05}) e8(x);               // cmp [rsi+4],5
    jcc(fail_b);
    for (int x : {0x83, 0x7e, 0x08, 0x03}) e8(x);               // cmp [rsi+8],3
    jcc(fail_b);
    for (int x : {0x83, 0x7e, 0x0c, 0x01}) e8(x);               // cmp [rsi+12],1
    jcc(fail_b);
    for (int x : {0xbf, 0x51, 0x00, 0x00, 0x00}) e8(x);         // mov edi,'Q'
    cg(G_PUTCHAR);
    leardi(0xb8);                                               // dtor_t
    for (int x : {0xbe, 0x51, 0x00, 0x00, 0x00}) e8(x);         // mov esi,0x51
    e8(0x31); e8(0xd2);                                         // xor edx,edx
    cg(G_TLS_ATEXIT);
    e8(0x85); e8(0xc0);
    jcc(fail_a);
    leardi(0x500);                                              // dtor_c
    for (int x : {0xbe, 0x42, 0x00, 0x00, 0x00}) e8(x);         // mov esi,0x42
    e8(0x31); e8(0xd2);
    cg(G_CXA_ATEXIT);
    e8(0x85); e8(0xc0);
    jcc(fail_a);
    for (int x : {0xb8, 0x37, 0x00, 0x00, 0x00, 0x5b, 0xc3}) e8(x);  // mov eax,55; pop rbx; ret
    const std::size_t stub_a = code.size();
    for (int x : {0xb8, 61, 0x00, 0x00, 0x00, 0x5b, 0xc3}) e8(x);
    const std::size_t stub_b = code.size();
    for (int x : {0xb8, 62, 0x00, 0x00, 0x00, 0x5b, 0xc3}) e8(x);
    for (const std::size_t i : fail_a) patches[i].target = 0x100 + stub_a;
    for (const std::size_t i : fail_b) patches[i].target = 0x100 + stub_b;
    for (const Patch& p : patches) {
      const std::int64_t disp = static_cast<std::int64_t>(p.target) - static_cast<std::int64_t>(0x100 + p.next);
      for (int i = 0; i < 4; ++i) code[p.at + i] = static_cast<std::uint8_t>((disp >> (8 * i)) & 0xff);
    }
    if (0x100 + code.size() > 0x200) { std::fprintf(stderr, "cxx synth overflow\n"); std::abort(); }
    std::memcpy(&b[0x100], code.data(), code.size());
  }
  // comparator at 0x4c0 (descending; returns *b - *a). On the FIRST invocation only it bumps
  // the counter byte at 0x258 (just past the int[4], before the rela area) and makes a NESTED host call (putchar 'n') from inside the qsort
  // callback - proving guest->host->guest->host FS transitions restore correctly.
  at = 0x4c0;
  {
    ins({0x50, 0x53});            // push rax; push rbx
    ins({0x41, 0x54});            // push r12
    ins({0x48, 0x89, 0xfb});      // mov rbx, rdi
    ins({0x4c, 0x8b, 0xe6});      // mov r12, rsi
    ins({0x80, 0x3d}); d32(0x258 - (at + 4 + 1)); ins({0x00});  // cmp byte [rip->0x258], 0
    ins({0x75, 0x12});            // jne skip (7+5+6 bytes ahead)
    ins({0x80, 0x05}); d32(0x258 - (at + 4 + 1)); ins({0x01});  // add byte [rip->0x258], 1
    ins({0xbf, 0x6e, 0x00, 0x00, 0x00});                        // mov edi, 'n'
    callgot(G_PUTCHAR);           // nested host call from inside the guest comparator
    // skip:
    ins({0x41, 0x8b, 0x04, 0x24});// mov eax, [r12]
    ins({0x2b, 0x03});            // sub eax, [rbx]
    ins({0x41, 0x5c, 0x5b, 0x59, 0xc3});  // pop r12; pop rbx; pop rcx; ret
  }
  // dtor_c at 0x500: the LAST destructor to run (plain atexit, after the thread dtor). It
  // VERIFIES the whole run instead of trusting stdout order: the comparator's nested call
  // happened exactly once (byte 0x258 == 1), the thread dtor ran first with arg 0x51 (byte
  // 0x20e == 0x51), and its own argument is 0x42 - any failure is UD2 (SIGILL: the run is
  // reported signaled, not finished). On success it BRANCHES on its argument and makes TWO
  // host calls: 'b' (arg was 0x42) or 'B' (it was not), then the raw argument byte.
  at = 0x500;
  {
    ins({0x53});                  // push rbx (callee-saved: the ABI requires us to preserve it,
                                  //  and call_with_fs holds the arch_prctl syscall number there)
    ins({0x48, 0x89, 0xfb});      // mov rbx, rdi  (keep the argument across host calls)
    ins({0x80, 0x3d}); d32(0x258 - (at + 4 + 1)); ins({0x01});  // cmp byte [rip->0x258], 1
    ins({0x74, 0x02}); ins({0x0f, 0x0b});   // je +2 (pass) ; ud2
    ins({0x80, 0x3d}); d32(0x259 - (at + 4 + 1)); ins({0x51});  // cmp byte [rip->0x259], 0x51
    ins({0x74, 0x02}); ins({0x0f, 0x0b});   // je +2 (pass) ; ud2
    ins({0x48, 0x83, 0xfb, 0x42});// cmp rbx, 0x42
    ins({0x74, 0x02}); ins({0x0f, 0x0b});   // je +2 ; ud2 (wrong argument is a hard failure)
    ins({0x75, 0x07});            // jne +7 (over the equal-case mov + jmp; never taken here)
    ins({0xbf, 0x62, 0x00, 0x00, 0x00});                        // mov edi, 'b'
    ins({0xeb, 0x05});            // jmp +5
    ins({0xbf, 0x42, 0x00, 0x00, 0x00});                        // mov edi, 'B'
    callgot(G_PUTCHAR);           // first host call
    ins({0x48, 0x89, 0xdf});      // mov rdi, rbx
    callgot(G_PUTCHAR);           // second host call: raw argument byte
    ins({0x5b, 0xc3});            // pop rbx; ret
  }
  if (at > b.size()) { std::fprintf(stderr, "cxx synth overflow\n"); std::abort(); }
  // data
  p64(b, 0x200, 0x240);                               // ptr_guard -> guard byte
  p64(b, 0x208, 0x248);                               // ptr_arr -> int[4]
  p32(b, 0x248, 5); p32(b, 0x24c, 3); p32(b, 0x250, 8); p32(b, 0x254, 1);
  const char* names[6] = {"putchar", "__cxa_atexit", "__cxa_thread_atexit_impl", "qsort", "__cxa_guard_acquire", "__cxa_guard_release"};
  std::vector<std::string> nids;
  for (const char* n : names) nids.push_back(pnr::loader::nid::compute_nid(n, "") + "#A#A");
  std::size_t so = 0x3c8 + 1;
  std::uint32_t off[6];
  for (int i = 0; i < 6; ++i) { off[i] = static_cast<std::uint32_t>(so - 0x3c8); std::memcpy(&b[so], nids[i].c_str(), nids[i].size() + 1); so += nids[i].size() + 1; }
  const std::uint64_t strsz = so - 0x3c8;
  for (int i = 0; i < 6; ++i) { const std::size_t s = 0x320 + 24 * (i + 1); p32(b, s, off[i]); b[s + 4] = 0x12; /* GLOBAL FUNC */ p16(b, s + 6, 0); }
  std::size_t r = 0x260;
  auto rela = [&](std::uint64_t o, std::uint64_t info, std::int64_t addend) { p64(b, r, o); p64(b, r + 8, info); p64(b, r + 16, static_cast<std::uint64_t>(addend)); r += 24; };
  rela(0x200, 8, 0x240);   // RELATIVE -> guard byte
  rela(0x208, 8, 0x248);   // RELATIVE -> int[4]
  for (int i = 0; i < 6; ++i) rela(G_PUTCHAR + 8 * i, (static_cast<std::uint64_t>(i + 1) << 32) | 6, 0);  // GLOB_DAT
  const std::uint64_t relasz = r - 0x260;
  std::size_t dy = 0x440;
  auto dyn = [&](std::int64_t tag, std::uint64_t v) { p64(b, dy, static_cast<std::uint64_t>(tag)); p64(b, dy + 8, v); dy += 16; };
  dyn(6, 0x320); dyn(5, 0x3c8); dyn(10, strsz); dyn(11, 24); dyn(7, 0x260); dyn(8, relasz); dyn(9, 24); dyn(0, 0);
  return b;
}


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
  const std::size_t FMT = 0x400, AB = 0x410, EXP = 0x420, HEX = 0x430, HELLO = 0x440, PF = 0x450, OK = 0x470, FPF = 0x474;
  str(FMT, "%d:%s:%x"); str(AB, "ab"); str(EXP, "-5:ab:ff"); str(HEX, "0x1f"); str(HELLO, "hello"); str(PF, "C3 libc %s %d\n"); str(OK, "ok");
  str(FPF, "%.3f %g\n");  // slice S35: fits the free gap before the GOT at 0x480
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
  // Slice S35: printf("%.3f %g\n", 3.14159, 2.0) - floating point arguments travel in xmm0/xmm1.
  rip({0x48, 0x8D, 0x3D}, FPF);
  emit({0x48, 0xB8, 0x18, 0x2D, 0x44, 0x54, 0xFB, 0x21, 0x09, 0x40});  // mov rax, 3.14159 bits
  emit({0x66, 0x48, 0x0F, 0x6E, 0xC0});                                 // movq xmm0, rax
  emit({0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40});  // mov rax, 2.0 bits
  emit({0x66, 0x48, 0x0F, 0x6E, 0xC8});                                 // movq xmm1, rax
  emit({0xB0, 0x02});                                                   // mov al, 2 (vector register count)
  call(G_PRINTF);
  emit({0x48, 0x83, 0xF8, 0x08}); check(0x74, 6);                       // "3.142 2\n" is 8 chars
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

// Slice S35 (review follow-up): fixed-signature floating point calls. Imports printf plus three
// real-libc FP functions and exercises, with byte-checked expectations: printf with 8 doubles
// (xmm0..xmm7, the full FP argument register set); fmod(double,double) returning double in xmm0;
// fmaxf(float,float) with deliberate garbage in the upper 32 bits of each lane so only correct
// low-32 float32 decoding produces the expected result; ldexp(double,int) mixing the FP and GP
// argument pools (double in xmm0, int in the first GP slot); and xmm0 FP returns throughout.
std::vector<std::uint8_t> build_synthetic_fp_elf() {
  std::vector<std::uint8_t> b(0x780, 0);
  const std::uint8_t ident[] = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0};
  std::memcpy(b.data(), ident, 8);
  p16(b, 0x10, 3); p16(b, 0x12, 62); p32(b, 0x14, 1);
  p64(b, 0x18, 0x100); p64(b, 0x20, 0x40);
  p16(b, 0x34, 64); p16(b, 0x36, 56); p16(b, 0x38, 2); p16(b, 0x3A, 64);
  const std::size_t total = b.size();
  p32(b, 0x40, 1); p32(b, 0x44, 7); p64(b, 0x48, 0); p64(b, 0x50, 0); p64(b, 0x58, 0); p64(b, 0x60, total); p64(b, 0x68, total); p64(b, 0x70, 0x1000);
  p32(b, 0x78, 2); p32(b, 0x7C, 6); p64(b, 0x80, 0x700); p64(b, 0x88, 0x700); p64(b, 0x90, 0x700); p64(b, 0x98, 0x80); p64(b, 0xA0, 0x80); p64(b, 0xA8, 8);
  auto str = [&](std::size_t at, const char* s) { std::memcpy(&b[at], s, std::strlen(s) + 1); };
  const std::size_t FMT8 = 0x400;
  str(FMT8, "%.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f\n");
  enum { G_PRINTF, G_FMOD, G_FMAXF, G_LDEXP, G_N };
  std::size_t pc = 0x100;
  std::vector<std::size_t> fails;
  auto emit = [&](std::initializer_list<int> bytes) { for (int v : bytes) b[pc++] = static_cast<std::uint8_t>(v); };
  auto imm32 = [&](std::uint32_t v) { p32(b, pc, v); pc += 4; };
  auto call = [&](int g) { emit({0xFF, 0x15}); p32(b, pc, static_cast<std::uint32_t>(0x480 + 8 * g - (pc + 4))); pc += 4; };
  auto rip = [&](std::initializer_list<int> op, std::size_t target) { emit(op); p32(b, pc, static_cast<std::uint32_t>(target - (pc + 4))); pc += 4; };
  auto check = [&](int ok_jcc, int n) { emit({ok_jcc, 10, 0xB8}); imm32(n); emit({0xE9}); fails.push_back(pc); pc += 4; };
  auto xmm_load = [&](int lane, std::uint64_t bits) {
    emit({0x48, 0xB8}); p64(b, pc, bits); pc += 8;                 // mov rax, imm64
    emit({0x66, 0x48, 0x0F, 0x6E, 0xC0 + 8 * lane});              // movq xmmK, rax
  };

  emit({0x53, 0x48, 0x83, 0xEC, 0x60});  // push rbx; sub rsp,0x60 (calls run 16-byte aligned)
  // printf with 8 double arguments: xmm0..xmm7 all carry live values.
  rip({0x48, 0x8D, 0x3D}, FMT8);
  const std::uint64_t one_to_eight[8] = {0x3FF0000000000000ULL, 0x4000000000000000ULL, 0x4008000000000000ULL, 0x4010000000000000ULL,
                                         0x4014000000000000ULL, 0x4018000000000000ULL, 0x401C000000000000ULL, 0x4020000000000000ULL};  // 1.0 .. 8.0
  for (int k = 0; k < 8; ++k) xmm_load(k, one_to_eight[k]);
  emit({0xB0, 0x08});                                            // mov al, 8 (vector register count)
  call(G_PRINTF);
  emit({0x48, 0x83, 0xF8, 0x20}); check(0x74, 1);                // "1.0 2.0 3.0 4.0 5.0 6.0 7.0 8.0\n" is 32 chars
  // fmod(7.5, 2.0) == 1.5, returned in xmm0.
  xmm_load(0, 0x401E000000000000ULL);                            // 7.5
  xmm_load(1, 0x4000000000000000ULL);                            // 2.0
  call(G_FMOD);
  emit({0x66, 0x48, 0x0F, 0x7E, 0xC0});                          // movq rax, xmm0
  emit({0x48, 0xBB}); p64(b, pc, 0x3FF8000000000000ULL); pc += 8;  // mov rbx, 1.5 bits
  emit({0x48, 0x39, 0xD8}); check(0x74, 2);                      // cmp rax, rbx
  // fmaxf(1.5f, 0.25f) == 1.5f with garbage in the upper 32 bits of both lanes: only decoding
  // the low 32 bits as float32 yields the expected bits.
  xmm_load(0, 0xDEADBEEF3FC00000ULL);                            // upper garbage | 1.5f
  xmm_load(1, 0x123456783E800000ULL);                            // upper garbage | 0.25f
  call(G_FMAXF);
  emit({0x66, 0x48, 0x0F, 0x7E, 0xC0});                          // movq rax, xmm0
  emit({0x3D}); imm32(0x3FC00000); check(0x74, 3);               // cmp eax, 1.5f bits (low 32)
  // ldexp(3.0, 2) == 12.0: mixed pools (FP argument in xmm0, integer in the first GP slot).
  xmm_load(0, 0x4008000000000000ULL);                            // 3.0
  emit({0xBF}); imm32(2);                                        // mov edi, 2
  call(G_LDEXP);
  emit({0x66, 0x48, 0x0F, 0x7E, 0xC0});                          // movq rax, xmm0
  emit({0x48, 0xBB}); p64(b, pc, 0x4028000000000000ULL); pc += 8;  // mov rbx, 12.0 bits
  emit({0x48, 0x39, 0xD8}); check(0x74, 4);                      // cmp rax, rbx
  emit({0x31, 0xC0});
  const std::size_t epi = pc;
  emit({0x48, 0x83, 0xC4, 0x60, 0x5B, 0xC3});
  for (std::size_t f : fails) p32(b, f, static_cast<std::uint32_t>(epi - (f + 4)));

  // Symbol table must sit immediately before the string table: the loader derives the symbol
  // count as (strtab - symtab) / 24.
  const char* names[G_N] = {"printf", "fmod", "fmaxf", "ldexp"};
  std::size_t so = 0x580 + 1;
  for (int i = 0; i < G_N; ++i) {
    const std::string nm = pnr::loader::nid::compute_nid(names[i], "") + "#A#A";
    const std::size_t sy = 0x500 + 24 * (i + 1);
    p32(b, sy, static_cast<std::uint32_t>(so - 0x580)); b[sy + 4] = 0x12; p16(b, sy + 6, 0);
    std::memcpy(&b[so], nm.c_str(), nm.size() + 1); so += nm.size() + 1;
  }
  const std::size_t strsz = so - 0x580;
  for (int i = 0; i < G_N; ++i) {
    const std::size_t r = 0x600 + 24 * i;
    p64(b, r, 0x480 + 8 * i); p64(b, r + 8, (static_cast<std::uint64_t>(i + 1) << 32) | 6); p64(b, r + 16, 0);
  }
  std::size_t dy = 0x700;
  auto dyn = [&](std::int64_t tag, std::uint64_t v) { p64(b, dy, static_cast<std::uint64_t>(tag)); p64(b, dy + 8, v); dy += 16; };
  dyn(6, 0x500); dyn(5, 0x580); dyn(10, strsz); dyn(11, 24); dyn(7, 0x600); dyn(8, 24 * G_N); dyn(9, 24); dyn(0, 0);
  return b;
}

// Slice S36: stack-spilled arguments. Calls printf with both register pools exhausted and the
// overflow interleaved: 8 doubles fill xmm0-7, fmt + 5 ints fill the gp registers, and the
// remaining args spill to the stack in argument order: i6, i7, i8 (gp overflow), then the 9th
// double (xmm overflow). The expected output only appears if the host consumes the spill window
// in argument order across both classes.
std::vector<std::uint8_t> build_synthetic_stkovf_elf() {
  std::vector<std::uint8_t> b(0x780, 0);
  const std::uint8_t ident[] = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0};
  std::memcpy(b.data(), ident, 8);
  p16(b, 0x10, 3); p16(b, 0x12, 62); p32(b, 0x14, 1);
  p64(b, 0x18, 0x100); p64(b, 0x20, 0x40);
  p16(b, 0x34, 64); p16(b, 0x36, 56); p16(b, 0x38, 2); p16(b, 0x3A, 64);
  const std::size_t total = b.size();
  p32(b, 0x40, 1); p32(b, 0x44, 7); p64(b, 0x48, 0); p64(b, 0x50, 0); p64(b, 0x58, 0); p64(b, 0x60, total); p64(b, 0x68, total); p64(b, 0x70, 0x1000);
  p32(b, 0x78, 2); p32(b, 0x7C, 6); p64(b, 0x80, 0x700); p64(b, 0x88, 0x700); p64(b, 0x90, 0x700); p64(b, 0x98, 0x80); p64(b, 0xA0, 0x80); p64(b, 0xA8, 8);
  auto str = [&](std::size_t at, const char* s) { std::memcpy(&b[at], s, std::strlen(s) + 1); };
  const std::size_t FMT = 0x400;
  str(FMT, "%.0f %d %.0f %d %.0f %d %.0f %d %.0f %d %.0f %d %.0f %d %.0f %d %.0f\n");
  std::size_t pc = 0x100;
  std::vector<std::size_t> fails;
  auto emit = [&](std::initializer_list<int> bytes) { for (int v : bytes) b[pc++] = static_cast<std::uint8_t>(v); };
  auto imm32 = [&](std::uint32_t v) { p32(b, pc, v); pc += 4; };
  auto call = [&](int g) { emit({0xFF, 0x15}); p32(b, pc, static_cast<std::uint32_t>(0x480 + 8 * g - (pc + 4))); pc += 4; };
  auto rip = [&](std::initializer_list<int> op, std::size_t target) { emit(op); p32(b, pc, static_cast<std::uint32_t>(target - (pc + 4))); pc += 4; };
  auto check = [&](int ok_jcc, int n) { emit({ok_jcc, 10, 0xB8}); imm32(n); emit({0xE9}); fails.push_back(pc); pc += 4; };
  auto xmm_load = [&](int lane, std::uint64_t bits) {
    emit({0x48, 0xB8}); p64(b, pc, bits); pc += 8;                 // mov rax, imm64
    emit({0x66, 0x48, 0x0F, 0x6E, 0xC0 + 8 * lane});              // movq xmmK, rax
  };

  emit({0x53, 0x48, 0x83, 0xEC, 0x60});  // push rbx; sub rsp,0x60
  rip({0x48, 0x8D, 0x3D}, FMT);          // lea rdi, [rip+FMT]
  const std::uint64_t one_to_eight[8] = {0x3FF0000000000000ULL, 0x4000000000000000ULL, 0x4008000000000000ULL, 0x4010000000000000ULL,
                                         0x4014000000000000ULL, 0x4018000000000000ULL, 0x401C000000000000ULL, 0x4020000000000000ULL};
  for (int k = 0; k < 8; ++k) xmm_load(k, one_to_eight[k]);       // xmm0-7 = 1.0 .. 8.0
  emit({0xBE}); imm32(11);                                         // mov esi, 11
  emit({0xBA}); imm32(12);                                         // mov edx, 12
  emit({0xB9}); imm32(13);                                         // mov ecx, 13
  emit({0x41, 0xB8}); imm32(14);                                   // mov r8d, 14
  emit({0x41, 0xB9}); imm32(15);                                   // mov r9d, 15
  // Stack spill in argument order (slot0 = 16, slot1 = 17, slot2 = 18, slot3 = 9.0): push in reverse.
  emit({0x49, 0xBA}); p64(b, pc, 0x4022000000000000ULL); pc += 8;  // mov r10, 9.0 bits
  emit({0x41, 0x52});                                              // push r10   (slot3)
  emit({0x41, 0xBA}); imm32(18);                                   // mov r10d, 18
  emit({0x41, 0x52});                                              // push r10   (slot2)
  emit({0x41, 0xBA}); imm32(17);                                   // mov r10d, 17
  emit({0x41, 0x52});                                              // push r10   (slot1)
  emit({0x41, 0xBA}); imm32(16);                                   // mov r10d, 16
  emit({0x41, 0x52});                                              // push r10   (slot0)
  emit({0xB0, 0x08});                                              // mov al, 8 (vector register count)
  call(0);                                                         // printf
  emit({0x48, 0x83, 0xC4, 0x20});                                  // add rsp, 32
  emit({0x48, 0x83, 0xF8, 0x2A}); check(0x74, 1);                  // "1 11 2 12 3 13 4 14 5 15 6 16 7 17 8 18 9\n" is 42 chars
  emit({0x31, 0xC0});
  const std::size_t epi = pc;
  emit({0x48, 0x83, 0xC4, 0x60, 0x5B, 0xC3});
  for (std::size_t f : fails) p32(b, f, static_cast<std::uint32_t>(epi - (f + 4)));

  const char* names[1] = {"printf"};
  std::size_t so = 0x580 + 1;
  for (int i = 0; i < 1; ++i) {
    const std::string nm = pnr::loader::nid::compute_nid(names[i], "") + "#A#A";
    const std::size_t sy = 0x500 + 24 * (i + 1);
    p32(b, sy, static_cast<std::uint32_t>(so - 0x580)); b[sy + 4] = 0x12; p16(b, sy + 6, 0);
    std::memcpy(&b[so], nm.c_str(), nm.size() + 1); so += nm.size() + 1;
  }
  const std::size_t strsz = so - 0x580;
  const std::size_t r = 0x600;
  p64(b, r, 0x480); p64(b, r + 8, (static_cast<std::uint64_t>(1) << 32) | 6); p64(b, r + 16, 0);
  std::size_t dy = 0x700;
  auto dyn = [&](std::int64_t tag, std::uint64_t v) { p64(b, dy, static_cast<std::uint64_t>(tag)); p64(b, dy + 8, v); dy += 16; };
  dyn(6, 0x500); dyn(5, 0x580); dyn(10, strsz); dyn(11, 24); dyn(7, 0x600); dyn(8, 24); dyn(9, 24); dyn(0, 0);
  return b;
}

// Slice S37: a module that reads an asset through the jailed file calls. Opens
// "/app0/asset.txt" (the /app0 prefix maps to the jail root), reads 12 bytes, byte-compares
// them against "pnr-asset-42" in guest code, prints "asset ok" and exits 0 on success;
// failure exits 1..5.
std::vector<std::uint8_t> build_synthetic_file_elf() {
  std::vector<std::uint8_t> b(0x780, 0);
  const std::uint8_t ident[] = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0};
  std::memcpy(b.data(), ident, 8);
  p16(b, 0x10, 3); p16(b, 0x12, 62); p32(b, 0x14, 1);
  p64(b, 0x18, 0x100); p64(b, 0x20, 0x40);
  p16(b, 0x34, 64); p16(b, 0x36, 56); p16(b, 0x38, 2); p16(b, 0x3A, 64);
  const std::size_t total = b.size();
  p32(b, 0x40, 1); p32(b, 0x44, 7); p64(b, 0x48, 0); p64(b, 0x50, 0); p64(b, 0x58, 0); p64(b, 0x60, total); p64(b, 0x68, total); p64(b, 0x70, 0x1000);
  p32(b, 0x78, 2); p32(b, 0x7C, 6); p64(b, 0x80, 0x700); p64(b, 0x88, 0x700); p64(b, 0x90, 0x700); p64(b, 0x98, 0x80); p64(b, 0xA0, 0x80); p64(b, 0xA8, 8);
  auto str = [&](std::size_t at, const char* s) { std::memcpy(&b[at], s, std::strlen(s) + 1); };
  const std::size_t PATH = 0x400, OK = 0x420, BUF = 0x440;
  str(PATH, "/app0/asset.txt"); str(OK, "asset ok");
  enum { G_OPEN, G_READ, G_CLOSE, G_PRINTF, G_N };
  std::size_t pc = 0x100;
  std::vector<std::size_t> fails;
  auto emit = [&](std::initializer_list<int> bytes) { for (int v : bytes) b[pc++] = static_cast<std::uint8_t>(v); };
  auto imm32 = [&](std::uint32_t v) { p32(b, pc, v); pc += 4; };
  auto call = [&](int g) { emit({0xFF, 0x15}); p32(b, pc, static_cast<std::uint32_t>(0x480 + 8 * g - (pc + 4))); pc += 4; };
  auto rip = [&](std::initializer_list<int> op, std::size_t target) { emit(op); p32(b, pc, static_cast<std::uint32_t>(target - (pc + 4))); pc += 4; };
  auto check = [&](int ok_jcc, int n) { emit({ok_jcc, 10, 0xB8}); imm32(n); emit({0xE9}); fails.push_back(pc); pc += 4; };
  auto buf_cmp32 = [&](std::size_t off, std::uint32_t v, int n) {
    emit({0x81, 0x3D}); p32(b, pc, static_cast<std::uint32_t>(BUF + off - (pc + 8))); pc += 4; imm32(v);
    check(0x74, n);
  };

  emit({0x53, 0x48, 0x83, 0xEC, 0x60});  // push rbx; sub rsp,0x60
  // sceKernelOpen("/app0/asset.txt", O_RDONLY)
  rip({0x48, 0x8D, 0x3D}, PATH); emit({0x31, 0xF6}); call(G_OPEN);
  emit({0x48, 0x83, 0xF8, 0x03}); check(0x7D, 1);          // fd >= 3 (jge)
  emit({0x48, 0x89, 0xC3});                                 // mov rbx, rax (fd)
  // sceKernelRead(fd, BUF, 12) == 12
  emit({0x48, 0x89, 0xC7}); rip({0x48, 0x8D, 0x35}, BUF); emit({0xBA}); imm32(12); call(G_READ);
  emit({0x48, 0x83, 0xF8, 0x0C}); check(0x74, 2);
  // content == "pnr-asset-42" (three dword compares)
  buf_cmp32(0, 0x2D726E70u, 3);  // "pnr-"
  buf_cmp32(4, 0x65737361u, 4);  // "asse"
  buf_cmp32(8, 0x32342D74u, 5);  // "t-42"
  // sceKernelClose(fd)
  emit({0x48, 0x89, 0xDF}); call(G_CLOSE);
  // printf("asset ok\n")
  rip({0x48, 0x8D, 0x3D}, OK); emit({0x31, 0xC0}); call(G_PRINTF);
  emit({0x31, 0xC0});
  const std::size_t epi = pc;
  emit({0x48, 0x83, 0xC4, 0x60, 0x5B, 0xC3});
  for (std::size_t f : fails) p32(b, f, static_cast<std::uint32_t>(epi - (f + 4)));

  const char* names[G_N] = {"sceKernelOpen", "sceKernelRead", "sceKernelClose", "printf"};
  std::size_t so = 0x580 + 1;
  for (int i = 0; i < G_N; ++i) {
    const std::string nm = pnr::loader::nid::compute_nid(names[i], "") + "#A#A";
    const std::size_t sy = 0x500 + 24 * (i + 1);
    p32(b, sy, static_cast<std::uint32_t>(so - 0x580)); b[sy + 4] = 0x12; p16(b, sy + 6, 0);
    std::memcpy(&b[so], nm.c_str(), nm.size() + 1); so += nm.size() + 1;
  }
  const std::size_t strsz = so - 0x580;
  for (int i = 0; i < G_N; ++i) {
    const std::size_t r = 0x600 + 24 * i;
    p64(b, r, 0x480 + 8 * i); p64(b, r + 8, (static_cast<std::uint64_t>(i + 1) << 32) | 6); p64(b, r + 16, 0);
  }
  std::size_t dy = 0x700;
  auto dyn = [&](std::int64_t tag, std::uint64_t v) { p64(b, dy, static_cast<std::uint64_t>(tag)); p64(b, dy + 8, v); dy += 16; };
  dyn(6, 0x500); dyn(5, 0x580); dyn(10, strsz); dyn(11, 24); dyn(7, 0x600); dyn(8, 24 * G_N); dyn(9, 24); dyn(0, 0);
  return b;
}

// Slice S38: a module that WRITES an asset through the jailed file calls. Opens
// "/app0/out.bin" with O_WRONLY|O_CREAT, writes "pnr-write-42" (12 bytes), closes, reopens
// read-only, reads the 12 bytes back and byte-compares them in guest code, prints "write ok"
// and exits 0 on success; any deviation jumps to the shared epilogue with a nonzero code.
std::vector<std::uint8_t> build_synthetic_filew_elf() {
  std::vector<std::uint8_t> b(0x780, 0);
  const std::uint8_t ident[] = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0};
  std::memcpy(b.data(), ident, 8);
  p16(b, 0x10, 3); p16(b, 0x12, 62); p32(b, 0x14, 1);
  p64(b, 0x18, 0x100); p64(b, 0x20, 0x40);
  p16(b, 0x34, 64); p16(b, 0x36, 56); p16(b, 0x38, 2); p16(b, 0x3A, 64);
  const std::size_t total = b.size();
  p32(b, 0x40, 1); p32(b, 0x44, 7); p64(b, 0x48, 0); p64(b, 0x50, 0); p64(b, 0x58, 0); p64(b, 0x60, total); p64(b, 0x68, total); p64(b, 0x70, 0x1000);
  p32(b, 0x78, 2); p32(b, 0x7C, 6); p64(b, 0x80, 0x700); p64(b, 0x88, 0x700); p64(b, 0x90, 0x700); p64(b, 0x98, 0x80); p64(b, 0xA0, 0x80); p64(b, 0xA8, 8);
  auto str = [&](std::size_t at, const char* s) { std::memcpy(&b[at], s, std::strlen(s) + 1); };
  const std::size_t PATH = 0x400, OK = 0x420, MSG = 0x440, BUF = 0x460;
  str(PATH, "/app0/out.bin"); str(OK, "write ok"); str(MSG, "pnr-write-42");
  enum { G_OPEN, G_WRITE, G_READ, G_CLOSE, G_PRINTF, G_N };
  std::size_t pc = 0x100;
  std::vector<std::size_t> fails;
  auto emit = [&](std::initializer_list<int> bytes) { for (int v : bytes) b[pc++] = static_cast<std::uint8_t>(v); };
  auto imm32 = [&](std::uint32_t v) { p32(b, pc, v); pc += 4; };
  auto call = [&](int g) { emit({0xFF, 0x15}); p32(b, pc, static_cast<std::uint32_t>(0x480 + 8 * g - (pc + 4))); pc += 4; };
  auto rip = [&](std::initializer_list<int> op, std::size_t target) { emit(op); p32(b, pc, static_cast<std::uint32_t>(target - (pc + 4))); pc += 4; };
  auto check = [&](int ok_jcc, int n) { emit({ok_jcc, 10, 0xB8}); imm32(n); emit({0xE9}); fails.push_back(pc); pc += 4; };
  auto buf_cmp32 = [&](std::size_t off, std::uint32_t v, int n) {
    emit({0x81, 0x3D}); p32(b, pc, static_cast<std::uint32_t>(BUF + off - (pc + 8))); pc += 4; imm32(v);
    check(0x74, n);
  };

  emit({0x53, 0x48, 0x83, 0xEC, 0x60});  // push rbx; sub rsp,0x60
  // fd = sceKernelOpen("/app0/out.bin", O_WRONLY|O_CREAT)   flags = 0x0001|0x0200
  rip({0x48, 0x8D, 0x3D}, PATH); emit({0xBE}); imm32(0x201); call(G_OPEN);
  emit({0x48, 0x83, 0xF8, 0x03}); check(0x7D, 1);          // fd >= 3 (jge)
  emit({0x48, 0x89, 0xC3});                                 // mov rbx, rax (fd)
  // sceKernelWrite(fd, MSG, 12) == 12
  emit({0x48, 0x89, 0xDF}); rip({0x48, 0x8D, 0x35}, MSG); emit({0xBA}); imm32(12); call(G_WRITE);
  emit({0x48, 0x83, 0xF8, 0x0C}); check(0x74, 2);
  // sceKernelClose(fd)
  emit({0x48, 0x89, 0xDF}); call(G_CLOSE);
  // fd2 = sceKernelOpen("/app0/out.bin", O_RDONLY)
  rip({0x48, 0x8D, 0x3D}, PATH); emit({0x31, 0xF6}); call(G_OPEN);
  emit({0x48, 0x83, 0xF8, 0x03}); check(0x7D, 3);
  emit({0x48, 0x89, 0xC3});                                 // mov rbx, rax
  // sceKernelRead(fd2, BUF, 12) == 12
  emit({0x48, 0x89, 0xDF}); rip({0x48, 0x8D, 0x35}, BUF); emit({0xBA}); imm32(12); call(G_READ);
  emit({0x48, 0x83, 0xF8, 0x0C}); check(0x74, 4);
  // sceKernelClose(fd2)
  emit({0x48, 0x89, 0xDF}); call(G_CLOSE);
  // read-back == "pnr-write-42" (three dword compares)
  buf_cmp32(0, 0x2D726E70u, 5);  // "pnr-"
  buf_cmp32(4, 0x74697277u, 6);  // "writ"
  buf_cmp32(8, 0x32342D65u, 7);  // "e-42"
  // printf("write ok\n")
  rip({0x48, 0x8D, 0x3D}, OK); emit({0x31, 0xC0}); call(G_PRINTF);
  emit({0x31, 0xC0});
  const std::size_t epi = pc;
  emit({0x48, 0x83, 0xC4, 0x60, 0x5B, 0xC3});
  for (std::size_t f : fails) p32(b, f, static_cast<std::uint32_t>(epi - (f + 4)));

  const char* names[G_N] = {"sceKernelOpen", "sceKernelWrite", "sceKernelRead", "sceKernelClose", "printf"};
  std::size_t so = 0x590 + 1;
  for (int i = 0; i < G_N; ++i) {
    const std::string nm = pnr::loader::nid::compute_nid(names[i], "") + "#A#A";
    const std::size_t sy = 0x500 + 24 * (i + 1);
    p32(b, sy, static_cast<std::uint32_t>(so - 0x590)); b[sy + 4] = 0x12; p16(b, sy + 6, 0);
    std::memcpy(&b[so], nm.c_str(), nm.size() + 1); so += nm.size() + 1;
  }
  const std::size_t strsz = so - 0x590;
  for (int i = 0; i < G_N; ++i) {
    const std::size_t r = 0x600 + 24 * i;
    p64(b, r, 0x480 + 8 * i); p64(b, r + 8, (static_cast<std::uint64_t>(i + 1) << 32) | 6); p64(b, r + 16, 0);
  }
  std::size_t dy = 0x700;
  auto dyn = [&](std::int64_t tag, std::uint64_t v) { p64(b, dy, static_cast<std::uint64_t>(tag)); p64(b, dy + 8, v); dy += 16; };
  dyn(6, 0x500); dyn(5, 0x590); dyn(10, strsz); dyn(11, 24); dyn(7, 0x600); dyn(8, 24 * G_N); dyn(9, 24); dyn(0, 0);
  return b;
}

// Slice S39: a module that seeks and stats through the jailed file calls. Opens
// "/app0/asset.txt" (seeded with "pnr-asset-42"), lseeks SEEK_SET/SEEK_CUR/SEEK_END and
// checks each returned position, reads at the sought offsets and dword-compares in guest
// code, fstats into guest memory (size qword, is_directory byte), prints "pos ok" and
// exits 0 on success; any deviation jumps to the shared epilogue with a nonzero code.
std::vector<std::uint8_t> build_synthetic_filepos_elf() {
  std::vector<std::uint8_t> b(0x780, 0);
  const std::uint8_t ident[] = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0};
  std::memcpy(b.data(), ident, 8);
  p16(b, 0x10, 3); p16(b, 0x12, 62); p32(b, 0x14, 1);
  p64(b, 0x18, 0x100); p64(b, 0x20, 0x40);
  p16(b, 0x34, 64); p16(b, 0x36, 56); p16(b, 0x38, 2); p16(b, 0x3A, 64);
  const std::size_t total = b.size();
  p32(b, 0x40, 1); p32(b, 0x44, 7); p64(b, 0x48, 0); p64(b, 0x50, 0); p64(b, 0x58, 0); p64(b, 0x60, total); p64(b, 0x68, total); p64(b, 0x70, 0x1000);
  p32(b, 0x78, 2); p32(b, 0x7C, 6); p64(b, 0x80, 0x700); p64(b, 0x88, 0x700); p64(b, 0x90, 0x700); p64(b, 0x98, 0x80); p64(b, 0xA0, 0x80); p64(b, 0xA8, 8);
  auto str = [&](std::size_t at, const char* s) { std::memcpy(&b[at], s, std::strlen(s) + 1); };
  const std::size_t PATH = 0x400, OK = 0x420, BUF = 0x440, BUF2 = 0x450, STAT = 0x460;
  str(PATH, "/app0/asset.txt"); str(OK, "pos ok");
  // 6 imports: symtab needs 7 entries -> strtab at 0x5A8 ((0x5A8-0x500)/24 == 7 symbols);
  // NID strings are 16 bytes each, so strtab runs to 0x609 and rela moves to 0x620.
  enum { G_OPEN, G_LSEEK, G_READ, G_FSTAT, G_CLOSE, G_PRINTF, G_N };
  std::size_t pc = 0x100;
  std::vector<std::size_t> fails;
  auto emit = [&](std::initializer_list<int> bytes) { for (int v : bytes) b[pc++] = static_cast<std::uint8_t>(v); };
  auto imm32 = [&](std::uint32_t v) { p32(b, pc, v); pc += 4; };
  auto call = [&](int g) { emit({0xFF, 0x15}); p32(b, pc, static_cast<std::uint32_t>(0x480 + 8 * g - (pc + 4))); pc += 4; };
  auto rip = [&](std::initializer_list<int> op, std::size_t target) { emit(op); p32(b, pc, static_cast<std::uint32_t>(target - (pc + 4))); pc += 4; };
  auto check = [&](int ok_jcc, int n) { emit({ok_jcc, 10, 0xB8}); imm32(n); emit({0xE9}); fails.push_back(pc); pc += 4; };
  auto cmp32 = [&](std::size_t addr, std::uint32_t v, int n) {
    emit({0x81, 0x3D}); p32(b, pc, static_cast<std::uint32_t>(addr - (pc + 8))); pc += 4; imm32(v);
    check(0x74, n);
  };

  emit({0x53, 0x48, 0x83, 0xEC, 0x60});  // push rbx; sub rsp,0x60
  // fd = sceKernelOpen("/app0/asset.txt", O_RDONLY)
  rip({0x48, 0x8D, 0x3D}, PATH); emit({0x31, 0xF6}); call(G_OPEN);
  emit({0x48, 0x83, 0xF8, 0x03}); check(0x7D, 1);          // fd >= 3 (jge)
  emit({0x48, 0x89, 0xC3});                                 // mov rbx, rax (fd)
  // sceKernelLseek(fd, 4, SEEK_SET) == 4
  emit({0x48, 0x89, 0xDF}); emit({0xBE}); imm32(4); emit({0x31, 0xD2}); call(G_LSEEK);
  emit({0x48, 0x83, 0xF8, 0x04}); check(0x74, 2);
  // sceKernelRead(fd, BUF, 4) == 4, BUF == "asse"
  emit({0x48, 0x89, 0xDF}); rip({0x48, 0x8D, 0x35}, BUF); emit({0xBA}); imm32(4); call(G_READ);
  emit({0x48, 0x83, 0xF8, 0x04}); check(0x74, 3);
  cmp32(BUF, 0x65737361u, 4);  // "asse"
  // sceKernelLseek(fd, 1, SEEK_CUR) == 9
  emit({0x48, 0x89, 0xDF}); emit({0xBE}); imm32(1); emit({0xBA}); imm32(1); call(G_LSEEK);
  emit({0x48, 0x83, 0xF8, 0x09}); check(0x74, 5);
  // sceKernelRead(fd, BUF2, 3) == 3, BUF2 == "-42\0"
  emit({0x48, 0x89, 0xDF}); rip({0x48, 0x8D, 0x35}, BUF2); emit({0xBA}); imm32(3); call(G_READ);
  emit({0x48, 0x83, 0xF8, 0x03}); check(0x74, 6);
  cmp32(BUF2, 0x0032342Du, 7);  // "-42" + zero byte (image is zero-filled)
  // sceKernelLseek(fd, -2, SEEK_END) == 10
  emit({0x48, 0x89, 0xDF}); emit({0x48, 0xC7, 0xC6}); imm32(0xFFFFFFFEu); emit({0xBA}); imm32(2); call(G_LSEEK);
  emit({0x48, 0x83, 0xF8, 0x0A}); check(0x74, 8);
  // sceKernelFstat(fd, STAT) == 0
  emit({0x48, 0x89, 0xDF}); rip({0x48, 0x8D, 0x35}, STAT); call(G_FSTAT);
  emit({0x48, 0x83, 0xF8, 0x00}); check(0x74, 9);
  // STAT.size == 12 (cmp qword [rip], imm32; pc is past the 3 opcode bytes, rip = pc+8)
  emit({0x48, 0x81, 0x3D}); p32(b, pc, static_cast<std::uint32_t>(STAT - (pc + 8))); pc += 4; imm32(12);
  check(0x74, 10);
  // STAT.is_directory == 0 (cmp byte [rip], imm8; pc past the 2 opcode bytes, rip = pc+5)
  emit({0x80, 0x3D}); p32(b, pc, static_cast<std::uint32_t>(STAT + 8 - (pc + 5))); pc += 4; emit({0x00});
  check(0x74, 11);
  // sceKernelClose(fd)
  emit({0x48, 0x89, 0xDF}); call(G_CLOSE);
  // printf("pos ok\n")
  rip({0x48, 0x8D, 0x3D}, OK); emit({0x31, 0xC0}); call(G_PRINTF);
  emit({0x31, 0xC0});
  const std::size_t epi = pc;
  emit({0x48, 0x83, 0xC4, 0x60, 0x5B, 0xC3});
  for (std::size_t f : fails) p32(b, f, static_cast<std::uint32_t>(epi - (f + 4)));

  const char* names[G_N] = {"sceKernelOpen", "sceKernelLseek", "sceKernelRead", "sceKernelFstat", "sceKernelClose", "printf"};
  std::size_t so = 0x5A8 + 1;
  for (int i = 0; i < G_N; ++i) {
    const std::string nm = pnr::loader::nid::compute_nid(names[i], "") + "#A#A";
    const std::size_t sy = 0x500 + 24 * (i + 1);
    p32(b, sy, static_cast<std::uint32_t>(so - 0x5A8)); b[sy + 4] = 0x12; p16(b, sy + 6, 0);
    std::memcpy(&b[so], nm.c_str(), nm.size() + 1); so += nm.size() + 1;
  }
  const std::size_t strsz = so - 0x5A8;
  for (int i = 0; i < G_N; ++i) {
    const std::size_t r = 0x620 + 24 * i;
    p64(b, r, 0x480 + 8 * i); p64(b, r + 8, (static_cast<std::uint64_t>(i + 1) << 32) | 6); p64(b, r + 16, 0);
  }
  std::size_t dy = 0x700;
  auto dyn = [&](std::int64_t tag, std::uint64_t v) { p64(b, dy, static_cast<std::uint64_t>(tag)); p64(b, dy + 8, v); dy += 16; };
  dyn(6, 0x500); dyn(5, 0x5A8); dyn(10, strsz); dyn(11, 24); dyn(7, 0x620); dyn(8, 24 * G_N); dyn(9, 24); dyn(0, 0);
  return b;
}

// Slice S40: prove the 7th scalar (stack) argument is captured end-to-end, using a CLEARLY
// SYNTHETIC host import (libpnrtest pnrTestSevenScalarArgs), not an invented Sony API. The
// guest puts 1..6 in rdi..r9, 700 on the stack, calls through the NID-bound import and
// checks rax == 721 in guest code; prints "abi7 ok" and exits 0. Requires the module-run
// side to install the synthetic imports (PNR_TEST_IMPORTS=1).
std::vector<std::uint8_t> build_synthetic_abi7_elf() {
  std::vector<std::uint8_t> b(0x780, 0);
  const std::uint8_t ident[] = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0};
  std::memcpy(b.data(), ident, 8);
  p16(b, 0x10, 3); p16(b, 0x12, 62); p32(b, 0x14, 1);
  p64(b, 0x18, 0x100); p64(b, 0x20, 0x40);
  p16(b, 0x34, 64); p16(b, 0x36, 56); p16(b, 0x38, 2); p16(b, 0x3A, 64);
  const std::size_t total = b.size();
  p32(b, 0x40, 1); p32(b, 0x44, 7); p64(b, 0x48, 0); p64(b, 0x50, 0); p64(b, 0x58, 0); p64(b, 0x60, total); p64(b, 0x68, total); p64(b, 0x70, 0x1000);
  p32(b, 0x78, 2); p32(b, 0x7C, 6); p64(b, 0x80, 0x700); p64(b, 0x88, 0x700); p64(b, 0x90, 0x700); p64(b, 0x98, 0x80); p64(b, 0xA0, 0x80); p64(b, 0xA8, 8);
  auto str = [&](std::size_t at, const char* s) { std::memcpy(&b[at], s, std::strlen(s) + 1); };
  const std::size_t OK = 0x400;
  str(OK, "abi7 ok");
  // 2 imports: symtab 3 entries -> strtab at 0x548 ((0x548-0x500)/24 == 3 symbols).
  // NID strings are 16 bytes each; strtab ends 0x569, rela sits clear at 0x580.
  enum { G_SEVEN, G_PRINTF, G_N };
  std::size_t pc = 0x100;
  std::vector<std::size_t> fails;
  auto emit = [&](std::initializer_list<int> bytes) { for (int v : bytes) b[pc++] = static_cast<std::uint8_t>(v); };
  auto imm32 = [&](std::uint32_t v) { p32(b, pc, v); pc += 4; };
  auto call = [&](int g) { emit({0xFF, 0x15}); p32(b, pc, static_cast<std::uint32_t>(0x480 + 8 * g - (pc + 4))); pc += 4; };
  auto rip = [&](std::initializer_list<int> op, std::size_t target) { emit(op); p32(b, pc, static_cast<std::uint32_t>(target - (pc + 4))); pc += 4; };
  auto check = [&](int ok_jcc, int n) { emit({ok_jcc, 10, 0xB8}); imm32(n); emit({0xE9}); fails.push_back(pc); pc += 4; };

  emit({0x53, 0x48, 0x83, 0xEC, 0x60});  // push rbx; sub rsp,0x60
  // rdi..r9 = 1,2,3,4,5,6
  emit({0xBF}); imm32(1); emit({0xBE}); imm32(2); emit({0xBA}); imm32(3);
  emit({0xB9}); imm32(4); emit({0x41, 0xB8}); imm32(5); emit({0x41, 0xB9}); imm32(6);
  // 7th arg on the stack (slot0 = [rsp] at the call); sub 16 keeps rsp 16-aligned.
  emit({0x48, 0x83, 0xEC, 0x10});                      // sub rsp, 16
  emit({0x48, 0xC7, 0x04, 0x24}); imm32(700);          // mov qword [rsp], 700
  call(G_SEVEN);
  emit({0x48, 0x83, 0xC4, 0x10});                      // add rsp, 16
  // rax == 1+2+3+4+5+6+700 == 721
  emit({0x48, 0x81, 0xF8}); imm32(721); check(0x74, 1);   // cmp rax, 721; je
  // printf("abi7 ok\n")
  rip({0x48, 0x8D, 0x3D}, OK); emit({0x31, 0xC0}); call(G_PRINTF);
  emit({0x31, 0xC0});
  const std::size_t epi = pc;
  emit({0x48, 0x83, 0xC4, 0x60, 0x5B, 0xC3});
  for (std::size_t f : fails) p32(b, f, static_cast<std::uint32_t>(epi - (f + 4)));

  const char* names[G_N] = {"pnrTestSevenScalarArgs", "printf"};
  std::size_t so = 0x548 + 1;
  for (int i = 0; i < G_N; ++i) {
    const std::string nm = pnr::loader::nid::compute_nid(names[i], "") + "#A#A";
    const std::size_t sy = 0x500 + 24 * (i + 1);
    p32(b, sy, static_cast<std::uint32_t>(so - 0x548)); b[sy + 4] = 0x12; p16(b, sy + 6, 0);
    std::memcpy(&b[so], nm.c_str(), nm.size() + 1); so += nm.size() + 1;
  }
  const std::size_t strsz = so - 0x548;
  for (int i = 0; i < G_N; ++i) {
    const std::size_t r = 0x580 + 24 * i;
    p64(b, r, 0x480 + 8 * i); p64(b, r + 8, (static_cast<std::uint64_t>(i + 1) << 32) | 6); p64(b, r + 16, 0);
  }
  std::size_t dy = 0x700;
  auto dyn = [&](std::int64_t tag, std::uint64_t v) { p64(b, dy, static_cast<std::uint64_t>(tag)); p64(b, dy + 8, v); dy += 16; };
  dyn(6, 0x500); dyn(5, 0x548); dyn(10, strsz); dyn(11, 24); dyn(7, 0x580); dyn(8, 24 * G_N); dyn(9, 24); dyn(0, 0);
  return b;
}

// Slice S41: a module that mmaps through the existing 6-argument "mmap" binding
// (loader::sony_mmap, Sony constants translated host-side). Guest code calls
// mmap(0, 4096, PROT_CPU_READ|CPU_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0), checks the pointer
// is nonzero and page-aligned, writes a qword pattern into the page and reads it back in
// guest machine code, then munmap(ptr, 4096). Prints "mmap ok" and exits 0 on
// success; any deviation jumps to the shared epilogue with a nonzero code.
std::vector<std::uint8_t> build_synthetic_mmap_elf() {
  std::vector<std::uint8_t> b(0x780, 0);
  const std::uint8_t ident[] = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0};
  std::memcpy(b.data(), ident, 8);
  p16(b, 0x10, 3); p16(b, 0x12, 62); p32(b, 0x14, 1);
  p64(b, 0x18, 0x100); p64(b, 0x20, 0x40);
  p16(b, 0x34, 64); p16(b, 0x36, 56); p16(b, 0x38, 2); p16(b, 0x3A, 64);
  const std::size_t total = b.size();
  p32(b, 0x40, 1); p32(b, 0x44, 7); p64(b, 0x48, 0); p64(b, 0x50, 0); p64(b, 0x58, 0); p64(b, 0x60, total); p64(b, 0x68, total); p64(b, 0x70, 0x1000);
  p32(b, 0x78, 2); p32(b, 0x7C, 6); p64(b, 0x80, 0x700); p64(b, 0x88, 0x700); p64(b, 0x90, 0x700); p64(b, 0x98, 0x80); p64(b, 0xA0, 0x80); p64(b, 0xA8, 8);
  auto str = [&](std::size_t at, const char* s) { std::memcpy(&b[at], s, std::strlen(s) + 1); };
  const std::size_t OK = 0x400;
  str(OK, "mmap ok");
  // 3 imports: symtab 4 entries -> strtab at 0x560 ((0x560-0x500)/24 == 4 symbols).
  // NID strings are 16 bytes each; strtab ends 0x591, rela sits clear at 0x5A0.
  enum { G_MMAP, G_MUNMAP, G_PRINTF, G_N };
  std::size_t pc = 0x100;
  std::vector<std::size_t> fails;
  auto emit = [&](std::initializer_list<int> bytes) { for (int v : bytes) b[pc++] = static_cast<std::uint8_t>(v); };
  auto imm32 = [&](std::uint32_t v) { p32(b, pc, v); pc += 4; };
  auto call = [&](int g) { emit({0xFF, 0x15}); p32(b, pc, static_cast<std::uint32_t>(0x480 + 8 * g - (pc + 4))); pc += 4; };
  auto rip = [&](std::initializer_list<int> op, std::size_t target) { emit(op); p32(b, pc, static_cast<std::uint32_t>(target - (pc + 4))); pc += 4; };
  auto check = [&](int ok_jcc, int n) { emit({ok_jcc, 10, 0xB8}); imm32(n); emit({0xE9}); fails.push_back(pc); pc += 4; };

  emit({0x53, 0x48, 0x83, 0xEC, 0x60});  // push rbx; sub rsp,0x60
  // mmap(0, 4096, SONY_PROT_CPU_READ|SONY_PROT_CPU_WRITE, SONY_MAP_PRIVATE|SONY_MAP_ANON, -1, 0)
  emit({0x31, 0xFF});                                // xor edi, edi (addr = 0)
  emit({0xBE}); imm32(4096);                         // mov esi, 4096 (len)
  emit({0xBA}); imm32(3);                            // mov edx, 3 (prot)
  emit({0xB9}); imm32(0x1002);                       // mov ecx, 0x1002 (flags)
  emit({0x49, 0xC7, 0xC0}); imm32(0xFFFFFFFFu);      // mov r8, -1 (fd, sign-extended)
  emit({0x45, 0x31, 0xC9});                          // xor r9d, r9d (offset = 0)
  call(G_MMAP);
  emit({0x48, 0x85, 0xC0}); check(0x75, 1);          // test rax,rax; jnz ok
  emit({0x48, 0xA9}); imm32(0xFFF); check(0x74, 2);  // test rax,0xFFF; jz ok (page-aligned)
  emit({0x48, 0x89, 0xC3});                          // mov rbx, rax (page)
  // write a qword pattern into the page, read it back
  emit({0x49, 0xBA}); p64(b, pc, 0xA5A5DEADBEEF5A5AULL); pc += 8;  // mov r10, pattern
  emit({0x4C, 0x89, 0x13});                          // mov [rbx], r10
  emit({0x4C, 0x39, 0x13}); check(0x74, 3);          // cmp [rbx], r10; je ok
  // munmap(page, 4096) == 0 (untracked POSIX unmap, pairs with the "mmap" binding)
  emit({0x48, 0x89, 0xDF});                          // mov rdi, rbx
  emit({0xBE}); imm32(4096);                         // mov esi, 4096
  call(G_MUNMAP);
  emit({0x48, 0x85, 0xC0}); check(0x74, 4);          // test rax,rax; jz ok
  // printf("mmap ok\n")
  rip({0x48, 0x8D, 0x3D}, OK); emit({0x31, 0xC0}); call(G_PRINTF);
  emit({0x31, 0xC0});
  const std::size_t epi = pc;
  emit({0x48, 0x83, 0xC4, 0x60, 0x5B, 0xC3});
  for (std::size_t f : fails) p32(b, f, static_cast<std::uint32_t>(epi - (f + 4)));

  const char* names[G_N] = {"mmap", "munmap", "printf"};
  std::size_t so = 0x560 + 1;
  for (int i = 0; i < G_N; ++i) {
    const std::string nm = pnr::loader::nid::compute_nid(names[i], "") + "#A#A";
    const std::size_t sy = 0x500 + 24 * (i + 1);
    p32(b, sy, static_cast<std::uint32_t>(so - 0x560)); b[sy + 4] = 0x12; p16(b, sy + 6, 0);
    std::memcpy(&b[so], nm.c_str(), nm.size() + 1); so += nm.size() + 1;
  }
  const std::size_t strsz = so - 0x560;
  for (int i = 0; i < G_N; ++i) {
    const std::size_t r = 0x5A0 + 24 * i;
    p64(b, r, 0x480 + 8 * i); p64(b, r + 8, (static_cast<std::uint64_t>(i + 1) << 32) | 6); p64(b, r + 16, 0);
  }
  std::size_t dy = 0x700;
  auto dyn = [&](std::int64_t tag, std::uint64_t v) { p64(b, dy, static_cast<std::uint64_t>(tag)); p64(b, dy + 8, v); dy += 16; };
  dyn(6, 0x500); dyn(5, 0x560); dyn(10, strsz); dyn(11, 24); dyn(7, 0x5A0); dyn(8, 24 * G_N); dyn(9, 24); dyn(0, 0);
  return b;
}

// Slice S42: memory protection transitions through the tracked libkernel memory
// calls, all from guest machine code. The guest maps a page RW via
// sceKernelMapMemory, writes and reads a qword pattern, queries the protection
// (sceKernelQueryMemoryProtection reports start/end/prot), drops the page to
// read-only with sceKernelMTypeprotect, queries again (prot must now be READ)
// and confirms the data survived, restores RW and rewrites, then unmaps with
// sceKernelMunmap. Prints "mprotect ok" and exits 0 on success; a distinct
// nonzero stage code (1-14) on any deviation. This proves the host mprotect
// round-trip and mapping-table bookkeeping as observed through the query call;
// it does not prove fault semantics (no guest SIGSEGV handler exists yet).
std::vector<std::uint8_t> build_synthetic_mprotect_elf() {
  std::vector<std::uint8_t> b(0x780, 0);
  const std::uint8_t ident[] = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0};
  std::memcpy(b.data(), ident, 8);
  p16(b, 0x10, 3); p16(b, 0x12, 62); p32(b, 0x14, 1);
  p64(b, 0x18, 0x100); p64(b, 0x20, 0x40);
  p16(b, 0x34, 64); p16(b, 0x36, 56); p16(b, 0x38, 2); p16(b, 0x3A, 64);
  const std::size_t total = b.size();
  p32(b, 0x40, 1); p32(b, 0x44, 7); p64(b, 0x48, 0); p64(b, 0x50, 0); p64(b, 0x58, 0); p64(b, 0x60, total); p64(b, 0x68, total); p64(b, 0x70, 0x1000);
  p32(b, 0x78, 2); p32(b, 0x7C, 6); p64(b, 0x80, 0x700); p64(b, 0x88, 0x700); p64(b, 0x90, 0x700); p64(b, 0x98, 0x80); p64(b, 0xA0, 0x80); p64(b, 0xA8, 8);
  auto str = [&](std::size_t at, const char* s) { std::memcpy(&b[at], s, std::strlen(s) + 1); };
  const std::size_t OK = 0x400;
  str(OK, "mprotect ok");
  // 5 imports: symtab 6 entries -> strtab at 0x590 ((0x590-0x500)/24 == 6 symbols).
  // NID strings are 16 bytes each; strtab ends 0x5E1, rela sits clear at 0x600.
  enum { G_MAP, G_MTYPE, G_QUERY, G_MUNMAP, G_PRINTF, G_N };
  std::size_t pc = 0x100;
  std::vector<std::size_t> fails;
  auto emit = [&](std::initializer_list<int> bytes) { for (int v : bytes) b[pc++] = static_cast<std::uint8_t>(v); };
  auto imm32 = [&](std::uint32_t v) { p32(b, pc, v); pc += 4; };
  auto call = [&](int g) { emit({0xFF, 0x15}); p32(b, pc, static_cast<std::uint32_t>(0x480 + 8 * g - (pc + 4))); pc += 4; };
  auto rip = [&](std::initializer_list<int> op, std::size_t target) { emit(op); p32(b, pc, static_cast<std::uint32_t>(target - (pc + 4))); pc += 4; };
  auto check = [&](int ok_jcc, int n) { emit({ok_jcc, 10, 0xB8}); imm32(n); emit({0xE9}); fails.push_back(pc); pc += 4; };
  const unsigned long long PAT1 = 0xA5A5DEADBEEF5A5AULL, PAT2 = 0x5A5A1234ABCDA5A5ULL;

  emit({0x53, 0x48, 0x83, 0xEC, 0x60});  // push rbx; sub rsp,0x60
  // sceKernelMapMemory(4096, SCE_KERNEL_PROT_READ|WRITE)
  emit({0xBF}); imm32(4096);                         // mov edi, 4096
  emit({0xBE}); imm32(3);                            // mov esi, 3
  call(G_MAP);
  emit({0x48, 0x85, 0xC0}); check(0x75, 1);          // test rax,rax; jnz ok
  emit({0x48, 0xA9}); imm32(0xFFF); check(0x74, 2);  // test rax,0xFFF; jz ok
  emit({0x48, 0x89, 0xC3});                          // mov rbx, rax (page)
  // write PAT1 into the page, read it back
  emit({0x49, 0xBA}); p64(b, pc, PAT1); pc += 8;     // mov r10, PAT1
  emit({0x4C, 0x89, 0x13});                          // mov [rbx], r10
  emit({0x4C, 0x39, 0x13}); check(0x74, 3);          // cmp [rbx], r10; je ok
  // sceKernelQueryMemoryProtection(page, &start, &end, &prot)
  emit({0x48, 0x89, 0xDF});                          // mov rdi, rbx
  emit({0x48, 0x8D, 0x74, 0x24, 0x10});              // lea rsi, [rsp+0x10]
  emit({0x48, 0x8D, 0x54, 0x24, 0x18});              // lea rdx, [rsp+0x18]
  emit({0x48, 0x8D, 0x4C, 0x24, 0x20});              // lea rcx, [rsp+0x20]
  call(G_QUERY);
  emit({0x48, 0x85, 0xC0}); check(0x74, 4);          // test rax,rax; jz ok
  emit({0x48, 0x8B, 0x44, 0x24, 0x10});              // mov rax, [rsp+0x10]
  emit({0x48, 0x39, 0xD8}); check(0x74, 5);          // cmp rax, rbx; je ok (start==page)
  emit({0x48, 0x8B, 0x44, 0x24, 0x18});              // mov rax, [rsp+0x18]
  emit({0x48, 0x8D, 0x93}); imm32(0x1000);           // lea rdx, [rbx+0x1000]
  emit({0x48, 0x39, 0xD0}); check(0x74, 6);          // cmp rax, rdx; je ok (end==page+4096)
  emit({0x48, 0x83, 0x7C, 0x24, 0x20, 0x03}); check(0x74, 7);  // cmp qword [rsp+0x20], 3; je ok (RW)
  // sceKernelMTypeprotect(page, 4096, SCE_KERNEL_PROT_READ)
  emit({0x48, 0x89, 0xDF});                          // mov rdi, rbx
  emit({0xBE}); imm32(4096);                         // mov esi, 4096
  emit({0xBA}); imm32(1);                            // mov edx, 1
  call(G_MTYPE);
  emit({0x48, 0x85, 0xC0}); check(0x74, 8);          // test rax,rax; jz ok
  // query again: prot must now be READ
  emit({0x48, 0x89, 0xDF});                          // mov rdi, rbx
  emit({0x48, 0x8D, 0x74, 0x24, 0x10});              // lea rsi, [rsp+0x10]
  emit({0x48, 0x8D, 0x54, 0x24, 0x18});              // lea rdx, [rsp+0x18]
  emit({0x48, 0x8D, 0x4C, 0x24, 0x20});              // lea rcx, [rsp+0x20]
  call(G_QUERY);
  emit({0x48, 0x85, 0xC0}); check(0x74, 9);          // test rax,rax; jz ok
  emit({0x48, 0x83, 0x7C, 0x24, 0x20, 0x01}); check(0x74, 10); // cmp qword [rsp+0x20], 1; je ok (RO)
  // data survived the protection change
  emit({0x49, 0xBA}); p64(b, pc, PAT1); pc += 8;     // mov r10, PAT1
  emit({0x4C, 0x39, 0x13}); check(0x74, 11);         // cmp [rbx], r10; je ok
  // restore RW and rewrite
  emit({0x48, 0x89, 0xDF});                          // mov rdi, rbx
  emit({0xBE}); imm32(4096);                         // mov esi, 4096
  emit({0xBA}); imm32(3);                            // mov edx, 3
  call(G_MTYPE);
  emit({0x48, 0x85, 0xC0}); check(0x74, 12);         // test rax,rax; jz ok
  emit({0x49, 0xBA}); p64(b, pc, PAT2); pc += 8;     // mov r10, PAT2
  emit({0x4C, 0x89, 0x13});                          // mov [rbx], r10
  emit({0x4C, 0x39, 0x13}); check(0x74, 13);         // cmp [rbx], r10; je ok
  // sceKernelMunmap(page, 4096) == 0
  emit({0x48, 0x89, 0xDF});                          // mov rdi, rbx
  emit({0xBE}); imm32(4096);                         // mov esi, 4096
  call(G_MUNMAP);
  emit({0x48, 0x85, 0xC0}); check(0x74, 14);         // test rax,rax; jz ok
  // printf("mprotect ok\n")
  rip({0x48, 0x8D, 0x3D}, OK); emit({0x31, 0xC0}); call(G_PRINTF);
  emit({0x31, 0xC0});
  const std::size_t epi = pc;
  emit({0x48, 0x83, 0xC4, 0x60, 0x5B, 0xC3});
  for (std::size_t f : fails) p32(b, f, static_cast<std::uint32_t>(epi - (f + 4)));

  const char* names[G_N] = {"sceKernelMapMemory", "sceKernelMTypeprotect",
                            "sceKernelQueryMemoryProtection", "sceKernelMunmap", "printf"};
  std::size_t so = 0x590 + 1;
  for (int i = 0; i < G_N; ++i) {
    const std::string nm = pnr::loader::nid::compute_nid(names[i], "") + "#A#A";
    const std::size_t sy = 0x500 + 24 * (i + 1);
    p32(b, sy, static_cast<std::uint32_t>(so - 0x590)); b[sy + 4] = 0x12; p16(b, sy + 6, 0);
    std::memcpy(&b[so], nm.c_str(), nm.size() + 1); so += nm.size() + 1;
  }
  const std::size_t strsz = so - 0x590;
  for (int i = 0; i < G_N; ++i) {
    const std::size_t r = 0x600 + 24 * i;
    p64(b, r, 0x480 + 8 * i); p64(b, r + 8, (static_cast<std::uint64_t>(i + 1) << 32) | 6); p64(b, r + 16, 0);
  }
  std::size_t dy = 0x700;
  auto dyn = [&](std::int64_t tag, std::uint64_t v) { p64(b, dy, static_cast<std::uint64_t>(tag)); p64(b, dy + 8, v); dy += 16; };
  dyn(6, 0x500); dyn(5, 0x590); dyn(10, strsz); dyn(11, 24); dyn(7, 0x600); dyn(8, 24 * G_N); dyn(9, 24); dyn(0, 0);
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
