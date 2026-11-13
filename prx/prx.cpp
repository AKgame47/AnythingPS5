// Implementations are original PNR code over MODULE 03 compat services.
// Taxonomy (which symbols exist, library grouping, NID binding concept)
// follows AnyPS5 (GPL-2.0) core/libs/prx - see prx.hpp header note.
#include "prx/prx.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "loader/nid.hpp"

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
  // Declared-only taxonomy entries (honest: no host implementation yet).
  symbols_.push_back(decl_only("libSceLibcInternal", "printf"));
  symbols_.push_back(decl_only("libSceLibcInternal", "malloc"));
  symbols_.push_back(decl_only("libSceLibcInternal", "free"));
  symbols_.push_back(decl_only("libSceLibcInternal", "memcpy"));
  symbols_.push_back(decl_only("libSceLibcInternal", "memset"));
  symbols_.push_back(decl_only("libSceLibcInternal", "strlen"));
  symbols_.push_back(decl_only("libSceLibcInternal", "strcmp"));
}

const PrxSymbol* PrxRegistry::find(const std::string& nid) const {
  for (const auto& s : symbols_)
    if (s.nid == nid) return &s;
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
