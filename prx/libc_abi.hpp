// PNR PRX - libSceLibcInternal and libSceSysmodule subset (Core slice C3), GPL-2.0.
// Host implementations of C library functions, bound to guest modules by NID.
// Status: Implemented, Tested (unit tests call the symbols directly; a synthetic machine-code
// module calls them through NID-bound thunks).
// Honest limits:
//  - Integer and pointer arguments only (at most 6 per call: a thunk limit). Floating point formats
//    (%f %e %g) and floating point arguments are NOT supported; they are printed literally.
//  - Like the real libc, these functions trust the guest's pointers. A bad pointer from guest code can
//    crash the guest process (the launcher is unaffected: modules run in a forked child).
//  - sprintf is capped at 4096 bytes. rand() is PNR's own generator, not Sony's.
//  - libSceSysmodule calls are STUBS (Experimental): they record ids and report success, no module
//    is really loaded and the id values are not verified against a console.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "prx/prx.hpp"

namespace pnr::prx {

// C-style formatter over integer arguments: flags '-' and '0', width, length h hh l ll z j t,
// conversions d i u x X o c s p %. Anything else is copied literally.
std::string format_c(const char* fmt, const std::vector<std::int64_t>& args);

// Adds the libc/sysmodule symbols to a registry's table.
void add_libc_abi_symbols(std::vector<PrxSymbol>& out);

// Stub state, exposed for tests.
bool sysmodule_is_loaded(std::int64_t id);
void sysmodule_reset();

}  // namespace pnr::prx
