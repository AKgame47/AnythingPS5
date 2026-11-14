// PNR plugin system - versions and version constraints (slice P1), GPL-2.0.
#include "plugin/semver.hpp"

#include <cctype>
#include <sstream>

namespace pnr::plugin {

std::string Version::str() const { return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch); }

bool parse_version(const std::string& s, Version* out) {
  std::uint32_t part[3] = {0, 0, 0};
  int idx = 0;
  std::size_t digits = 0;
  if (s.empty()) return false;
  for (char c : s) {
    if (c == '.') {
      if (digits == 0 || idx >= 2) return false;
      ++idx; digits = 0;
    } else if (std::isdigit(static_cast<unsigned char>(c))) {
      if (++digits > 9) return false;
      part[idx] = part[idx] * 10 + static_cast<std::uint32_t>(c - '0');
    } else {
      return false;
    }
  }
  if (idx != 2 || digits == 0) return false;
  if (out) *out = Version{part[0], part[1], part[2]};
  return true;
}

std::string Constraint::parse(const std::string& s) {
  clauses_.clear();
  text_ = s;
  std::istringstream in(s);
  std::string tok;
  int n = 0;
  while (in >> tok) {
    ++n;
    if (tok == "*") { if (n != 1 || in >> tok) return "'*' cannot be combined with other comparators"; return {}; }
    char op = '=';
    std::size_t skip = 0;
    if (tok.compare(0, 2, ">=") == 0) { op = 'G'; skip = 2; }
    else if (tok.compare(0, 2, "<=") == 0) { op = 'L'; skip = 2; }
    else if (tok[0] == '>') { op = '>'; skip = 1; }
    else if (tok[0] == '<') { op = '<'; skip = 1; }
    else if (tok[0] == '=') { op = '='; skip = 1; }
    else if (tok[0] == '^') { op = '^'; skip = 1; }
    else if (tok[0] == '~') { op = '~'; skip = 1; }
    Version v;
    if (!parse_version(tok.substr(skip), &v)) { clauses_.clear(); return "bad version in constraint: '" + tok + "'"; }
    clauses_.push_back({op, v});
  }
  if (n == 0) return "empty version constraint";
  return {};
}

bool Constraint::satisfied_by(const Version& v) const {
  for (const auto& c : clauses_) {
    switch (c.op) {
      case '=': if (!(v == c.v)) return false; break;
      case '>': if (!(c.v < v)) return false; break;
      case 'G': if (v < c.v) return false; break;
      case '<': if (!(v < c.v)) return false; break;
      case 'L': if (c.v < v) return false; break;
      case '^': if (v < c.v || v.major != c.v.major) return false; break;
      case '~': if (v < c.v || v.major != c.v.major || v.minor != c.v.minor) return false; break;
    }
  }
  return true;  // no clauses = "*"
}

}  // namespace pnr::plugin
