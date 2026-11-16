// PNR PRX - libSceLibcInternal and libSceSysmodule subset (Core slice C3), GPL-2.0.
// Host implementations of C library functions, bound to guest modules by NID.
// Status: Implemented, Tested (unit tests call the symbols directly; a synthetic machine-code
// module calls them through NID-bound thunks).
// Honest limits:
//  - At most 6 integer/pointer and 8 floating point arguments per call (thunk register limit;
//    arguments spilled to the stack are not captured).
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

// C-style formatter: flags '-' and '0', width, precision, length h hh l ll z j t,
// integer conversions d i u x X o c s p % over the gp pool, and (slice S35) floating point
// conversions f F e E g G a A over the xmm pool (long double is refused and stays literal).
// Anything else is copied literally.
std::string format_c(const char* fmt, const std::vector<std::int64_t>& args);
// GuestCallArgs form: gp0 is the index of the first variadic gp slot (printf: 1, sprintf: 2,
// snprintf: 3); the xmm pool always starts at x[0] for the first FP variadic argument.
std::string format_c_guest(const char* fmt, const GuestCallArgs& a, std::size_t gp0 = 0);

// Adds the libc/sysmodule symbols to a registry's table.
void add_libc_abi_symbols(std::vector<PrxSymbol>& out);

// Stub state, exposed for tests.
bool sysmodule_is_loaded(std::int64_t id);
void sysmodule_reset();

}  // namespace pnr::prx
