// PNR Native Executable Loader - MODULE 02: PS NID support (v1)
// NID computation and lookup for PlayStation-style imported symbols.
// Algorithm adapted from AnyPS5 (https://github.com/boykopovar/AnyPS5),
// core/libs/nid, licensed GPL-2.0; AnythingPS5 is GPL-2.0 (see LICENSE).
// Status: Implemented, Tested.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace pnr::loader::nid {

// SHA-1 digest (20 bytes) of arbitrary data. Implemented for NID use.
std::array<std::uint8_t, 20> sha1(const std::vector<std::uint8_t>& data);

// PS NID: base64(reversed first 8 bytes of SHA1(symbol_name + fixed suffix))
// with the PS alphabet, 11 characters. Throws std::invalid_argument on an
// empty symbol name.
std::string compute_nid(const std::string& symbol_name,
                        const std::string& library_name);

// Looks up a NID in the known-symbols table. Returns the symbol name or an
// empty string when unknown. The table covers a small set of common
// libkernel/libSceLibcInternal symbols today and grows with the PRX layer.
std::string resolve_nid(const std::string& nid);

}  // namespace pnr::loader::nid
