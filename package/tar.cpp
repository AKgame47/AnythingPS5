#include "package/tar.hpp"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace pnr::pkg {
namespace {

void octal(char* field, size_t width, uint64_t value) {
  // width includes the required NUL/space terminator
  char tmp[32];
  std::snprintf(tmp, sizeof(tmp), "%0*llo", int(width - 1),
                (unsigned long long)value);
  std::memcpy(field, tmp, width - 1);
  field[width - 1] = '\0';
}

}  // namespace

Status tar_write(const std::string& out_path,
                 const std::vector<TarEntry>& entries) {
  for (const auto& e : entries) {
    if (e.name.empty() || e.name.size() > 100)
      return Status::Fail(50, "pkg.tar",
                          "entry name empty or >100 bytes: " + e.name);
    if (e.name[0] == '/' || e.name.find("..") != std::string::npos)
      return Status::Fail(51, "pkg.tar",
                          "unsafe entry name rejected at write: " + e.name);
  }
  std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
  if (!out) return Status::Fail(52, "pkg.tar", "cannot open for write: " + out_path);

  for (const auto& e : entries) {
    char hdr[512];
    std::memset(hdr, 0, sizeof(hdr));
    std::memcpy(hdr, e.name.c_str(), e.name.size());
    octal(hdr + 100, 8, 0644);                    // mode
    octal(hdr + 108, 8, 0);                       // uid
    octal(hdr + 116, 8, 0);                       // gid
    octal(hdr + 124, 12, e.data.size());          // size
    octal(hdr + 136, 12, 0);                      // mtime
    hdr[156] = '0';                               // typeflag: regular
    std::memcpy(hdr + 257, "ustar", 5);
    hdr[263] = '0'; hdr[264] = '0';               // version "00"
    std::memset(hdr + 148, ' ', 8);               // checksum field = spaces
    uint32_t sum = 0;
    for (size_t i = 0; i < 512; ++i) sum += uint8_t(hdr[i]);
    {                                             // POSIX: 6 octal digits, NUL, space
      char chk[8];
      std::snprintf(chk, sizeof(chk), "%06o", sum);
      std::memcpy(hdr + 148, chk, 6);
      hdr[154] = '\0';
      hdr[155] = ' ';
    }
    out.write(hdr, sizeof(hdr));
    if (!e.data.empty())
      out.write(reinterpret_cast<const char*>(e.data.data()), e.data.size());
    size_t pad = (512 - (e.data.size() % 512)) % 512;
    static const char zeros[512] = {0};
    if (pad) out.write(zeros, pad);
  }
  static const char zeros[512] = {0};
  out.write(zeros, 512);
  out.write(zeros, 512);                          // end-of-archive marker
  if (!out) return Status::Fail(53, "pkg.tar", "write failed: " + out_path);
  return Status::Ok();
}

Status tar_read(const std::string& in_path, std::vector<TarEntry>& out) {
  std::ifstream in(in_path, std::ios::binary);
  if (!in) return Status::Fail(54, "pkg.tar", "cannot open for read: " + in_path);
  out.clear();
  for (;;) {
    char hdr[512];
    in.read(hdr, sizeof(hdr));
    if (in.gcount() != 512)
      return Status::Fail(55, "pkg.tar", "truncated header");
    bool all_zero = true;
    for (char c : hdr) if (c != 0) { all_zero = false; break; }
    if (all_zero) return Status::Ok();            // end marker

    // verify header checksum
    uint32_t stored = 0;
    {
      char chk[9] = {0};
      std::memcpy(chk, hdr + 148, 8);
      stored = uint32_t(strtoul(chk, nullptr, 8));
    }
    uint32_t sum = 0;
    for (size_t i = 0; i < 512; ++i)
      sum += (i >= 148 && i < 156) ? uint8_t(' ') : uint8_t(hdr[i]);
    if (sum != stored)
      return Status::Fail(56, "pkg.tar", "header checksum mismatch");

    char sizebuf[13] = {0};
    std::memcpy(sizebuf, hdr + 124, 12);
    uint64_t size = strtoull(sizebuf, nullptr, 8);
    if (size > (1ull << 32))
      return Status::Fail(57, "pkg.tar", "entry unreasonably large");
    const char type = hdr[156];

    std::string name(hdr, strnlen(hdr, 100));
    if (type == '0' || type == '\0') {
      TarEntry e;
      e.name = name;
      e.data.resize(size);
      if (size) in.read(reinterpret_cast<char*>(e.data.data()), size);
      if (!in) return Status::Fail(58, "pkg.tar", "truncated entry data: " + name);
      out.push_back(std::move(e));
    } else {
      in.seekg(size, std::ios::cur);              // skip non-regular entry
    }
    size_t pad = (512 - (size % 512)) % 512;
    if (pad) in.seekg(pad, std::ios::cur);
    if (!in) return Status::Fail(59, "pkg.tar", "truncated padding");
  }
}

}  // namespace pnr::pkg
