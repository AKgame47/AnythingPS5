#include "prx/libc_abi.hpp"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>

#include "loader/nid.hpp"

namespace pnr::prx {

namespace {
using Args = const std::vector<std::int64_t>&;
const char* S(std::int64_t v) { return reinterpret_cast<const char*>(v); }
char* M(std::int64_t v) { return reinterpret_cast<char*>(v); }
std::int64_t R(const void* p) { return reinterpret_cast<std::int64_t>(p); }

std::mutex g_sys_mu;
std::set<std::int64_t>& sys_set() { static std::set<std::int64_t> s; return s; }
std::uint32_t g_rand = 1;
constexpr std::int64_t kSceEinval = SCE_KERNEL_ERROR_EINVAL;
}  // namespace

std::string format_c(const char* fmt, const std::vector<std::int64_t>& args) {
  std::string out;
  std::size_t ai = 0;
  if (!fmt) return out;
  for (const char* p = fmt; *p; ++p) {
    if (*p != '%') { out.push_back(*p); continue; }
    const char* start = p;
    ++p;
    bool left = false, zero = false;
    for (;; ++p) { if (*p == '-') left = true; else if (*p == '0') zero = true; else break; }
    int width = 0;
    while (*p >= '0' && *p <= '9') { width = std::min(width * 10 + (*p - '0'), 512); ++p; }
    int len = 0;  // 0 int, 1 long (l, ll, z, j, t), -1 short, -2 char
    if (*p == 'h') { len = -1; ++p; if (*p == 'h') { len = -2; ++p; } }
    else if (*p == 'l') { len = 1; ++p; if (*p == 'l') ++p; }
    else if (*p == 'z' || *p == 'j' || *p == 't') { len = 1; ++p; }
    const char c = *p;
    if (c == '\0') { out.append(start, p); break; }
    if (c == '%') { out.push_back('%'); continue; }
    std::string body;
    bool ok = true;
    auto next = [&](std::int64_t* v) { if (ai < args.size()) { *v = args[ai++]; return true; } return false; };
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

bool sysmodule_is_loaded(std::int64_t id) { std::lock_guard<std::mutex> l(g_sys_mu); return sys_set().count(id) != 0; }
void sysmodule_reset() { std::lock_guard<std::mutex> l(g_sys_mu); sys_set().clear(); }

void add_libc_abi_symbols(std::vector<PrxSymbol>& out) {
  auto add = [&](const char* lib, const char* name, std::function<std::int64_t(Args)> fn) {
    out.push_back(PrxSymbol{lib, name, loader::nid::compute_nid(name, lib), true, std::move(fn)});
  };
  const char* L = "libSceLibcInternal";
  auto need = [](Args a, std::size_t n) { return a.size() >= n; };

  add(L, "memmove", [need](Args a) { return need(a, 3) && a[0] && a[1] ? R(std::memmove(M(a[0]), S(a[1]), static_cast<std::size_t>(a[2]))) : 0; });
  add(L, "memcmp", [need](Args a) -> std::int64_t { return need(a, 3) && a[0] && a[1] ? std::memcmp(S(a[0]), S(a[1]), static_cast<std::size_t>(a[2])) : 0; });
  add(L, "memchr", [need](Args a) { return need(a, 3) && a[0] ? R(std::memchr(S(a[0]), static_cast<int>(a[1]), static_cast<std::size_t>(a[2]))) : 0; });
  add(L, "strcpy", [need](Args a) { return need(a, 2) && a[0] && a[1] ? R(std::strcpy(M(a[0]), S(a[1]))) : 0; });
  add(L, "strncpy", [need](Args a) { return need(a, 3) && a[0] && a[1] ? R(std::strncpy(M(a[0]), S(a[1]), static_cast<std::size_t>(a[2]))) : 0; });
  add(L, "strcat", [need](Args a) { return need(a, 2) && a[0] && a[1] ? R(std::strcat(M(a[0]), S(a[1]))) : 0; });
  add(L, "strncmp", [need](Args a) -> std::int64_t { return need(a, 3) && a[0] && a[1] ? std::strncmp(S(a[0]), S(a[1]), static_cast<std::size_t>(a[2])) : 0; });
  add(L, "strchr", [need](Args a) { return need(a, 2) && a[0] ? R(std::strchr(S(a[0]), static_cast<int>(a[1]))) : 0; });
  add(L, "strrchr", [need](Args a) { return need(a, 2) && a[0] ? R(std::strrchr(S(a[0]), static_cast<int>(a[1]))) : 0; });
  add(L, "strstr", [need](Args a) { return need(a, 2) && a[0] && a[1] ? R(std::strstr(S(a[0]), S(a[1]))) : 0; });
  add(L, "calloc", [need](Args a) { return need(a, 2) ? R(std::calloc(static_cast<std::size_t>(a[0]), static_cast<std::size_t>(a[1]))) : 0; });
  add(L, "realloc", [need](Args a) { return need(a, 2) ? R(std::realloc(M(a[0]), static_cast<std::size_t>(a[1]))) : 0; });
  add(L, "snprintf", [need](Args a) -> std::int64_t {
    if (!need(a, 3)) return -1;
    const std::string s = format_c(S(a[2]), {a.begin() + 3, a.end()});
    const std::size_t n = static_cast<std::size_t>(a[1]);
    if (a[0] && n > 0) { const std::size_t k = std::min(s.size(), n - 1); std::memcpy(M(a[0]), s.data(), k); M(a[0])[k] = '\0'; }
    return static_cast<std::int64_t>(s.size());
  });
  add(L, "sprintf", [need](Args a) -> std::int64_t {
    if (!need(a, 2) || !a[0]) return -1;
    std::string s = format_c(S(a[1]), {a.begin() + 2, a.end()});
    if (s.size() > 4095) s.resize(4095);
    std::memcpy(M(a[0]), s.c_str(), s.size() + 1);
    return static_cast<std::int64_t>(s.size());
  });
  add(L, "puts", [need](Args a) -> std::int64_t { return need(a, 1) && a[0] ? std::fputs(S(a[0]), stdout) >= 0 && std::fputc('\n', stdout) >= 0 ? 1 : -1 : -1; });
  add(L, "putchar", [need](Args a) -> std::int64_t { return need(a, 1) ? std::fputc(static_cast<int>(a[0]) & 0xff, stdout) : -1; });
  add(L, "atoi", [need](Args a) -> std::int64_t { return need(a, 1) && a[0] ? std::atoi(S(a[0])) : 0; });
  add(L, "strtol", [need](Args a) -> std::int64_t {
    if (!need(a, 3) || !a[0]) return 0;
    char* end = nullptr;
    const long v = std::strtol(S(a[0]), &end, static_cast<int>(a[2]));
    if (a[1]) *reinterpret_cast<char**>(a[1]) = end;
    return v;
  });
  add(L, "abs", [need](Args a) -> std::int64_t { const auto v = need(a, 1) ? static_cast<std::int32_t>(a[0]) : 0; return v < 0 ? -static_cast<std::int64_t>(v) : v; });
  add(L, "labs", [need](Args a) -> std::int64_t { const auto v = need(a, 1) ? a[0] : 0; return v < 0 ? -v : v; });
  add(L, "toupper", [need](Args a) -> std::int64_t { return need(a, 1) ? std::toupper(static_cast<int>(a[0]) & 0xff) : 0; });
  add(L, "tolower", [need](Args a) -> std::int64_t { return need(a, 1) ? std::tolower(static_cast<int>(a[0]) & 0xff) : 0; });
  add(L, "isdigit", [need](Args a) -> std::int64_t { return need(a, 1) && std::isdigit(static_cast<int>(a[0]) & 0xff) ? 1 : 0; });
  add(L, "isalpha", [need](Args a) -> std::int64_t { return need(a, 1) && std::isalpha(static_cast<int>(a[0]) & 0xff) ? 1 : 0; });
  add(L, "isspace", [need](Args a) -> std::int64_t { return need(a, 1) && std::isspace(static_cast<int>(a[0]) & 0xff) ? 1 : 0; });
  add(L, "srand", [need](Args a) -> std::int64_t { if (need(a, 1)) g_rand = static_cast<std::uint32_t>(a[0]); return 0; });
  add(L, "rand", [](Args) -> std::int64_t { g_rand = g_rand * 1103515245u + 12345u; return (g_rand >> 16) & 0x7fff; });
  add(L, "exit", [need](Args a) -> std::int64_t {
    std::fflush(stdout); std::fflush(stderr);
    ::_exit(need(a, 1) ? static_cast<int>(a[0]) & 0xff : 0);
    return 0;
  });

  // libSceSysmodule: STUBS. Ids are recorded, nothing is loaded.
  const char* Y = "libSceSysmodule";
  add(Y, "sceSysmoduleLoadModule", [need](Args a) -> std::int64_t {
    if (!need(a, 1)) return kSceEinval;
    std::lock_guard<std::mutex> l(g_sys_mu); sys_set().insert(a[0]); return SCE_OK;
  });
  add(Y, "sceSysmoduleIsLoaded", [need](Args a) -> std::int64_t {
    if (!need(a, 1)) return kSceEinval;
    return sysmodule_is_loaded(a[0]) ? SCE_OK : SCE_KERNEL_ERROR_UNKNOWN;
  });
  add(Y, "sceSysmoduleUnloadModule", [need](Args a) -> std::int64_t {
    if (!need(a, 1)) return kSceEinval;
    std::lock_guard<std::mutex> l(g_sys_mu); return sys_set().erase(a[0]) ? SCE_OK : SCE_KERNEL_ERROR_UNKNOWN;
  });
}

}  // namespace pnr::prx
