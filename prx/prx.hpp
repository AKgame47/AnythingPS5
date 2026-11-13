// PNR PRX system-library layer - MODULE 02 companion (v1)
// Host implementations of a small, honest subset of PlayStation-style
// system libraries, bound to games by NID. Library/symbol organization
// follows the taxonomy of AnyPS5 (https://github.com/boykopovar/AnyPS5,
// GPL-2.0) core/libs/prx; implementations here are original PNR code
// built on our compat services (no AnyPS5 implementation code copied).
// Status: Implemented, Tested. Only the symbols listed as implemented
// actually work; everything else resolves to a clear "not implemented".
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pnr::prx {

using SceInt32 = std::int32_t;
constexpr SceInt32 SCE_OK = 0;
constexpr SceInt32 SCE_KERNEL_ERROR_ENOMEM = -1;
constexpr SceInt32 SCE_KERNEL_ERROR_EINVAL = -2;
constexpr SceInt32 SCE_KERNEL_ERROR_UNKNOWN = -3;

// One exported symbol of a PS-style system library.
struct PrxSymbol {
  std::string library;   // e.g. "libkernel"
  std::string name;      // e.g. "sceKernelUsleep"
  std::string nid;       // computed via loader::nid::compute_nid
  bool implemented;      // false => declared in taxonomy, no host impl yet
  std::function<std::int64_t(const std::vector<std::int64_t>&)> invoke;
};

// The registry a loader consults to bind imports by NID.
class PrxRegistry {
 public:
  PrxRegistry();  // populates the built-in taxonomy subset

  // All known symbols (implemented or declared-only).
  const std::vector<PrxSymbol>& symbols() const { return symbols_; }

  // Look up by NID. Returns nullptr when the NID is not in the taxonomy.
  const PrxSymbol* find(const std::string& nid) const;

  // Bind an imported NID to a callable host trampoline.
  // Returns SCE_OK and sets fn on success;
  // SCE_KERNEL_ERROR_UNKNOWN when the NID is not in the taxonomy;
  // SCE_KERNEL_ERROR_EINVAL when known but not yet implemented (honest).
  SceInt32 bind(const std::string& nid,
                std::function<std::int64_t(const std::vector<std::int64_t>&)>* fn,
                std::string* error) const;

 private:
  std::vector<PrxSymbol> symbols_;
};

// Host implementations (libkernel subset), built on compat services:
SceInt32 sceKernelUsleep(std::uint32_t microseconds);
void* sceKernelAlloc(std::size_t size);
SceInt32 sceKernelFree(void* ptr);
SceInt32 sceKernelSleep(std::uint32_t seconds);
SceInt32 sceKernelCreateThread(const char* name, void (*entry)(void*), void* arg);

}  // namespace pnr::prx
