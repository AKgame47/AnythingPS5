// PNR plugin system - versions and version constraints (slice P1), GPL-2.0.
// Strict "MAJOR.MINOR.PATCH" (digits only, each at most 9 digits). Pre-release and build suffixes are
// refused rather than guessed at. Constraints: space separated comparators, ALL must hold:
//   =1.2.3  >1.2.3  >=1.2.3  <1.2.3  <=1.2.3  ^1.2.3 (same major)  ~1.2.3 (same major.minor)
// A bare "1.2.3" means =1.2.3. "*" matches every version.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pnr::plugin {

struct Version {
  std::uint32_t major = 0, minor = 0, patch = 0;
  bool operator==(const Version& o) const { return major == o.major && minor == o.minor && patch == o.patch; }
  bool operator<(const Version& o) const { return major != o.major ? major < o.major : minor != o.minor ? minor < o.minor : patch < o.patch; }
  std::string str() const;
};

bool parse_version(const std::string& s, Version* out);

class Constraint {
 public:
  // Returns an empty string on success, otherwise a plain-words error.
  std::string parse(const std::string& s);
  bool satisfied_by(const Version& v) const;
  const std::string& text() const { return text_; }
 private:
  struct Clause { char op; Version v; };  // op: '=', '>', 'G' (>=), '<', 'L' (<=), '^', '~'
  std::vector<Clause> clauses_;
  std::string text_;
};

}  // namespace pnr::plugin
