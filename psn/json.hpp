// Minimal strict JSON reader for PSN response parsing (offline, bounded).
// Not a general-purpose library: depth <= 32, input <= 16 MiB, strings
// <= 1 MiB, no comments, no trailing commas, no duplicate-key checks
// (last key wins). Numbers are kept as double. GPL-2.0.
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace pnr::psn {

struct Json {
  enum class T { Null, Bool, Num, Str, Arr, Obj } t = T::Null;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<Json> a;
  std::map<std::string, Json> o;

  bool is(T x) const { return t == x; }
  const Json* get(const std::string& k) const {
    if (t != T::Obj) return nullptr;
    auto it = o.find(k);
    return it == o.end() ? nullptr : &it->second;
  }
};

constexpr size_t kJsonMaxInput = 16u * 1024 * 1024;

// Returns true on success. On failure `err` names the problem and byte
// offset (never echoes input text, so secrets cannot leak through it).
bool json_parse(const std::string& text, Json* out, std::string* err);

}  // namespace pnr::psn
