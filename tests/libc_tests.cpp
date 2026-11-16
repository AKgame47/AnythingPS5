// Core slice C3 tests: libSceLibcInternal / libSceSysmodule subset. Unit calls go through the
// registry like a loader would (by NID); a synthetic machine-code module covers the thunk path.
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "loader/module_loader.hpp"
#include "loader/nid.hpp"
#include "prx/libc_abi.hpp"
#include "prx/prx.hpp"
#include "tools/synth_elf.hpp"

void pnr_libc_tests(int& pass, int& fail) {
  using namespace pnr::prx;
  auto check = [&](bool ok, const std::string& l) { if (ok) ++pass; else ++fail; std::printf("%s %s\n", ok ? "PASS" : "FAIL", l.c_str()); };
  PrxRegistry reg;
  auto call = [&](const char* name, std::vector<std::int64_t> a) -> std::int64_t {
    const PrxSymbol* s = reg.find_name(name);
    if (!s || !s->implemented) { std::printf("missing %s\n", name); return -999999; }
    pnr::prx::GuestCallArgs g{};
    for (std::size_t k = 0; k < a.size() && k < 6; ++k) g.i[k] = a[k];
    return s->invoke(g);
  };
  auto P = [](const void* p) { return reinterpret_cast<std::int64_t>(p); };

  // Formatter.
  check(format_c("%d|%5d|%-5d|%05d", {-7, 42, 42, -42}) == "-7|   42|42   |-0042", "fmt: %d with width, left and zero padding");
  check(format_c("%d %u", {0xFFFFFFFFll, 0xFFFFFFFFll}) == "-1 4294967295", "fmt: %d and %u use 32-bit semantics without l");
  check(format_c("%ld %lu %lld %zu", {-1, -1, -1, 5}) == "-1 18446744073709551615 -1 5", "fmt: l, ll and z use 64 bits");
  check(format_c("%x %X %o %hhx %hx", {255, 255, 8, 0x1ff, 0x1ffff}) == "ff FF 10 ff ffff", "fmt: x X o with h and hh");
  check(format_c("%s|%8s|%-4s|%c|%%", {P("hi"), P("abc"), P("z"), 'Q'}) == "hi|     abc|z   |Q|%", "fmt: s with width, c, %%");
  check(format_c("%s", {0}) == "(null)", "fmt: a null string prints (null)");
  check(format_c("%p", {0x1000}) == "0x1000", "fmt: %p");
  check(format_c("%f and %e", {1}) == "%f and %e", "fmt: without an xmm pool, floating point conversions stay literal");

  // Slice S35: floating point conversions consume the xmm pool (SysV varargs rule),
  // independently of the gp pool.
  { auto fpa = [](std::initializer_list<std::int64_t> gi, std::initializer_list<double> gx) {
      GuestCallArgs g{}; std::size_t k = 0;
      for (std::int64_t v : gi) { if (k < 6) g.i[k++] = v; }
      k = 0; for (double v : gx) { if (k < 8) g.x[k++] = v; }
      return g;
    };
    check(format_c_guest("%f", fpa({}, {3.5})) == "3.500000", "fmt: %f default precision");
    check(format_c_guest("%.3f", fpa({}, {3.14159})) == "3.142", "fmt: %.3f rounds via host libc");
    check(format_c_guest("%e", fpa({}, {0.125})) == "1.250000e-01", "fmt: %e");
    check(format_c_guest("%g|%G", fpa({}, {2.0, 0.00001234})) == "2|1.234E-05", "fmt: %g and %G");
    check(format_c_guest("%10.2f|%-8.1f|", fpa({}, {3.5, 2.5})) == "      3.50|2.5     |", "fmt: %f with width, precision and left flag");
    check(format_c_guest("%08.2f", fpa({}, {-3.5})) == "-0003.50", "fmt: %f zero padding keeps the sign first");
    check(format_c_guest("%f", fpa({}, {0.0})) == "0.000000", "fmt: %f of zero");
    check(format_c_guest("%f", fpa({}, {-0.0})) == "-0.000000", "fmt: %f of negative zero");
    check(format_c_guest("%f %f", fpa({}, {1.0})) == "1.000000 0.000000", "fmt: the xmm pool has 8 slots; unset slots read as 0.0 in tests (real thunks pass the live registers)");
    check(format_c_guest("%Lf", fpa({}, {1.0})) == "%Lf", "fmt: long double is refused (literal)");
    check(format_c_guest("%d %f %s %e", fpa({7, P("ok")}, {0.5, 8.0})) == "7 0.500000 ok 8.000000e+00",
          "fmt: gp and xmm pools advance independently");
    check(format_c_guest("%a", fpa({}, {1.0})) == "0x1p+0", "fmt: %a hex float");
  }
  check(format_c("%d %d", {1}) == "1 %d", "fmt: a missing argument stays literal");
  check(format_c("100%", {}) == "100%", "fmt: trailing percent");

  // String and memory functions.
  char buf[32];
  check(call("snprintf", {P(buf), 6, P("%s-%d"), P("abcdef"), 12}) == 9 && std::strcmp(buf, "abcde") == 0, "snprintf: truncates, NUL terminates, returns the full length");
  check(call("snprintf", {0, 0, P("xyz")}) == 3, "snprintf: size 0 only measures");
  check(call("sprintf", {P(buf), P("%d+%d"), 2, 3}) == 3 && std::strcmp(buf, "2+3") == 0, "sprintf: formats");
  char a[16] = "hello", d[16] = {};
  check(call("strcpy", {P(d), P(a)}) == P(d) && std::strcmp(d, "hello") == 0, "strcpy");
  check(call("strcat", {P(d), P(" w")}) == P(d) && std::strcmp(d, "hello w") == 0, "strcat");
  check(call("strncmp", {P("abcd"), P("abzz"), 2}) == 0 && call("strncmp", {P("abcd"), P("abzz"), 3}) < 0, "strncmp");
  check(call("strchr", {P("a/b/c"), '/'}) != 0 && call("strchr", {P("abc"), '/'}) == 0, "strchr finds or returns null");
  check(std::strcmp(reinterpret_cast<const char*>(call("strrchr", {P("a/b/c"), '/'})), "/c") == 0, "strrchr finds the last slash");
  check(std::strcmp(reinterpret_cast<const char*>(call("strstr", {P("find the needle"), P("the")})), "the needle") == 0 && call("strstr", {P("abc"), P("zz")}) == 0, "strstr");
  char ov[16] = "abcdef";
  call("memmove", {P(ov + 2), P(ov), 4});
  check(std::memcmp(ov, "ababcd", 6) == 0, "memmove: overlapping ranges are safe");
  check(call("memcmp", {P("abc"), P("abd"), 3}) < 0 && call("memcmp", {P("abc"), P("abd"), 2}) == 0, "memcmp");
  check(call("memchr", {P("abc"), 'c', 3}) != 0 && call("memchr", {P("abc"), 'z', 3}) == 0, "memchr");
  check(call("strcpy", {0, P("x")}) == 0 && call("strlen", {0}) == 0, "null pointers are refused where the signature allows");
  void* z = reinterpret_cast<void*>(call("calloc", {4, 8}));
  bool zeroed = z != nullptr; for (int i = 0; z && i < 32; ++i) zeroed = zeroed && reinterpret_cast<char*>(z)[i] == 0;
  z = reinterpret_cast<void*>(call("realloc", {P(z), 64}));
  check(zeroed && z != nullptr, "calloc zeroes and realloc grows");
  call("free", {P(z)});

  // Numbers and characters.
  check(call("atoi", {P("  -42x")}) == -42, "atoi");
  const char* num = "0x1fZ"; const char* end = nullptr;
  check(call("strtol", {P(num), P(&end), 0}) == 31 && end == num + 4, "strtol: base 0 hex and endptr");
  check(call("strtol", {P("777"), 0, 8}) == 511, "strtol: base 8, null endptr");
  check(call("abs", {static_cast<std::int64_t>(0xFFFFFFFBll)}) == 5 && call("labs", {-9}) == 9, "abs on a 32-bit register value, labs");
  check(call("toupper", {'a'}) == 'A' && call("tolower", {'Q'}) == 'q' && call("isdigit", {'7'}) == 1 && call("isalpha", {'7'}) == 0 && call("isspace", {' '}) == 1, "ctype functions");
  call("srand", {7}); const auto r1 = call("rand", {}); call("srand", {7});
  check(r1 == call("rand", {}) && r1 >= 0 && r1 <= 0x7fff, "rand: seeded and repeatable");

  // Sysmodule stubs are honest stubs.
  sysmodule_reset();
  check(call("sceSysmoduleIsLoaded", {0x80}) != 0, "sysmodule: not loaded before a load call");
  check(call("sceSysmoduleLoadModule", {0x80}) == 0 && call("sceSysmoduleIsLoaded", {0x80}) == 0 && sysmodule_is_loaded(0x80), "sysmodule: load records the id (stub)");
  check(call("sceSysmoduleUnloadModule", {0x80}) == 0 && call("sceSysmoduleUnloadModule", {0x80}) != 0, "sysmodule: unload, then unloading again is an error");

  // Every new libc symbol resolves by NID the way a loader does it.
  const char* names[] = {"memmove", "memcmp", "memchr", "strcpy", "strncpy", "strcat", "strncmp", "strchr", "strrchr", "strstr", "calloc", "realloc", "snprintf", "sprintf", "puts", "putchar", "atoi", "strtol", "abs", "labs", "toupper", "tolower", "isdigit", "isalpha", "isspace", "srand", "rand", "exit"};
  bool all = true;
  for (const char* n : names) {
    const std::string nid = pnr::loader::nid::compute_nid(n, "libSceLibcInternal");
    std::function<std::int64_t(const pnr::prx::GuestCallArgs&)> fn; std::string err;
    all = all && reg.bind(nid, &fn, &err) == SCE_OK;
  }
  check(all, "bind: every C3 libc symbol binds by NID");

  // Machine-code module through the thunks.
  const std::string dir = "/tmp/pnr_libc_" + std::to_string(::getpid());
  if (std::system(("mkdir -p " + dir).c_str()) != 0) return;
  { auto b = pnr::tools::build_synthetic_libc_elf(); std::ofstream f(dir + "/l.elf", std::ios::binary); f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size())); }
  pnr::loader::ModuleLoadReport rep; pnr::loader::ModuleRunResult rr;
  std::fflush(stdout);
  auto st = pnr::loader::run_module(dir + "/l.elf", reg, &rr, &rep);
  check(st.ok && rep.bound.size() == 7 && rr.finished && !rr.signaled && rr.exit_code == 0, "module: snprintf, strcmp, strtol, calloc, memmove, strlen and printf all work from machine code via NID");

  // S35 review follow-up: fixed-signature FP calls from machine code. The module checks, in
  // guest code with exact bit expectations: printf with 8 doubles filling xmm0-7; fmod's double
  // return in xmm0; fmaxf float32 decoding with garbage in the upper 32 bits of both lanes;
  // ldexp's mixed FP+GP argument pools; all returning 0 only when every check passes.
  {
    for (const char* n : {"fmod", "fmaxf", "ldexp"}) {
      const std::string nid = pnr::loader::nid::compute_nid(n, "libSceLibcInternal");
      std::function<std::int64_t(const pnr::prx::GuestCallArgs&)> fn; std::string err;
      check(reg.bind(nid, &fn, &err) == SCE_OK, std::string("bind: ") + n + " binds by NID");
    }
    { auto b = pnr::tools::build_synthetic_fp_elf(); std::ofstream f(dir + "/fp.elf", std::ios::binary); f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size())); }
    pnr::loader::ModuleLoadReport frep; pnr::loader::ModuleRunResult frr;
    std::fflush(stdout);
    auto fst = pnr::loader::run_module(dir + "/fp.elf", reg, &frr, &frep);
    check(fst.ok && frep.bound.size() == 4 && frr.finished && !frr.signaled && frr.exit_code == 0,
          "fp module: printf x8, fmod, fmaxf (float32 lanes) and ldexp (mixed pools) all work from machine code via NID");
  }
  if (std::system(("rm -rf " + dir).c_str()) != 0) {}
}
