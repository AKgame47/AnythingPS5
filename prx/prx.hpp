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
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace pnr::prx {

class PsFileTable;  // prx_file.hpp

using SceInt32 = std::int32_t;
constexpr SceInt32 SCE_OK = 0;
constexpr SceInt32 SCE_KERNEL_ERROR_ENOMEM = -1;
constexpr SceInt32 SCE_KERNEL_ERROR_EINVAL = -2;
constexpr SceInt32 SCE_KERNEL_ERROR_UNKNOWN = -3;

// Slice S35: the arguments a guest-to-host thunk captures at the call boundary, per the
// System V AMD64 ABI: up to 6 integer/pointer arguments (rdi, rsi, rdx, rcx, r8, r9) in i[],
// and up to 8 floating point arguments (xmm0-7) in x[]. Arguments beyond the register counts
// (stack spill) are NOT captured - a documented thunk limit.
struct GuestCallArgs {
  std::int64_t i[6] = {};
  double x[8] = {};
  // Slice S36: raw stack-spill window. Per the SysV ABI, arguments past the register pools
  // (7th+ gp, 9th+ xmm) sit at [rsp+8...] at call time as a dense array of 8-byte slots in
  // argument order, regardless of class. The thunk copies the first 8 slots verbatim; bindings
  // and the varargs formatter decode them per their own signature/conversion order. Supported
  // slot width is 8 bytes (no 16-byte types); calls spilling more than 8 slots are unsupported.
  std::int64_t s[8] = {};
};

// Lane decoding. x[] holds the raw 64-bit xmm lane contents bit-exactly. A DOUBLE argument is
// the whole lane; a FLOAT argument is the lane's low 32 bits (the upper 32 are unspecified by
// the ABI and must not be read). FP RETURN values travel in xmm0: a binding returns the value's
// bits as its int64 result and the FP-return thunk moves them back into xmm0 for the guest.
inline double arg_double(const GuestCallArgs& a, int k) { return a.x[k]; }
inline float arg_float(const GuestCallArgs& a, int k) {
  std::uint64_t bits;
  std::memcpy(&bits, &a.x[k], 8);
  const std::uint32_t lo = static_cast<std::uint32_t>(bits);
  float f;
  std::memcpy(&f, &lo, 4);
  return f;
}
inline std::int64_t fp_ret_bits(double d) { std::int64_t r; std::memcpy(&r, &d, 8); return r; }
inline std::int64_t fp_ret_bits(float f) { std::uint32_t b; std::memcpy(&b, &f, 4); return b; }

// One exported symbol of a PS-style system library.
struct PrxSymbol {
  std::string library;   // e.g. "libkernel"
  std::string name;      // e.g. "sceKernelUsleep"
  std::string nid;       // computed via loader::nid::compute_nid
  bool implemented;      // false => declared in taxonomy, no host impl yet
  std::function<std::int64_t(const GuestCallArgs&)> invoke;
  bool returns_fp = false;  // true => guest expects the result in xmm0 (float/double return)
};

// The registry a loader consults to bind imports by NID.
class PrxRegistry {
 public:
  PrxRegistry();  // populates the built-in taxonomy subset

  // All known symbols (implemented or declared-only).
  const std::vector<PrxSymbol>& symbols() const { return symbols_; }

  // Look up by NID. Returns nullptr when the NID is not in the taxonomy.
  const PrxSymbol* find(const std::string& nid) const;

  // Look up by symbol name (any library). nullptr when not in the taxonomy.
  const PrxSymbol* find_name(const std::string& name) const;

  // Bind an imported NID to a callable host trampoline.
  // Returns SCE_OK and sets fn on success;
  // SCE_KERNEL_ERROR_UNKNOWN when the NID is not in the taxonomy;
  // SCE_KERNEL_ERROR_EINVAL when known but not yet implemented (honest).
  SceInt32 bind(const std::string& nid,
                std::function<std::int64_t(const GuestCallArgs&)>* fn,
                std::string* error) const;

  // Installs the libkernel file calls (open/read/write/close/lseek/fstat)
  // backed by a jailed descriptor table. Until this is called those
  // symbols stay declared-only (honest). Not thread-safe vs bind(); call
  // once during setup.
  void install_file_calls(PsFileTable* table);

  // Test-only ABI scaffolding for the regression suite: clearly synthetic host
  // imports (library "libpnrtest"), NOT Sony APIs. Used to prove ABI capture
  // properties (e.g. the 7th scalar stack argument) without inventing a Sony
  // signature. Installed only on explicit request (PNR_TEST_IMPORTS).
  void install_synthetic_test_calls();

 private:
  std::vector<PrxSymbol> symbols_;
};

// Host implementations (libkernel subset), built on compat services:
SceInt32 sceKernelUsleep(std::uint32_t microseconds);
void* sceKernelAlloc(std::size_t size);
SceInt32 sceKernelFree(void* ptr);
SceInt32 sceKernelSleep(std::uint32_t seconds);
SceInt32 sceKernelCreateThread(const char* name, void (*entry)(void*), void* arg);

// Host implementations (libkernel memory management), over POSIX
// mmap/mprotect/munmap. Only anonymous private mappings are supported;
// anything else is EINVAL. Queries cover mappings created through this
// API only - stated, not widened.
constexpr std::int64_t SCE_KERNEL_PROT_READ = 1;
constexpr std::int64_t SCE_KERNEL_PROT_WRITE = 2;
constexpr std::int64_t SCE_KERNEL_PROT_EXEC = 4;
void* sceKernelMapMemory(std::size_t size, std::int64_t prot);
SceInt32 sceKernelMunmap(void* addr, std::size_t size);
SceInt32 sceKernelMTypeprotect(void* addr, std::size_t size, std::int64_t prot);
SceInt32 sceKernelQueryMemoryProtection(void* addr, void** start, void** end,
                                        std::int64_t* prot);

// Host implementations (libSceLibcInternal subset). printf is a bounded
// formatter: integer conversions plus %f %e %g %a (slice S35), consuming the
// SysV gp/xmm argument pools captured by the thunk; anything else prints
// literally. gp0 is the index of the first variadic gp slot (1 for printf).
void* sce_libc_memcpy(void* dst, const void* src, std::size_t n);
void* sce_libc_memset(void* dst, int c, std::size_t n);
std::size_t sce_libc_strlen(const char* s);
int sce_libc_strcmp(const char* a, const char* b);
void* sce_libc_malloc(std::size_t size);
void sce_libc_free(void* ptr);
int sce_libc_printf(const char* fmt, const GuestCallArgs& args, std::size_t gp0);

}  // namespace pnr::prx
