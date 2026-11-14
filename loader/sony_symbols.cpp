#include "loader/sony_symbols.hpp"

namespace pnr::loader {
namespace {
int idx(char c) {
  static const char kA[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
  for (int i = 0; i < 64; ++i) if (kA[i] == c) return i;
  return -1;
}
}  // namespace

int decode_sony_id(const std::string& s) {
  if (s.empty() || s.size() > 3) return -1;
  int v = 0;
  for (char c : s) {
    int i = idx(c);
    if (i < 0) return -1;
    v = v * 64 + i;
  }
  return v;
}

bool parse_sony_symbol(const std::string& name, SonySymbol* out) {
  if (name.size() < 15 || name[11] != '#') return false;
  const auto second = name.find('#', 12);
  if (second == std::string::npos) return false;
  SonySymbol s;
  s.nid = name.substr(0, 11);
  for (char c : s.nid) if (idx(c) < 0) return false;
  s.lib_b64 = name.substr(12, second - 12);
  s.mod_b64 = name.substr(second + 1);
  s.lib_id = decode_sony_id(s.lib_b64);
  s.mod_id = decode_sony_id(s.mod_b64);
  if (s.lib_id < 0 || s.mod_id < 0) return false;
  if (out) *out = s;
  return true;
}

}  // namespace pnr::loader
