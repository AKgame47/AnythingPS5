#include "prx/libc_abi.hpp"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>

#include "loader/guest_runtime.hpp"
#include "loader/nid.hpp"

namespace pnr::prx {

namespace {
using Args = const GuestCallArgs&;
const char* S(std::int64_t v) { return reinterpret_cast<const char*>(v); }
char* M(std::int64_t v) { return reinterpret_cast<char*>(v); }
std::int64_t R(const void* p) { return reinterpret_cast<std::int64_t>(p); }

std::mutex g_sys_mu;
std::set<std::int64_t>& sys_set() { static std::set<std::int64_t> s; return s; }
std::uint32_t g_rand = 1;
constexpr std::int64_t kSceEinval = SCE_KERNEL_ERROR_EINVAL;
// Slice S34: qsort's guest comparator (thread-local so nested/concurrent sorts do not race)
// and the __cxa_guard_* lock.
thread_local std::uint64_t s_qsort_cmp = 0;
int qsort_guest_cmp(const void* x, const void* y) {
  return static_cast<int>(pnr::loader::call_guest_function(s_qsort_cmp, reinterpret_cast<std::int64_t>(x),
                                                           reinterpret_cast<std::int64_t>(y)));
}
std::mutex& guard_mu() { static std::mutex m; return m; }
std::condition_variable& guard_cv() { static std::condition_variable c; return c; }
}  // namespace

namespace {
// Slice S35: the formatter core works over the two SysV argument pools the thunk captures:
// up to 6 general-purpose slots (gp) and up to 8 xmm slots (fp). Conversions d i u x X o c s p
// consume gp in order; f F e E g G a A consume fp in order (the System V varargs rule: FP
// arguments travel in xmm registers, independently of the gp sequence).
// Slice S36: stk/nstk is the raw stack-spill window (GuestCallArgs::s). Overflow arguments
// sit there in argument order regardless of class, and conversions also consume them in
// argument order, so a single shared cursor serves both pools: when a conversion's own pool
// is exhausted it takes the next stack slot.
std::string format_c_pools(const char* fmt, const std::int64_t* gp, std::size_t ngp,
                           const double* fp, std::size_t nfp,
                           const std::int64_t* stk = nullptr, std::size_t nstk = 0) {
  std::string out;
  std::size_t ai = 0, fi = 0, si = 0;
  if (!fmt) return out;
  for (const char* p = fmt; *p; ++p) {
    if (*p != '%') { out.push_back(*p); continue; }
    const char* start = p;
    ++p;
    bool left = false, zero = false;
    for (;; ++p) { if (*p == '-') left = true; else if (*p == '0') zero = true; else break; }
    int width = 0;
    while (*p >= '0' && *p <= '9') { width = std::min(width * 10 + (*p - '0'), 512); ++p; }
    int prec = -1;
    if (*p == '.') { ++p; prec = 0; while (*p >= '0' && *p <= '9') { prec = std::min(prec * 10 + (*p - '0'), 100); ++p; } }
    int len = 0;  // 0 int, 1 long (l, ll, z, j, t), -1 short, -2 char
    if (*p == 'h') { len = -1; ++p; if (*p == 'h') { len = -2; ++p; } }
    else if (*p == 'l') { len = 1; ++p; if (*p == 'l') ++p; }
    else if (*p == 'z' || *p == 'j' || *p == 't') { len = 1; ++p; }
    else if (*p == 'L') { len = 2; ++p; }
    const char c = *p;
    if (c == '\0') { out.append(start, p); break; }
    if (c == '%') { out.push_back('%'); continue; }
    std::string body;
    bool ok = true;
    auto next = [&](std::int64_t* v) { if (ai < ngp) { *v = gp[ai++]; return true; } if (si < nstk) { *v = stk[si++]; return true; } return false; };
    auto nextf = [&](double* v) { if (fi < nfp) { *v = fp[fi++]; return true; } if (si < nstk) { std::memcpy(v, &stk[si++], 8); return true; } return false; };
    if (c == 'f' || c == 'F' || c == 'e' || c == 'E' || c == 'g' || c == 'G' || c == 'a' || c == 'A') {
      // Floating point conversion: long double (L) is refused (stays literal); l is plain double.
      double dv = 0;
      if (len != 2 && nextf(&dv)) {
        // Rebuild the spec with the parsed (capped) flags/width/precision and let the host libc
        // produce correctly rounded output.
        std::string spec = "%";
        if (left) spec.push_back('-');
        if (zero) spec.push_back('0');
        if (width) spec += std::to_string(width);
        if (prec >= 0) { spec.push_back('.'); spec += std::to_string(prec); }
        spec.push_back(c);
        char buf[800];
        std::snprintf(buf, sizeof buf, spec.c_str(), dv);
        body = buf;
      } else { ok = false; }
      out += ok ? body : std::string(start, static_cast<std::size_t>(p - start) + 1);
      continue;
    }
    std::int64_t v = 0;
    switch (c) {
      case 'd': case 'i': {
        if (!next(&v)) { ok = false; break; }
        const long long s = len == 1 ? v : len == -1 ? static_cast<std::int16_t>(v) : len == -2 ? static_cast<std::int8_t>(v) : static_cast<std::int32_t>(v);
        body = std::to_string(s);
        break;
      }
      case 'u': case 'x': case 'X': case 'o': {
        if (!next(&v)) { ok = false; break; }
        const unsigned long long u = len == 1 ? static_cast<std::uint64_t>(v) : len == -1 ? static_cast<std::uint16_t>(v) : len == -2 ? static_cast<std::uint8_t>(v) : static_cast<std::uint32_t>(v);
        char buf[40];
        std::snprintf(buf, sizeof buf, c == 'u' ? "%llu" : c == 'x' ? "%llx" : c == 'X' ? "%llX" : "%llo", u);
        body = buf;
        break;
      }
      case 'p': {
        if (!next(&v)) { ok = false; break; }
        char buf[40]; std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(v)); body = buf;
        break;
      }
      case 'c': if (!next(&v)) { ok = false; break; } body.assign(1, static_cast<char>(v)); zero = false; break;
      case 's': if (!next(&v)) { ok = false; break; } body = v ? S(v) : "(null)"; zero = false; break;
      default: ok = false; break;
    }
    if (!ok) {  // unsupported or missing argument: literal passthrough, honest
      out.append(start, static_cast<std::size_t>(p - start) + 1);
      continue;
    }
    if (static_cast<int>(body.size()) < width) {
      const std::size_t pad = static_cast<std::size_t>(width) - body.size();
      if (left) body.append(pad, ' ');
      else if (zero) {
        const std::size_t signlen = (!body.empty() && body[0] == '-') ? 1 : 0;
        body.insert(signlen, pad, '0');
      } else body.insert(0, pad, ' ');
    }
    out += body;
  }
  return out;
}
}  // namespace

