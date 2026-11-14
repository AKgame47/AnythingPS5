// Implementations are original PNR code over MODULE 03 compat services.
// Taxonomy (which symbols exist, library grouping, NID binding concept)
// follows AnyPS5 (GPL-2.0) core/libs/prx - see prx.hpp header note.
#include "prx/prx.hpp"
#include "loader/guest_runtime.hpp"

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#include <sys/mman.h>
#include <unistd.h>

#include "loader/nid.hpp"
#include "prx/prx_file.hpp"

namespace pnr::prx {

SceInt32 sceKernelUsleep(std::uint32_t microseconds) {
  if (microseconds > 60u * 1000u * 1000u) return SCE_KERNEL_ERROR_EINVAL;
  std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
  return SCE_OK;
}

void* sceKernelAlloc(std::size_t size) {
  if (size == 0) return nullptr;
  return std::malloc(size);  // host-backed; compat MemoryService accounting
                             // hooks in when the runtime wires it (Planned)
}

SceInt32 sceKernelFree(void* ptr) {
  std::free(ptr);
  return SCE_OK;
}

SceInt32 sceKernelSleep(std::uint32_t seconds) {
  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  return SCE_OK;
}

SceInt32 sceKernelCreateThread(const char* name, void (*entry)(void*), void* arg) {
  if (entry == nullptr) return SCE_KERNEL_ERROR_EINVAL;
  try {
    std::thread t([entry, arg] { entry(arg); });
    t.detach();
  } catch (...) {
    return SCE_KERNEL_ERROR_UNKNOWN;
  }
  (void)name;  // thread naming is host-specific; recorded as Planned
  return SCE_OK;
}


namespace {

struct Mapping { void* start; std::size_t size; std::int64_t prot; };
std::mutex g_map_mu;
std::vector<Mapping>& mappings() { static std::vector<Mapping> m; return m; }

Mapping* find_mapping(void* addr) {
  for (auto& m : mappings())
    if (addr >= m.start && addr < static_cast<char*>(m.start) + m.size)
      return &m;
  return nullptr;
}

bool prot_valid(std::int64_t prot) {
  return prot >= 0 && prot <= (SCE_KERNEL_PROT_READ | SCE_KERNEL_PROT_WRITE |
                               SCE_KERNEL_PROT_EXEC);
}

}  // namespace

void* sceKernelMapMemory(std::size_t size, std::int64_t prot) {
  if (size == 0 || !prot_valid(prot)) return nullptr;
  void* p = ::mmap(nullptr, size, static_cast<int>(prot),
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) return nullptr;
  std::lock_guard<std::mutex> lk(g_map_mu);
  mappings().push_back(Mapping{p, size, prot});
  return p;
}

SceInt32 sceKernelMunmap(void* addr, std::size_t size) {
  if (addr == nullptr || size == 0) return SCE_KERNEL_ERROR_EINVAL;
  std::lock_guard<std::mutex> lk(g_map_mu);
  Mapping* m = find_mapping(addr);
  if (m == nullptr) return SCE_KERNEL_ERROR_EINVAL;  // not ours: honest
  if (size > m->size) return SCE_KERNEL_ERROR_EINVAL;
  if (::munmap(addr, size) != 0) return SCE_KERNEL_ERROR_UNKNOWN;
  if (addr == m->start && size == m->size) {
    *m = mappings().back();
    mappings().pop_back();
  } else if (addr == m->start) {
    m->start = static_cast<char*>(m->start) + size;
    m->size -= size;
  } else if (static_cast<char*>(addr) + size ==
             static_cast<char*>(m->start) + m->size) {
    m->size -= size;
  } else {
    return SCE_KERNEL_ERROR_EINVAL;  // middle-split unmap: not supported yet
  }
  return SCE_OK;
}

SceInt32 sceKernelMTypeprotect(void* addr, std::size_t size, std::int64_t prot) {
  if (addr == nullptr || size == 0 || !prot_valid(prot))
    return SCE_KERNEL_ERROR_EINVAL;
  std::lock_guard<std::mutex> lk(g_map_mu);
  Mapping* m = find_mapping(addr);
  if (m == nullptr) return SCE_KERNEL_ERROR_EINVAL;
  if (::mprotect(addr, size, static_cast<int>(prot)) != 0)
    return SCE_KERNEL_ERROR_UNKNOWN;
  if (addr == m->start && size >= m->size) m->prot = prot;
  return SCE_OK;
}

SceInt32 sceKernelQueryMemoryProtection(void* addr, void** start, void** end,
                                        std::int64_t* prot) {
  if (addr == nullptr) return SCE_KERNEL_ERROR_EINVAL;
  std::lock_guard<std::mutex> lk(g_map_mu);
  Mapping* m = find_mapping(addr);
  if (m == nullptr) return SCE_KERNEL_ERROR_EINVAL;  // unknown range: honest
  if (start) *start = m->start;
  if (end) *end = static_cast<char*>(m->start) + m->size;
  if (prot) *prot = m->prot;
  return SCE_OK;
}

namespace {

// Bounded printf core: returns the formatted text (testable), printf wrapper
// writes it to stdout. Supported: %d %i %u %x %s %c %%. Others stay literal.
std::string format_bounded(const char* fmt, const std::vector<std::int64_t>& args) {
  std::string out;
  std::size_t ai = 0;
  for (const char* p = fmt; p != nullptr && *p != '\0'; ++p) {
    if (*p != '%') { out.push_back(*p); continue; }
    ++p;
    if (*p == '\0') { out.push_back('%'); break; }
    switch (*p) {
      case '%': out.push_back('%'); break;
      case 'c':
        if (ai < args.size()) out.push_back(static_cast<char>(args[ai++]));
        break;
      case 's': {
        if (ai < args.size()) {
          const char* sp = reinterpret_cast<const char*>(args[ai++]);
          if (sp != nullptr) out += sp; else out += "(null)";
        }
        break;
      }
      case 'd': case 'i':
        if (ai < args.size()) out += std::to_string(static_cast<long long>(args[ai++]));
        break;
      case 'u':
        if (ai < args.size()) out += std::to_string(static_cast<unsigned long long>(args[ai++]));
        break;
      case 'x': {
        if (ai < args.size()) {
          char buf[24];
          std::snprintf(buf, sizeof buf, "%llx", static_cast<unsigned long long>(args[ai++]));
          out += buf;
        }
        break;
      }
      default:  // unsupported specifier: honest literal passthrough
        out.push_back('%');
        out.push_back(*p);
        break;
    }
  }
  return out;
}

}  // namespace

void* sce_libc_memcpy(void* dst, const void* src, std::size_t n) {
  if (dst == nullptr || src == nullptr) return nullptr;
  return std::memcpy(dst, src, n);
}
void* sce_libc_memset(void* dst, int c, std::size_t n) {
  if (dst == nullptr) return nullptr;
  return std::memset(dst, c, n);
}
std::size_t sce_libc_strlen(const char* s) {
  return s == nullptr ? 0 : std::strlen(s);
}
int sce_libc_strcmp(const char* a, const char* b) {
  if (a == nullptr || b == nullptr) return (a == b) ? 0 : (a == nullptr ? -1 : 1);
  return std::strcmp(a, b);
}
void* sce_libc_malloc(std::size_t size) {
  if (size == 0) return nullptr;
  return std::malloc(size);
}
void sce_libc_free(void* ptr) { std::free(ptr); }
int sce_libc_printf(const char* fmt, const std::vector<std::int64_t>& args) {
  const std::string out = format_bounded(fmt, args);
  std::fputs(out.c_str(), stdout);
  return static_cast<int>(out.size());
}

namespace {
PrxSymbol decl_only(const char* lib, const char* name) {
  return PrxSymbol{lib, name, loader::nid::compute_nid(name, lib), false, nullptr};
}
PrxSymbol impl(const char* lib, const char* name,
               std::function<std::int64_t(const std::vector<std::int64_t>&)> fn) {
  return PrxSymbol{lib, name, loader::nid::compute_nid(name, lib), true, std::move(fn)};
}
}  // namespace

PrxRegistry::PrxRegistry() {
  // Implemented libkernel subset (matches the loader seed-table symbols).
  symbols_.push_back(impl("libkernel", "sceKernelUsleep",
      [](const std::vector<std::int64_t>& a) -> std::int64_t {
        return a.empty() ? SCE_KERNEL_ERROR_EINVAL
                         : sceKernelUsleep(static_cast<std::uint32_t>(a[0]));
      }));
  symbols_.push_back(impl("libkernel", "sceKernelSleep",
      [](const std::vector<std::int64_t>& a) -> std::int64_t {
        return a.empty() ? SCE_KERNEL_ERROR_EINVAL
                         : sceKernelSleep(static_cast<std::uint32_t>(a[0]));
      }));
  symbols_.push_back(impl("libkernel", "sceKernelAlloc",
      [](const std::vector<std::int64_t>& a) -> std::int64_t {
        if (a.empty()) return SCE_KERNEL_ERROR_EINVAL;
        return reinterpret_cast<std::int64_t>(
            sceKernelAlloc(static_cast<std::size_t>(a[0])));
      }));
  symbols_.push_back(impl("libkernel", "sceKernelFree",
      [](const std::vector<std::int64_t>& a) -> std::int64_t {
        return a.empty() ? SCE_KERNEL_ERROR_EINVAL
                         : sceKernelFree(reinterpret_cast<void*>(a[0]));
      }));
  symbols_.push_back(impl("libkernel", "sceKernelCreateThread",
      [](const std::vector<std::int64_t>& a) -> std::int64_t {
        if (a.size() < 2) return SCE_KERNEL_ERROR_EINVAL;
        auto* entry = reinterpret_cast<void (*)(void*)>(a[0]);
        void* arg = a.size() > 1 ? reinterpret_cast<void*>(a[1]) : nullptr;
        return sceKernelCreateThread("pnr-prx-thread", entry, arg);
      }));
  // C2: libkernel ABI subset (Sony constants translated in loader/guest_runtime).
  using V = const std::vector<std::int64_t>&;
  symbols_.push_back(impl("libkernel", "mmap", [](V a) -> std::int64_t {
    if (a.size() < 6) return -1;
    return loader::sony_mmap(a[0], a[1], a[2], a[3], a[4], a[5]);
  }));
  symbols_.push_back(impl("libkernel", "sceKernelClockGettime", [](V a) -> std::int64_t {
    return a.size() < 2 ? SCE_KERNEL_ERROR_EINVAL : loader::sony_clock_gettime(a[0], a[1]);
  }));
  symbols_.push_back(impl("libkernel", "scePthreadMutexInit", [](V a) -> std::int64_t {
    return a.empty() ? SCE_KERNEL_ERROR_EINVAL : loader::guest_mutex_op(0, reinterpret_cast<std::int64_t*>(a[0]));
  }));
  symbols_.push_back(impl("libkernel", "scePthreadMutexLock", [](V a) -> std::int64_t {
    return a.empty() ? SCE_KERNEL_ERROR_EINVAL : loader::guest_mutex_op(1, reinterpret_cast<std::int64_t*>(a[0]));
  }));
  symbols_.push_back(impl("libkernel", "scePthreadMutexUnlock", [](V a) -> std::int64_t {
    return a.empty() ? SCE_KERNEL_ERROR_EINVAL : loader::guest_mutex_op(2, reinterpret_cast<std::int64_t*>(a[0]));
  }));
  symbols_.push_back(impl("libkernel", "scePthreadMutexDestroy", [](V a) -> std::int64_t {
    return a.empty() ? SCE_KERNEL_ERROR_EINVAL : loader::guest_mutex_op(3, reinterpret_cast<std::int64_t*>(a[0]));
  }));
  symbols_.push_back(impl("libkernel", "scePthreadCreate", [](V a) -> std::int64_t {
    if (a.size() < 4) return SCE_KERNEL_ERROR_EINVAL;
    return loader::guest_thread_create(reinterpret_cast<std::int64_t*>(a[0]), a[2], a[3]);
  }));
  symbols_.push_back(impl("libkernel", "scePthreadJoin", [](V a) -> std::int64_t {
    if (a.size() < 2) return SCE_KERNEL_ERROR_EINVAL;
    return loader::guest_thread_join(a[0], reinterpret_cast<std::int64_t*>(a[1]));
  }));
  symbols_.push_back(impl("libkernel", "scePthreadSelf", [](V) -> std::int64_t { return loader::guest_thread_self(); }));
  // Declared-only taxonomy entries (honest: no host implementation yet).
  symbols_.push_back(decl_only("libkernel", "sceKernelOpen"));
  symbols_.push_back(decl_only("libkernel", "sceKernelRead"));
  symbols_.push_back(decl_only("libkernel", "sceKernelWrite"));
  symbols_.push_back(decl_only("libkernel", "sceKernelClose"));
  symbols_.push_back(decl_only("libkernel", "sceKernelLseek"));
  symbols_.push_back(decl_only("libkernel", "sceKernelFstat"));
  symbols_.push_back(decl_only("libkernel", "sceKernelReserveVirtualRange"));
  symbols_.push_back(decl_only("libkernel", "sceKernelAvailableFlexibleMemorySize"));
  // libkernel memory management (over POSIX mmap/mprotect/munmap).
  symbols_.push_back(impl("libkernel", "sceKernelMapMemory",
      [](const std::vector<std::int64_t>& a) -> std::int64_t {
        if (a.size() < 2) return SCE_KERNEL_ERROR_EINVAL;
        return reinterpret_cast<std::int64_t>(sceKernelMapMemory(
            static_cast<std::size_t>(a[0]), a[1]));
      }));
  symbols_.push_back(impl("libkernel", "sceKernelMunmap",
      [](const std::vector<std::int64_t>& a) -> std::int64_t {
        if (a.size() < 2) return SCE_KERNEL_ERROR_EINVAL;
        return sceKernelMunmap(reinterpret_cast<void*>(a[0]),
                               static_cast<std::size_t>(a[1]));
      }));
  symbols_.push_back(impl("libkernel", "sceKernelMTypeprotect",
      [](const std::vector<std::int64_t>& a) -> std::int64_t {
        if (a.size() < 3) return SCE_KERNEL_ERROR_EINVAL;
        return sceKernelMTypeprotect(reinterpret_cast<void*>(a[0]),
                                     static_cast<std::size_t>(a[1]), a[2]);
      }));
  symbols_.push_back(impl("libkernel", "sceKernelQueryMemoryProtection",
      [](const std::vector<std::int64_t>& a) -> std::int64_t {
        if (a.empty()) return SCE_KERNEL_ERROR_EINVAL;
        void* s = nullptr; void* e = nullptr; std::int64_t prot = 0;
        const auto rc = sceKernelQueryMemoryProtection(
            reinterpret_cast<void*>(a[0]), &s, &e, &prot);
        if (rc == SCE_OK && a.size() >= 4) {
          *reinterpret_cast<void**>(a[1]) = s;
          *reinterpret_cast<void**>(a[2]) = e;
          *reinterpret_cast<std::int64_t*>(a[3]) = prot;
        }
        return rc;
      }));
  // libSceLibcInternal subset, implemented over host equivalents.
  symbols_.push_back(impl("libSceLibcInternal", "printf",
      [](const std::vector<std::int64_t>& a) -> std::int64_t {
        if (a.empty()) return SCE_KERNEL_ERROR_EINVAL;
        const auto* fmt = reinterpret_cast<const char*>(a[0]);
        return sce_libc_printf(fmt, {a.begin() + 1, a.end()});
      }));
  symbols_.push_back(impl("libSceLibcInternal", "malloc",
      [](const std::vector<std::int64_t>& a) -> std::int64_t {
        if (a.empty()) return SCE_KERNEL_ERROR_EINVAL;
        return reinterpret_cast<std::int64_t>(sce_libc_malloc(static_cast<std::size_t>(a[0])));
      }));
  symbols_.push_back(impl("libSceLibcInternal", "free",
      [](const std::vector<std::int64_t>& a) -> std::int64_t {
        if (a.empty()) return SCE_KERNEL_ERROR_EINVAL;
        sce_libc_free(reinterpret_cast<void*>(a[0]));
        return SCE_OK;
      }));
  symbols_.push_back(impl("libSceLibcInternal", "memcpy",
      [](const std::vector<std::int64_t>& a) -> std::int64_t {
        if (a.size() < 3) return SCE_KERNEL_ERROR_EINVAL;
        return reinterpret_cast<std::int64_t>(sce_libc_memcpy(
            reinterpret_cast<void*>(a[0]), reinterpret_cast<const void*>(a[1]),
            static_cast<std::size_t>(a[2])));
      }));
  symbols_.push_back(impl("libSceLibcInternal", "memset",
      [](const std::vector<std::int64_t>& a) -> std::int64_t {
        if (a.size() < 3) return SCE_KERNEL_ERROR_EINVAL;
        return reinterpret_cast<std::int64_t>(sce_libc_memset(
            reinterpret_cast<void*>(a[0]), static_cast<int>(a[1]),
            static_cast<std::size_t>(a[2])));
      }));
  symbols_.push_back(impl("libSceLibcInternal", "strlen",
      [](const std::vector<std::int64_t>& a) -> std::int64_t {
        if (a.empty()) return SCE_KERNEL_ERROR_EINVAL;
        return static_cast<std::int64_t>(
            sce_libc_strlen(reinterpret_cast<const char*>(a[0])));
      }));
  symbols_.push_back(impl("libSceLibcInternal", "strcmp",
      [](const std::vector<std::int64_t>& a) -> std::int64_t {
        if (a.size() < 2) return SCE_KERNEL_ERROR_EINVAL;
        return sce_libc_strcmp(reinterpret_cast<const char*>(a[0]),
                               reinterpret_cast<const char*>(a[1]));
      }));
}

void PrxRegistry::install_file_calls(PsFileTable* t) {
  if (t == nullptr) return;
  auto set_impl = [this, t](const char* name,
      std::function<std::int64_t(const std::vector<std::int64_t>&)> fn) {
    for (auto& s : symbols_)
      if (s.name == name) { s.implemented = true; s.invoke = std::move(fn); }
  };
  set_impl("sceKernelOpen", [t](const std::vector<std::int64_t>& a) -> std::int64_t {
    if (a.size() < 2) return SCE_KERNEL_ERROR_EINVAL;
    return t->open(reinterpret_cast<const char*>(a[0]), a[1]);
  });
  set_impl("sceKernelRead", [t](const std::vector<std::int64_t>& a) -> std::int64_t {
    if (a.size() < 3) return SCE_KERNEL_ERROR_EINVAL;
    return t->read(a[0], reinterpret_cast<void*>(a[1]),
                   static_cast<std::size_t>(a[2]));
  });
  set_impl("sceKernelWrite", [t](const std::vector<std::int64_t>& a) -> std::int64_t {
    if (a.size() < 3) return SCE_KERNEL_ERROR_EINVAL;
    return t->write(a[0], reinterpret_cast<const void*>(a[1]),
                    static_cast<std::size_t>(a[2]));
  });
  set_impl("sceKernelClose", [t](const std::vector<std::int64_t>& a) -> std::int64_t {
    if (a.empty()) return SCE_KERNEL_ERROR_EINVAL;
    return t->close(a[0]);
  });
  set_impl("sceKernelLseek", [t](const std::vector<std::int64_t>& a) -> std::int64_t {
    if (a.size() < 3) return SCE_KERNEL_ERROR_EINVAL;
    return t->lseek(a[0], a[1], a[2]);
  });
  set_impl("sceKernelFstat", [t](const std::vector<std::int64_t>& a) -> std::int64_t {
    if (a.size() < 2) return SCE_KERNEL_ERROR_EINVAL;
    return t->fstat(a[0], reinterpret_cast<SceKernelStat*>(a[1]));
  });
}

const PrxSymbol* PrxRegistry::find(const std::string& nid) const {
  for (const auto& s : symbols_)
    if (s.nid == nid) return &s;
  return nullptr;
}

const PrxSymbol* PrxRegistry::find_name(const std::string& name) const {
  for (const auto& s : symbols_)
    if (s.name == name) return &s;
  return nullptr;
}

SceInt32 PrxRegistry::bind(
    const std::string& nid,
    std::function<std::int64_t(const std::vector<std::int64_t>&)>* fn,
    std::string* error) const {
  const PrxSymbol* s = find(nid);
  if (s == nullptr) {
    if (error) *error = "NID not in PRX taxonomy: " + nid;
    return SCE_KERNEL_ERROR_UNKNOWN;
  }
  if (!s->implemented) {
    if (error)
      *error = "symbol declared but not yet implemented: " + s->library + ":" + s->name;
    return SCE_KERNEL_ERROR_EINVAL;
  }
  *fn = s->invoke;
  return SCE_OK;
}

}  // namespace pnr::prx
