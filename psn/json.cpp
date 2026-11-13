#include "psn/json.hpp"

#include <cmath>
#include <cstdlib>

namespace pnr::psn {
namespace {
struct P {
  const std::string& t;
  size_t i = 0;
  std::string err;
  explicit P(const std::string& s) : t(s) {}
  bool fail(const char* m) {
    if (err.empty()) err = std::string(m) + " at byte " + std::to_string(i);
    return false;
  }
  void ws() { while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) ++i; }
  static void utf8(std::string& o, unsigned c) {
    if (c < 0x80) o += static_cast<char>(c);
    else if (c < 0x800) { o += static_cast<char>(0xC0 | (c >> 6)); o += static_cast<char>(0x80 | (c & 63)); }
    else if (c < 0x10000) { o += static_cast<char>(0xE0 | (c >> 12)); o += static_cast<char>(0x80 | ((c >> 6) & 63)); o += static_cast<char>(0x80 | (c & 63)); }
    else { o += static_cast<char>(0xF0 | (c >> 18)); o += static_cast<char>(0x80 | ((c >> 12) & 63)); o += static_cast<char>(0x80 | ((c >> 6) & 63)); o += static_cast<char>(0x80 | (c & 63)); }
  }
  bool hex4(unsigned* v) {
    if (i + 4 > t.size()) return fail("truncated \\u escape");
    unsigned x = 0;
    for (int k = 0; k < 4; ++k) {
      char c = t[i + k];
      x <<= 4;
      if (c >= '0' && c <= '9') x |= c - '0';
      else if (c >= 'a' && c <= 'f') x |= c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') x |= c - 'A' + 10;
      else return fail("bad \\u escape");
    }
    i += 4; *v = x; return true;
  }
  bool str(std::string* o) {
    ++i;  // opening quote
    o->clear();
    while (true) {
      if (i >= t.size()) return fail("unterminated string");
      unsigned char c = static_cast<unsigned char>(t[i]);
      if (c == '"') { ++i; return true; }
      if (c < 0x20) return fail("control character in string");
      if (o->size() > (1u << 20)) return fail("string too long");
      if (c != '\\') { *o += static_cast<char>(c); ++i; continue; }
      ++i;
      if (i >= t.size()) return fail("truncated escape");
      char e = t[i++];
      switch (e) {
        case '"': *o += '"'; break; case '\\': *o += '\\'; break; case '/': *o += '/'; break;
        case 'b': *o += '\b'; break; case 'f': *o += '\f'; break; case 'n': *o += '\n'; break;
        case 'r': *o += '\r'; break; case 't': *o += '\t'; break;
        case 'u': {
          unsigned v = 0;
          if (!hex4(&v)) return false;
          if (v >= 0xD800 && v <= 0xDBFF) {
            if (i + 1 < t.size() && t[i] == '\\' && t[i + 1] == 'u') {
              i += 2; unsigned lo = 0;
              if (!hex4(&lo)) return false;
              if (lo < 0xDC00 || lo > 0xDFFF) return fail("bad surrogate pair");
              v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
            } else return fail("lone surrogate");
          } else if (v >= 0xDC00 && v <= 0xDFFF) return fail("lone surrogate");
          utf8(*o, v);
          break;
        }
        default: return fail("bad escape");
      }
    }
  }
  bool val(Json* j, int depth) {
    if (depth > 32) return fail("nesting too deep");
    ws();
    if (i >= t.size()) return fail("unexpected end");
    char c = t[i];
    if (c == '{') {
      j->t = Json::T::Obj; ++i; ws();
      if (i < t.size() && t[i] == '}') { ++i; return true; }
      while (true) {
        ws();
        if (i >= t.size() || t[i] != '"') return fail("expected object key");
        std::string k;
        if (!str(&k)) return false;
        ws();
        if (i >= t.size() || t[i] != ':') return fail("expected ':'");
        ++i;
        Json v;
        if (!val(&v, depth + 1)) return false;
        j->o[k] = std::move(v);
        ws();
        if (i < t.size() && t[i] == ',') { ++i; continue; }
        if (i < t.size() && t[i] == '}') { ++i; return true; }
        return fail("expected ',' or '}'");
      }
    }
    if (c == '[') {
      j->t = Json::T::Arr; ++i; ws();
      if (i < t.size() && t[i] == ']') { ++i; return true; }
      while (true) {
        Json v;
        if (!val(&v, depth + 1)) return false;
        j->a.push_back(std::move(v));
        if (j->a.size() > 1000000) return fail("array too long");
        ws();
        if (i < t.size() && t[i] == ',') { ++i; continue; }
        if (i < t.size() && t[i] == ']') { ++i; return true; }
        return fail("expected ',' or ']'");
      }
    }
    if (c == '"') { j->t = Json::T::Str; return str(&j->s); }
    if (t.compare(i, 4, "true") == 0) { j->t = Json::T::Bool; j->b = true; i += 4; return true; }
    if (t.compare(i, 5, "false") == 0) { j->t = Json::T::Bool; j->b = false; i += 5; return true; }
    if (t.compare(i, 4, "null") == 0) { j->t = Json::T::Null; i += 4; return true; }
    if (c == '-' || (c >= '0' && c <= '9')) {
      size_t st = i;
      if (t[i] == '-') ++i;
      if (i >= t.size() || t[i] < '0' || t[i] > '9') return fail("bad number");
      if (t[i] == '0') ++i; else while (i < t.size() && t[i] >= '0' && t[i] <= '9') ++i;
      if (i < t.size() && t[i] == '.') {
        ++i; if (i >= t.size() || t[i] < '0' || t[i] > '9') return fail("bad number");
        while (i < t.size() && t[i] >= '0' && t[i] <= '9') ++i;
      }
      if (i < t.size() && (t[i] == 'e' || t[i] == 'E')) {
        ++i; if (i < t.size() && (t[i] == '+' || t[i] == '-')) ++i;
        if (i >= t.size() || t[i] < '0' || t[i] > '9') return fail("bad number");
        while (i < t.size() && t[i] >= '0' && t[i] <= '9') ++i;
      }
      j->t = Json::T::Num;
      j->n = std::strtod(t.substr(st, i - st).c_str(), nullptr);
      if (!std::isfinite(j->n)) return fail("number out of range");
      return true;
    }
    return fail("unexpected character");
  }
};
}  // namespace

bool json_parse(const std::string& text, Json* out, std::string* err) {
  if (text.size() > kJsonMaxInput) { if (err) *err = "input too large"; return false; }
  P p(text);
  Json j;
  if (!p.val(&j, 0)) { if (err) *err = p.err; return false; }
  p.ws();
  if (p.i != text.size()) { p.fail("trailing data"); if (err) *err = p.err; return false; }
  *out = std::move(j);
  return true;
}

}  // namespace pnr::psn