std::string format_c(const char* fmt, const std::vector<std::int64_t>& args) {
  return format_c_pools(fmt, args.data(), args.size(), nullptr, 0);
}

std::string format_c_guest(const char* fmt, const GuestCallArgs& a, std::size_t gp0) {
  const std::size_t n = gp0 < 6 ? 6 - gp0 : 0;
  return format_c_pools(fmt, a.i + gp0, n, a.x, 8, a.s, 8);
}

bool sysmodule_is_loaded(std::int64_t id) { std::lock_guard<std::mutex> l(g_sys_mu); return sys_set().count(id) != 0; }
void sysmodule_reset() { std::lock_guard<std::mutex> l(g_sys_mu); sys_set().clear(); }

void add_libc_abi_symbols(std::vector<PrxSymbol>& out) {
  auto add = [&](const char* lib, const char* name, std::function<std::int64_t(Args)> fn) {
    out.push_back(PrxSymbol{lib, name, loader::nid::compute_nid(name, lib), true, std::move(fn)});
  };
  const char* L = "libSceLibcInternal";
  auto need = [](Args, std::size_t n) { return n <= 6; };

  add(L, "memmove", [need](Args a) { return need(a, 3) && a.i[0] && a.i[1] ? R(std::memmove(M(a.i[0]), S(a.i[1]), static_cast<std::size_t>(a.i[2]))) : 0; });
  add(L, "memcmp", [need](Args a) -> std::int64_t { return need(a, 3) && a.i[0] && a.i[1] ? std::memcmp(S(a.i[0]), S(a.i[1]), static_cast<std::size_t>(a.i[2])) : 0; });
  add(L, "memchr", [need](Args a) { return need(a, 3) && a.i[0] ? R(std::memchr(S(a.i[0]), static_cast<int>(a.i[1]), static_cast<std::size_t>(a.i[2]))) : 0; });
  add(L, "strcpy", [need](Args a) { return need(a, 2) && a.i[0] && a.i[1] ? R(std::strcpy(M(a.i[0]), S(a.i[1]))) : 0; });
  add(L, "strncpy", [need](Args a) { return need(a, 3) && a.i[0] && a.i[1] ? R(std::strncpy(M(a.i[0]), S(a.i[1]), static_cast<std::size_t>(a.i[2]))) : 0; });
  add(L, "strcat", [need](Args a) { return need(a, 2) && a.i[0] && a.i[1] ? R(std::strcat(M(a.i[0]), S(a.i[1]))) : 0; });
  add(L, "strncmp", [need](Args a) -> std::int64_t { return need(a, 3) && a.i[0] && a.i[1] ? std::strncmp(S(a.i[0]), S(a.i[1]), static_cast<std::size_t>(a.i[2])) : 0; });
  add(L, "strchr", [need](Args a) { return need(a, 2) && a.i[0] ? R(std::strchr(S(a.i[0]), static_cast<int>(a.i[1]))) : 0; });
  add(L, "strrchr", [need](Args a) { return need(a, 2) && a.i[0] ? R(std::strrchr(S(a.i[0]), static_cast<int>(a.i[1]))) : 0; });
  add(L, "strstr", [need](Args a) { return need(a, 2) && a.i[0] && a.i[1] ? R(std::strstr(S(a.i[0]), S(a.i[1]))) : 0; });
  add(L, "calloc", [need](Args a) { return need(a, 2) ? R(std::calloc(static_cast<std::size_t>(a.i[0]), static_cast<std::size_t>(a.i[1]))) : 0; });
  add(L, "realloc", [need](Args a) { return need(a, 2) ? R(std::realloc(M(a.i[0]), static_cast<std::size_t>(a.i[1]))) : 0; });
  add(L, "snprintf", [need](Args a) -> std::int64_t {
    if (!need(a, 3)) return -1;
    const std::string s = format_c_guest(S(a.i[2]), a, 3);
    const std::size_t n = static_cast<std::size_t>(a.i[1]);
    if (a.i[0] && n > 0) { const std::size_t k = std::min(s.size(), n - 1); std::memcpy(M(a.i[0]), s.data(), k); M(a.i[0])[k] = '\0'; }
    return static_cast<std::int64_t>(s.size());
  });
  add(L, "sprintf", [need](Args a) -> std::int64_t {
    if (!need(a, 2) || !a.i[0]) return -1;
    std::string s = format_c_guest(S(a.i[1]), a, 2);
    if (s.size() > 4095) s.resize(4095);
    std::memcpy(M(a.i[0]), s.c_str(), s.size() + 1);
    return static_cast<std::int64_t>(s.size());
  });
  add(L, "puts", [need](Args a) -> std::int64_t { return need(a, 1) && a.i[0] ? std::fputs(S(a.i[0]), stdout) >= 0 && std::fputc('\n', stdout) >= 0 ? 1 : -1 : -1; });
  add(L, "putchar", [need](Args a) -> std::int64_t { return need(a, 1) ? std::fputc(static_cast<int>(a.i[0]) & 0xff, stdout) : -1; });
  add(L, "atoi", [need](Args a) -> std::int64_t { return need(a, 1) && a.i[0] ? std::atoi(S(a.i[0])) : 0; });
  add(L, "strtol", [need](Args a) -> std::int64_t {
    if (!need(a, 3) || !a.i[0]) return 0;
    char* end = nullptr;
    const long v = std::strtol(S(a.i[0]), &end, static_cast<int>(a.i[2]));
    if (a.i[1]) *reinterpret_cast<char**>(a.i[1]) = end;
    return v;
  });
  add(L, "abs", [need](Args a) -> std::int64_t { const auto v = need(a, 1) ? static_cast<std::int32_t>(a.i[0]) : 0; return v < 0 ? -static_cast<std::int64_t>(v) : v; });
  add(L, "labs", [need](Args a) -> std::int64_t { const auto v = need(a, 1) ? a.i[0] : 0; return v < 0 ? -v : v; });
  add(L, "toupper", [need](Args a) -> std::int64_t { return need(a, 1) ? std::toupper(static_cast<int>(a.i[0]) & 0xff) : 0; });
  add(L, "tolower", [need](Args a) -> std::int64_t { return need(a, 1) ? std::tolower(static_cast<int>(a.i[0]) & 0xff) : 0; });
  add(L, "isdigit", [need](Args a) -> std::int64_t { return need(a, 1) && std::isdigit(static_cast<int>(a.i[0]) & 0xff) ? 1 : 0; });
  add(L, "isalpha", [need](Args a) -> std::int64_t { return need(a, 1) && std::isalpha(static_cast<int>(a.i[0]) & 0xff) ? 1 : 0; });
  add(L, "isspace", [need](Args a) -> std::int64_t { return need(a, 1) && std::isspace(static_cast<int>(a.i[0]) & 0xff) ? 1 : 0; });
  add(L, "srand", [need](Args a) -> std::int64_t { if (need(a, 1)) g_rand = static_cast<std::uint32_t>(a.i[0]); return 0; });
  add(L, "rand", [](Args) -> std::int64_t { g_rand = g_rand * 1103515245u + 12345u; return (g_rand >> 16) & 0x7fff; });
  // Slice S32: atexit registers a GUEST function pointer; the exit sequence (these handlers
  // LIFO, then the module's DT_FINI_ARRAY reversed and DT_FINI) runs with the guest FS base,
  // both when the guest calls exit() and when its entry point returns.
  add(L, "atexit", [need](Args a) -> std::int64_t {
    return need(a, 1) && a.i[0] ? pnr::loader::guest_atexit_register(static_cast<std::uint64_t>(a.i[0])) : kSceEinval;
  });
  add(L, "exit", [need](Args a) -> std::int64_t {
    pnr::loader::guest_exit_with(need(a, 1) ? static_cast<int>(a.i[0]) & 0xff : 0);
    return 0;
  });
  // Slice S34: C++ ABI + qsort. __cxa_atexit/__cxa_thread_atexit_impl register GUEST function
  // pointers with one argument; the dso argument is accepted and ignored (no per-module unload).
  add(L, "__cxa_atexit", [need](Args a) -> std::int64_t {
    return need(a, 3) && a.i[0] ? pnr::loader::guest_cxa_atexit_register(static_cast<std::uint64_t>(a.i[0]), static_cast<std::uint64_t>(a.i[1]))
                              : kSceEinval;
  });
  add(L, "__cxa_thread_atexit_impl", [need](Args a) -> std::int64_t {
    return need(a, 3) && a.i[0] ? pnr::loader::guest_cxa_thread_atexit_register(static_cast<std::uint64_t>(a.i[0]), static_cast<std::uint64_t>(a.i[1]))
                              : kSceEinval;
  });
  // qsort with a GUEST comparator: the trampoline calls back into guest code through
  // call_guest_function (S32), which switches FS. Pointers are guest addresses = host addresses
  // (same process). The comparator's int result passes through unchanged.
  add(L, "qsort", [need](Args a) -> std::int64_t {
    if (!need(a, 4) || !a.i[3] || (a.i[1] != 0 && (!a.i[0] || !a.i[2]))) return kSceEinval;
    s_qsort_cmp = static_cast<std::uint64_t>(a.i[3]);
    std::qsort(M(a.i[0]), static_cast<std::size_t>(a.i[1]), static_cast<std::size_t>(a.i[2]), &qsort_guest_cmp);
    return 0;
  });
  // __cxa_guard_* for function-local statics (Itanium layout): byte 0 of the guard object is the
  // initialized flag, byte 1 is the initialization-pending flag. A thread that finds the pending
  // flag set BLOCKS until the initializing thread releases or aborts, so one static is never
  // initialized twice. (Recursive acquisition by the initializing thread itself deadlocks, as
  // with glibc; guest code must not do that.)
  // flag (Itanium ABI). Simplified semantics: no blocking wait for an in-progress init on another
  // thread (documented); a plain mutex keeps concurrent checks consistent.
  add(L, "__cxa_guard_acquire", [need](Args a) -> std::int64_t {
    if (!need(a, 1) || !a.i[0]) return kSceEinval;
    auto* g = reinterpret_cast<std::uint8_t*>(M(a.i[0]));
    std::unique_lock<std::mutex> l(guard_mu());
    while ((g[1] & 1) != 0) guard_cv().wait(l);  // another thread is initializing: wait for it
    if (g[0] != 0) return 0;                     // already initialized
    g[1] = 1;                                    // claim the initialization
    return 1;
  });
  add(L, "__cxa_guard_release", [need](Args a) -> std::int64_t {
    if (!need(a, 1) || !a.i[0]) return kSceEinval;
    auto* g = reinterpret_cast<std::uint8_t*>(M(a.i[0]));
    { std::lock_guard<std::mutex> l(guard_mu()); g[0] = 1; g[1] = 0; }
    guard_cv().notify_all();
    return 0;
  });
  add(L, "__cxa_guard_abort", [need](Args a) -> std::int64_t {
    if (!need(a, 1) || !a.i[0]) return kSceEinval;
    auto* g = reinterpret_cast<std::uint8_t*>(M(a.i[0]));
    { std::lock_guard<std::mutex> l(guard_mu()); g[0] = 0; g[1] = 0; }
    guard_cv().notify_all();
    return 0;
  });
  add(L, "__cxa_pure_virtual", [](Args) -> std::int64_t {
    std::fprintf(stderr, "pnr: pure virtual function called in guest module\n");
    ::_exit(70);  // abort semantics: no atexit/fini, matching the host C++ runtime
  });

  // libSceSysmodule: STUBS. Ids are recorded, nothing is loaded.
  const char* Y = "libSceSysmodule";
  add(Y, "sceSysmoduleLoadModule", [need](Args a) -> std::int64_t {
    if (!need(a, 1)) return kSceEinval;
    std::lock_guard<std::mutex> l(g_sys_mu); sys_set().insert(a.i[0]); return SCE_OK;
  });
  add(Y, "sceSysmoduleIsLoaded", [need](Args a) -> std::int64_t {
    if (!need(a, 1)) return kSceEinval;
    return sysmodule_is_loaded(a.i[0]) ? SCE_OK : SCE_KERNEL_ERROR_UNKNOWN;
  });
  add(Y, "sceSysmoduleUnloadModule", [need](Args a) -> std::int64_t {
    if (!need(a, 1)) return kSceEinval;
    std::lock_guard<std::mutex> l(g_sys_mu); return sys_set().erase(a.i[0]) ? SCE_OK : SCE_KERNEL_ERROR_UNKNOWN;
  });
}

}  // namespace pnr::prx
