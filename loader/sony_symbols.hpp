// PNR Loader - Sony-style dynamic symbol names (Core slice C1), GPL-2.0.
// Executables prepared for PlayStation systems name their imports
// "<NID>#<library id>#<module id>": an 11-character NID (see nid.hpp) followed
// by base64-coded library and module ids. This parses that form.
// Status: Implemented, Tested (unit tests). Only the NAME FORM is handled here.
#pragma once
#include <string>

namespace pnr::loader {

struct SonySymbol {
  std::string nid;      // 11 characters
  std::string lib_b64;  // library id as written
  std::string mod_b64;  // module id as written
  int lib_id = -1;      // decoded
  int mod_id = -1;
};

// Decodes a 1-3 character id in the PS base64 alphabet. Returns -1 when invalid.
int decode_sony_id(const std::string& s);

// True when `name` has the exact form NID#lib#mod. Plain names (printf, sceFoo) return false.
bool parse_sony_symbol(const std::string& name, SonySymbol* out);

}  // namespace pnr::loader
