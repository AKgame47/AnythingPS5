// PNR plugin system - archive reading (slice P3b), GPL-2.0. See archive.hpp.
#include "plugin/archive.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <vector>

#include "plugin/manifest.hpp"

namespace fs = std::filesystem;
namespace pnr::plugin {
namespace {

struct Bits {
  const uint8_t* d; std::size_t n, pos = 0; uint32_t buf = 0; int cnt = 0; bool bad = false;
  int bit() { if (cnt == 0) { if (pos >= n) { bad = true; return 0; } buf = d[pos++]; cnt = 8; } int b = buf & 1; buf >>= 1; --cnt; return b; }
  uint32_t get(int k) { uint32_t v = 0; for (int i = 0; i < k; ++i) v |= (uint32_t)bit() << i; return v; }
};
struct Huff { uint16_t count[16]{}, sym[288]{}; };
bool build(Huff& h, const uint8_t* len, int n) {
  std::memset(h.count, 0, sizeof h.count);
  for (int i = 0; i < n; ++i) h.count[len[i]]++;
  int left = 1;
  for (int l = 1; l < 16; ++l) { left <<= 1; left -= h.count[l]; if (left < 0) return false; }
  uint16_t offs[16]; offs[1] = 0;
  for (int l = 1; l < 15; ++l) offs[l + 1] = offs[l] + h.count[l];
  for (int i = 0; i < n; ++i) if (len[i]) h.sym[offs[len[i]]++] = (uint16_t)i;
  return true;
}
int decode(Bits& b, const Huff& h) {
  int code = 0, first = 0, index = 0;
  for (int l = 1; l < 16; ++l) {
    code |= b.bit();
    int c = h.count[l];
    if (code - c < first) return h.sym[index + (code - first)];
    index += c; first += c; first <<= 1; code <<= 1;
    if (b.bad) return -1;
  }
  return -1;
}
const uint16_t kLb[] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
const uint16_t kLe[] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
const uint16_t kDb[] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
const uint16_t kDe[] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

bool inflate_raw(Bits& b, std::string* out, std::size_t max_out, std::string* err) {
  auto fail = [&](const char* m) { *err = m; return false; };
  int last;
  do {
    last = b.bit();
    uint32_t type = b.get(2);
    if (b.bad) return fail("truncated gzip data");
    if (type == 0) {
      b.cnt = 0;
      if (b.pos + 4 > b.n) return fail("truncated stored block");
      uint32_t len = b.d[b.pos] | (b.d[b.pos + 1] << 8), nlen = b.d[b.pos + 2] | (b.d[b.pos + 3] << 8);
      b.pos += 4;
      if (len != (~nlen & 0xffff)) return fail("bad stored block");
      if (b.pos + len > b.n) return fail("truncated stored block");
      if (out->size() + len > max_out) return fail("archive expands past the size limit");
      out->append((const char*)b.d + b.pos, len); b.pos += len;
    } else if (type == 1 || type == 2) {
      Huff lit, dist;
      if (type == 1) {
        uint8_t l[288]; int i = 0;
        for (; i < 144; ++i) l[i] = 8;
        for (; i < 256; ++i) l[i] = 9;
        for (; i < 280; ++i) l[i] = 7;
        for (; i < 288; ++i) l[i] = 8;
        build(lit, l, 288);
        uint8_t dl[30]; for (int k = 0; k < 30; ++k) dl[k] = 5; build(dist, dl, 30);
      } else {
        int nlen = (int)b.get(5) + 257, ndist = (int)b.get(5) + 1, ncode = (int)b.get(4) + 4;
        if (nlen > 286 || ndist > 30) return fail("bad dynamic block");
        static const uint8_t ord[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
        uint8_t lens[320] = {0};
        for (int i = 0; i < ncode; ++i) lens[ord[i]] = (uint8_t)b.get(3);
        Huff cl; if (!build(cl, lens, 19)) return fail("bad code lengths");
        uint8_t all[320] = {0}; int idx = 0;
        while (idx < nlen + ndist) {
          int s = decode(b, cl);
          if (s < 0) return fail("bad code length symbol");
          if (s < 16) all[idx++] = (uint8_t)s;
          else {
            int rep, v = 0;
            if (s == 16) { if (idx == 0) return fail("bad repeat"); v = all[idx - 1]; rep = 3 + (int)b.get(2); }
            else if (s == 17) rep = 3 + (int)b.get(3); else rep = 11 + (int)b.get(7);
            if (idx + rep > nlen + ndist) return fail("bad repeat");
            while (rep--) all[idx++] = (uint8_t)v;
          }
        }
        if (all[256] == 0) return fail("no end-of-block code");
        if (!build(lit, all, nlen) || !build(dist, all + nlen, ndist)) return fail("bad huffman table");
      }
      for (;;) {
        int s = decode(b, lit);
        if (s < 0 || b.bad) return fail("bad or truncated data");
        if (s < 256) { if (out->size() + 1 > max_out) return fail("archive expands past the size limit"); out->push_back((char)s); }
        else if (s == 256) break;
        else {
          s -= 257; if (s >= 29) return fail("bad length symbol");
          std::size_t len = kLb[s] + b.get(kLe[s]);
          int ds = decode(b, dist); if (ds < 0 || ds >= 30) return fail("bad distance symbol");
          std::size_t d = kDb[ds] + b.get(kDe[ds]);
          if (b.bad) return fail("truncated data");
          if (d > out->size()) return fail("distance too far back");
          if (out->size() + len > max_out) return fail("archive expands past the size limit");
          for (std::size_t i = 0; i < len; ++i) out->push_back((*out)[out->size() - d]);
        }
      }
    } else return fail("bad block type");
  } while (!last);
  return true;
}

uint32_t crc32(const std::string& s) {
  static uint32_t t[256]; static bool init = false;
  if (!init) { for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i; for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; } init = true; }
  uint32_t c = 0xFFFFFFFFu; for (unsigned char ch : s) c = t[(c ^ ch) & 0xFF] ^ (c >> 8); return c ^ 0xFFFFFFFFu;
}

std::size_t octal(const char* p, std::size_t n) { std::size_t v = 0; for (std::size_t i = 0; i < n && p[i] >= '0' && p[i] <= '7'; ++i) v = v * 8 + (std::size_t)(p[i] - '0'); return v; }

struct Ent { std::string path; std::string data; bool dir; };

bool read_tar(const std::string& t, const ArchiveLimits& lim, std::vector<Ent>* out, std::string* err) {
  std::size_t pos = 0, total = 0; std::string pax_path;
  while (pos + 512 <= t.size()) {
    const char* h = t.data() + pos;
    bool zero = true; for (int i = 0; i < 512; ++i) if (h[i]) { zero = false; break; }
    if (zero) return true;
    uint32_t sum = 0; for (int i = 0; i < 512; ++i) sum += (i >= 148 && i < 156) ? ' ' : (unsigned char)h[i];
    if (sum != octal(h + 148, 8)) { *err = "tar header checksum mismatch"; return false; }
    std::size_t size = octal(h + 124, 12); char type = h[156] ? h[156] : '0';
    std::size_t body = pos + 512;
    if (size > t.size() || body + size > t.size()) { *err = "truncated tar"; return false; }
    std::string name(h, strnlen(h, 100));
    if (std::memcmp(h + 257, "ustar", 5) == 0) { std::string pre(h + 345, strnlen(h + 345, 155)); if (!pre.empty()) name = pre + "/" + name; }
    if (type == 'g') { /* global pax header: not used for paths */ }
    else if (type == 'x') {
      std::string px = t.substr(body, size); std::size_t i = 0; pax_path.clear();
      while (i < px.size()) {
        std::size_t sp = px.find(' ', i); if (sp == std::string::npos) break;
        std::size_t len = std::strtoul(px.substr(i, sp - i).c_str(), nullptr, 10); if (len == 0 || i + len > px.size()) break;
        std::string rec = px.substr(sp + 1, i + len - sp - 2);
        if (rec.rfind("path=", 0) == 0) pax_path = rec.substr(5);
        i += len;
      }
    } else if (type == '0' || type == '5') {
      if (!pax_path.empty()) { name = pax_path; pax_path.clear(); }
      bool dir = type == '5';
      while (!name.empty() && name.back() == '/') name.pop_back();
      if (!name.empty()) {
        if (out->size() >= lim.max_files) { *err = "too many files in the archive"; return false; }
        total += size; if (total > lim.max_total_bytes) { *err = "archive is too large"; return false; }
        out->push_back({name, dir ? std::string() : t.substr(body, size), dir});
      }
    } else { *err = std::string("archive holds an entry that is not a plain file or folder (type '") + type + "'): symlinks, hard links and devices are refused"; return false; }
    pos = body + (size + 511) / 512 * 512;
  }
  *err = "tar has no end marker"; return false;
}
}  // namespace

bool gunzip(const std::string& in, std::string* out, std::size_t max_out, std::string* err) {
  out->clear();
  if (in.size() < 18 || (uint8_t)in[0] != 0x1f || (uint8_t)in[1] != 0x8b || in[2] != 8) { *err = "not a gzip stream"; return false; }
  uint8_t flg = (uint8_t)in[3]; std::size_t p = 10;
  if (flg & 4) { if (p + 2 > in.size()) { *err = "bad gzip header"; return false; } p += 2 + ((uint8_t)in[p] | ((uint8_t)in[p + 1] << 8)); }
  if (flg & 8) { while (p < in.size() && in[p]) ++p; ++p; }
  if (flg & 16) { while (p < in.size() && in[p]) ++p; ++p; }
  if (flg & 2) p += 2;
  if (p + 8 > in.size()) { *err = "bad gzip header"; return false; }
  Bits b{(const uint8_t*)in.data() + p, in.size() - p - 8};
  if (!inflate_raw(b, out, max_out, err)) return false;
  const uint8_t* tr = (const uint8_t*)in.data() + in.size() - 8;
  uint32_t crc = tr[0] | (tr[1] << 8) | (tr[2] << 16) | ((uint32_t)tr[3] << 24);
  if (crc != crc32(*out)) { *err = "gzip checksum mismatch"; return false; }
  return true;
}

bool extract_archive(const std::string& bytes, const std::string& dest, const ArchiveLimits& lim, std::string* err) {
  std::error_code ec;
  if (fs::exists(dest, ec)) { *err = "destination already exists"; return false; }
  std::string tar = bytes;
  if (bytes.size() > 2 && (uint8_t)bytes[0] == 0x1f && (uint8_t)bytes[1] == 0x8b) { if (!gunzip(bytes, &tar, lim.max_total_bytes + (1u << 20), err)) return false; }
  std::vector<Ent> ents;
  if (!read_tar(tar, lim, &ents, err)) return false;
  for (auto& e : ents) {  // refuse unsafe raw paths before anything is stripped or skipped
    bool unsafe = e.path.empty() || e.path[0] == '/' || e.path.find('\\') != std::string::npos || e.path.find('\0') != std::string::npos;
    std::size_t s = 0;
    while (!unsafe && s <= e.path.size()) { std::size_t q = e.path.find('/', s); if (q == std::string::npos) q = e.path.size(); if (e.path.substr(s, q - s) == "..") unsafe = true; s = q + 1; }
    if (unsafe) { *err = "unsafe path in archive: '" + e.path.substr(0, 80) + "'"; return false; }
  }
  // strip one shared top-level folder
  std::string top; bool shared = !ents.empty();
  for (auto& e : ents) { std::size_t s = e.path.find('/'); std::string f = s == std::string::npos ? (e.dir ? e.path : std::string()) : e.path.substr(0, s); if (f.empty() || (!top.empty() && f != top)) { shared = false; break; } top = f; }
  std::set<std::string> seen;
  if (lim.skip_hidden) {
    std::vector<Ent> kept;
    for (auto& e : ents) {
      std::string rel = (shared && e.path.size() > top.size()) ? e.path.substr(top.size() + 1) : e.path;
      bool hid = false; std::size_t s = 0;
      while (s <= rel.size()) { std::size_t q = rel.find('/', s); if (q == std::string::npos) q = rel.size(); if (q > s && rel[s] == '.' && rel.substr(s, q - s) != "." && rel.substr(s, q - s) != "..") hid = true; s = q + 1; }
      if (!hid) kept.push_back(std::move(e));
    }
    ents.swap(kept);
  }
  for (auto& e : ents) {
    std::string rel = e.path;
    if (shared) { if (rel == top) continue; rel = rel.substr(top.size() + 1); }
    if (!valid_relative_path(rel)) { *err = "unsafe path in archive: '" + e.path.substr(0, 80) + "'"; return false; }
    e.path = rel;
    if (!seen.insert(rel).second) { *err = "archive lists a path twice: " + rel; return false; }
  }
  fs::create_directories(dest, ec);
  for (auto& e : ents) {
    if (shared && e.path == top) continue;
    fs::path f = fs::path(dest) / e.path;
    if (e.dir) { fs::create_directories(f, ec); continue; }
    fs::create_directories(f.parent_path(), ec);
    std::ofstream o(f, std::ios::binary); o << e.data;
    if (!o) { fs::remove_all(dest, ec); *err = "cannot write " + e.path; return false; }
  }
  return true;
}

bool extract_archive_file(const std::string& path, const std::string& dest, const ArchiveLimits& lim, std::string* err) {
  std::error_code ec;
  auto sz = fs::file_size(path, ec);
  if (ec) { *err = "cannot read " + path; return false; }
  if (sz > lim.max_total_bytes) { *err = "archive file is too large"; return false; }
  std::ifstream in(path, std::ios::binary); std::stringstream ss; ss << in.rdbuf();
  bool ok = extract_archive(ss.str(), dest, lim, err);
  if (!ok) fs::remove_all(dest, ec);
  return ok;
}

}  // namespace pnr::plugin
