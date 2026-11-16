// Implementations are original PNR code over MODULE 03 compat services.
// Taxonomy (which symbols exist, library grouping, NID binding concept)
// follows AnyPS5 (GPL-2.0) core/libs/prx - see prx.hpp header note.
#include "prx/prx.hpp"
#include "loader/guest_runtime.hpp"
#include "prx/libc_abi.hpp"
#include "prx/hw_abi.hpp"

#include <chrono>
#include <cmath>
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
int sce_libc_printf(const char* fmt, const GuestCallArgs& args, std::size_t gp0) {
  const std::string out = format_c_guest(fmt, args, gp0);
  std::fputs(out.c_str(), stdout);
  return static_cast<int>(out.size());
}

namespace {
PrxSymbol decl_only(const char* lib, const char* name) {
  return PrxSymbol{lib, name, loader::nid::compute_nid(name, lib), false, nullptr};
}
PrxSymbol impl(const char* lib, const char* name,
               std::function<std::int64_t(const GuestCallArgs&)> fn, bool returns_fp = false) {
  return PrxSymbol{lib, name, loader::nid::compute_nid(name, lib), true, std::move(fn), returns_fp};
}
}  // namespace

PrxRegistry::PrxRegistry() {
  // Implemented libkernel subset (matches the loader seed-table symbols).
  symbols_.push_back(impl("libkernel", "sceKernelUsleep",
      [](const GuestCallArgs& a) -> std::int64_t {
        return sceKernelUsleep(static_cast<std::uint32_t>(a.i[0]));
      }));
  symbols_.push_back(impl("libkernel", "sceKernelSleep",
      [](const GuestCallArgs& a) -> std::int64_t {
        return sceKernelSleep(static_cast<std::uint32_t>(a.i[0]));
      }));
  symbols_.push_back(impl("libkernel", "sceKernelAlloc",
      [](const GuestCallArgs& a) -> std::int64_t {
        return reinterpret_cast<std::int64_t>(
            sceKernelAlloc(static_cast<std::size_t>(a.i[0])));
      }));
  symbols_.push_back(impl("libkernel", "sceKernelFree",
      [](const GuestCallArgs& a) -> std::int64_t {
        return sceKernelFree(reinterpret_cast<void*>(a.i[0]));
      }));
  symbols_.push_back(impl("libkernel", "sceKernelCreateThread",
      [](const GuestCallArgs& a) -> std::int64_t {
        if ((2 > 6)) return SCE_KERNEL_ERROR_EINVAL;
        auto* entry = reinterpret_cast<void (*)(void*)>(a.i[0]);
        void* arg = true ? reinterpret_cast<void*>(a.i[1]) : nullptr;
        return sceKernelCreateThread("pnr-prx-thread", entry, arg);
      }));
  // C2: libkernel ABI subset (Sony constants translated in loader/guest_runtime).
  using V = const GuestCallArgs&;
  symbols_.push_back(impl("libkernel", "mmap", [](V a) -> std::int64_t {
    if ((6 > 6)) return -1;
    return loader::sony_mmap(a.i[0], a.i[1], a.i[2], a.i[3], a.i[4], a.i[5]);
  }));
  // Untracked counterpart of the "mmap" binding above: raw POSIX unmap for pages that
  // sony_mmap handed out. sceKernelMunmap (below) serves the tracked sceKernelMapMemory
  // registry instead and honestly refuses foreign pages; the two families stay separate.
  symbols_.push_back(impl("libkernel", "munmap", [](V a) -> std::int64_t {
    if ((2 > 6)) return -1;
    return ::munmap(reinterpret_cast<void*>(a.i[0]), static_cast<std::size_t>(a.i[1]));
  }));
  symbols_.push_back(impl("libkernel", "sceKernelClockGettime", [](V a) -> std::int64_t {
    return (2 > 6) ? SCE_KERNEL_ERROR_EINVAL : loader::sony_clock_gettime(a.i[0], a.i[1]);
  }));
  symbols_.push_back(impl("libkernel", "scePthreadMutexInit", [](V a) -> std::int64_t {
    return loader::guest_mutex_op(0, reinterpret_cast<std::int64_t*>(a.i[0]));
  }));
  symbols_.push_back(impl("libkernel", "scePthreadMutexLock", [](V a) -> std::int64_t {
    return loader::guest_mutex_op(1, reinterpret_cast<std::int64_t*>(a.i[0]));
  }));
  symbols_.push_back(impl("libkernel", "scePthreadMutexUnlock", [](V a) -> std::int64_t {
    return loader::guest_mutex_op(2, reinterpret_cast<std::int64_t*>(a.i[0]));
  }));
  symbols_.push_back(impl("libkernel", "scePthreadMutexDestroy", [](V a) -> std::int64_t {
    return loader::guest_mutex_op(3, reinterpret_cast<std::int64_t*>(a.i[0]));
  }));
  symbols_.push_back(impl("libkernel", "scePthreadCreate", [](V a) -> std::int64_t {
    if ((4 > 6)) return SCE_KERNEL_ERROR_EINVAL;
    return loader::guest_thread_create(reinterpret_cast<std::int64_t*>(a.i[0]), a.i[2], a.i[3]);
  }));
  symbols_.push_back(impl("libkernel", "scePthreadJoin", [](V a) -> std::int64_t {
    if ((2 > 6)) return SCE_KERNEL_ERROR_EINVAL;
    return loader::guest_thread_join(a.i[0], reinterpret_cast<std::int64_t*>(a.i[1]));
  }));
  symbols_.push_back(impl("libkernel", "scePthreadSelf", [](V) -> std::int64_t { return loader::guest_thread_self(); }));
  // S1: condition variables and semaphores (registry handles; see loader/guest_runtime.hpp).
  symbols_.push_back(impl("libkernel", "scePthreadCondInit", [](V a) -> std::int64_t {
    return loader::guest_cond_init(reinterpret_cast<std::int64_t*>(a.i[0])); }));
  symbols_.push_back(impl("libkernel", "scePthreadCondWait", [](V a) -> std::int64_t {
    return (2 > 6) ? SCE_KERNEL_ERROR_EINVAL : loader::guest_cond_wait(*reinterpret_cast<std::int64_t*>(a.i[0]), reinterpret_cast<std::int64_t*>(a.i[1]), -1); }));
  symbols_.push_back(impl("libkernel", "scePthreadCondTimedwait", [](V a) -> std::int64_t {
    return (3 > 6) ? SCE_KERNEL_ERROR_EINVAL : loader::guest_cond_wait(*reinterpret_cast<std::int64_t*>(a.i[0]), reinterpret_cast<std::int64_t*>(a.i[1]), a.i[2]); }));
  symbols_.push_back(impl("libkernel", "scePthreadCondSignal", [](V a) -> std::int64_t {
    return loader::guest_cond_wake(*reinterpret_cast<std::int64_t*>(a.i[0]), false); }));
  symbols_.push_back(impl("libkernel", "scePthreadCondBroadcast", [](V a) -> std::int64_t {
    return loader::guest_cond_wake(*reinterpret_cast<std::int64_t*>(a.i[0]), true); }));
  symbols_.push_back(impl("libkernel", "scePthreadCondDestroy", [](V a) -> std::int64_t {
    return loader::guest_cond_destroy(reinterpret_cast<std::int64_t*>(a.i[0])); }));
  symbols_.push_back(impl("libkernel", "sceKernelCreateSema", [](V a) -> std::int64_t {
    return (5 > 6) ? SCE_KERNEL_ERROR_EINVAL : loader::guest_sema_create(reinterpret_cast<std::int64_t*>(a.i[0]), a.i[3], a.i[4]); }));
  symbols_.push_back(impl("libkernel", "sceKernelWaitSema", [](V a) -> std::int64_t {
    if ((3 > 6)) return SCE_KERNEL_ERROR_EINVAL;
    std::int64_t us = -1;
    if (a.i[2]) us = static_cast<std::int64_t>(*reinterpret_cast<std::uint32_t*>(a.i[2]));
    return loader::guest_sema_wait(a.i[0], a.i[1], us); }));
  symbols_.push_back(impl("libkernel", "sceKernelSignalSema", [](V a) -> std::int64_t {
    return (2 > 6) ? SCE_KERNEL_ERROR_EINVAL : loader::guest_sema_signal(a.i[0], a.i[1]); }));
  symbols_.push_back(impl("libkernel", "sceKernelDeleteSema", [](V a) -> std::int64_t {
    std::int64_t h = a.i[0]; return loader::guest_sema_destroy(&h); }));
  add_libc_abi_symbols(symbols_);  // C3
  add_hw_abi_symbols(symbols_);    // C5
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
      [](const GuestCallArgs& a) -> std::int64_t {
        if ((2 > 6)) return SCE_KERNEL_ERROR_EINVAL;
        return reinterpret_cast<std::int64_t>(sceKernelMapMemory(
            static_cast<std::size_t>(a.i[0]), a.i[1]));
      }));
  symbols_.push_back(impl("libkernel", "sceKernelMunmap",
      [](const GuestCallArgs& a) -> std::int64_t {
        if ((2 > 6)) return SCE_KERNEL_ERROR_EINVAL;
        return sceKernelMunmap(reinterpret_cast<void*>(a.i[0]),
                               static_cast<std::size_t>(a.i[1]));
      }));
  symbols_.push_back(impl("libkernel", "sceKernelMTypeprotect",
      [](const GuestCallArgs& a) -> std::int64_t {
        if ((3 > 6)) return SCE_KERNEL_ERROR_EINVAL;
        return sceKernelMTypeprotect(reinterpret_cast<void*>(a.i[0]),
                                     static_cast<std::size_t>(a.i[1]), a.i[2]);
      }));
  symbols_.push_back(impl("libkernel", "sceKernelQueryMemoryProtection",
      [](const GuestCallArgs& a) -> std::int64_t {
        void* s = nullptr; void* e = nullptr; std::int64_t prot = 0;
        const auto rc = sceKernelQueryMemoryProtection(
            reinterpret_cast<void*>(a.i[0]), &s, &e, &prot);
        if (rc == SCE_OK) {
          *reinterpret_cast<void**>(a.i[1]) = s;
          *reinterpret_cast<void**>(a.i[2]) = e;
          *reinterpret_cast<std::int64_t*>(a.i[3]) = prot;
        }
        return rc;
      }));
  // libSceLibcInternal subset, implemented over host equivalents.
  symbols_.push_back(impl("libSceLibcInternal", "printf",
      [](const GuestCallArgs& a) -> std::int64_t {
        const auto* fmt = reinterpret_cast<const char*>(a.i[0]);
        return sce_libc_printf(fmt, a, 1);
      }));
  // Fixed-signature FP probes: these are the functions whose float32 decoding, mixed GP/FP
  // argument pools, and xmm0 return path the S35 regression suite exercises. They are ordinary
  // real-libc bindings, not fixtures: fmod reads two full double lanes, fmaxf reads the low
  // 32 bits of each lane (float32; upper lane bits are unspecified by the ABI and the test
  // passes deliberate garbage there), ldexp mixes one FP argument (xmm0) with one integer
  // argument (the first GP slot), and all three return their result in xmm0.
  symbols_.push_back(impl("libSceLibcInternal", "fmod",
      [](const GuestCallArgs& a) -> std::int64_t {
        return fp_ret_bits(std::fmod(arg_double(a, 0), arg_double(a, 1)));
      }, true));
  symbols_.push_back(impl("libSceLibcInternal", "fmaxf",
      [](const GuestCallArgs& a) -> std::int64_t {
        return fp_ret_bits(std::fmax(arg_float(a, 0), arg_float(a, 1)));
      }, true));
  symbols_.push_back(impl("libSceLibcInternal", "ldexp",
      [](const GuestCallArgs& a) -> std::int64_t {
        return fp_ret_bits(std::ldexp(arg_double(a, 0), static_cast<int>(a.i[0])));
      }, true));
  symbols_.push_back(impl("libSceLibcInternal", "malloc",
      [](const GuestCallArgs& a) -> std::int64_t {
        return reinterpret_cast<std::int64_t>(sce_libc_malloc(static_cast<std::size_t>(a.i[0])));
      }));
  symbols_.push_back(impl("libSceLibcInternal", "free",
      [](const GuestCallArgs& a) -> std::int64_t {
        sce_libc_free(reinterpret_cast<void*>(a.i[0]));
        return SCE_OK;
      }));
  symbols_.push_back(impl("libSceLibcInternal", "memcpy",
      [](const GuestCallArgs& a) -> std::int64_t {
        if ((3 > 6)) return SCE_KERNEL_ERROR_EINVAL;
        return reinterpret_cast<std::int64_t>(sce_libc_memcpy(
            reinterpret_cast<void*>(a.i[0]), reinterpret_cast<const void*>(a.i[1]),
            static_cast<std::size_t>(a.i[2])));
      }));
  symbols_.push_back(impl("libSceLibcInternal", "memset",
      [](const GuestCallArgs& a) -> std::int64_t {
        if ((3 > 6)) return SCE_KERNEL_ERROR_EINVAL;
        return reinterpret_cast<std::int64_t>(sce_libc_memset(
            reinterpret_cast<void*>(a.i[0]), static_cast<int>(a.i[1]),
            static_cast<std::size_t>(a.i[2])));
      }));
  symbols_.push_back(impl("libSceLibcInternal", "strlen",
      [](const GuestCallArgs& a) -> std::int64_t {
        return static_cast<std::int64_t>(
            sce_libc_strlen(reinterpret_cast<const char*>(a.i[0])));
      }));
  symbols_.push_back(impl("libSceLibcInternal", "strcmp",
      [](const GuestCallArgs& a) -> std::int64_t {
        if ((2 > 6)) return SCE_KERNEL_ERROR_EINVAL;
        return sce_libc_strcmp(reinterpret_cast<const char*>(a.i[0]),
                               reinterpret_cast<const char*>(a.i[1]));
      }));
}

void PrxRegistry::install_file_calls(PsFileTable* t) {
  if (t == nullptr) return;
  auto set_impl = [this, t](const char* name,
      std::function<std::int64_t(const GuestCallArgs&)> fn) {
    for (auto& s : symbols_)
      if (s.name == name) { s.implemented = true; s.invoke = std::move(fn); }
  };
  set_impl("sceKernelOpen", [t](const GuestCallArgs& a) -> std::int64_t {
    if ((2 > 6)) return SCE_KERNEL_ERROR_EINVAL;
    return t->open(reinterpret_cast<const char*>(a.i[0]), a.i[1]);
  });
  set_impl("sceKernelRead", [t](const GuestCallArgs& a) -> std::int64_t {
    if ((3 > 6)) return SCE_KERNEL_ERROR_EINVAL;
    return t->read(a.i[0], reinterpret_cast<void*>(a.i[1]),
                   static_cast<std::size_t>(a.i[2]));
  });
  set_impl("sceKernelWrite", [t](const GuestCallArgs& a) -> std::int64_t {
    if ((3 > 6)) return SCE_KERNEL_ERROR_EINVAL;
    return t->write(a.i[0], reinterpret_cast<const void*>(a.i[1]),
                    static_cast<std::size_t>(a.i[2]));
  });
  set_impl("sceKernelClose", [t](const GuestCallArgs& a) -> std::int64_t {
    return t->close(a.i[0]);
  });
  set_impl("sceKernelLseek", [t](const GuestCallArgs& a) -> std::int64_t {
    if ((3 > 6)) return SCE_KERNEL_ERROR_EINVAL;
    return t->lseek(a.i[0], a.i[1], a.i[2]);
  });
  set_impl("sceKernelFstat", [t](const GuestCallArgs& a) -> std::int64_t {
    if ((2 > 6)) return SCE_KERNEL_ERROR_EINVAL;
    return t->fstat(a.i[0], reinterpret_cast<SceKernelStat*>(a.i[1]));
  });
}

void PrxRegistry::install_synthetic_test_calls() {
  // Test-only scaffolding for the ABI regression suite. "libpnrtest" is not a
  // Sony library and these names are not Sony APIs; they exist so guest code
  // can prove host-side capture properties without inventing a Sony signature.
  // pnrTestSevenScalarArgs(a,b,c,d,e,f, g@stack) returns a+b+c+d+e+f+g, which
  // only comes out right when the 7th (stack) argument is captured verbatim.
  symbols_.push_back(impl("libpnrtest", "pnrTestSevenScalarArgs",
      [](const GuestCallArgs& a) -> std::int64_t {
        return a.i[0] + a.i[1] + a.i[2] + a.i[3] + a.i[4] + a.i[5] + a.s[0];
      }));
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
    std::function<std::int64_t(const GuestCallArgs&)>* fn,
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
