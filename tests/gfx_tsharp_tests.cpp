// Slice S19 tests: T#/S# descriptor decode + validation. Pure host-side
// rules, no Vulkan device needed. Descriptor field layout values follow
// public shadPS4 src/video_core/amdgpu/resource.h (values only).
#include <cstdint>
#include <cstdio>
#include <string>

#include "graphics/gfx_tsharp.hpp"

namespace {
using namespace pnr::graphics;

// Build the 8 dwords of a Color2D 8_8_8_8 unorm linear T# by hand.
void make_tsharp(std::uint32_t w[8], std::uint64_t base, std::uint32_t width,
                 std::uint32_t height, std::uint32_t pitch, std::uint32_t tiling) {
  std::uint64_t q0 = (base >> 8) & 0x3FFFFFFFFFull;
  q0 |= std::uint64_t(10) << 52;  // data_format 8_8_8_8
  q0 |= std::uint64_t(0) << 58;   // num_format unorm
  std::uint64_t q1 = std::uint64_t(width - 1) | (std::uint64_t(height - 1) << 14);
  q1 |= std::uint64_t(0) << 32 | std::uint64_t(1) << 35 | std::uint64_t(2) << 38 | std::uint64_t(3) << 41;
  q1 |= std::uint64_t(tiling & 0x1F) << 52;
  q1 |= std::uint64_t(9) << 60;  // Color2D (valid bit set)
  std::uint64_t q2 = std::uint64_t(pitch - 1) << 13;
  w[0] = std::uint32_t(q0); w[1] = std::uint32_t(q0 >> 32);
  w[2] = std::uint32_t(q1); w[3] = std::uint32_t(q1 >> 32);
  w[4] = std::uint32_t(q2); w[5] = std::uint32_t(q2 >> 32);
  w[6] = 0; w[7] = 0;
}

}  // namespace

void pnr_gfx_tsharp_tests(int& pass, int& fail) {
  auto check = [&](bool ok, const std::string& l) { if (ok) ++pass; else ++fail; std::printf("%s %s\n", ok ? "PASS" : "FAIL", l.c_str()); };

  std::uint32_t w[8];
  make_tsharp(w, 0x100000, 64, 32, 64, 0);
  TSharp t;
  check(decode_tsharp(w, &t), "tsharp: Color2D descriptor decodes (valid bit set)");
  check(t.base == 0x100000 && t.width == 64 && t.height == 32 && t.pitch == 64,
        "tsharp: base, width, height and pitch come back exactly");
  check(t.data_format == 10 && t.num_format == 0 && t.type == 9 && t.tiling_index == 0,
        "tsharp: format, type and tiling index decode");
  check(t.dst_sel[0] == 0 && t.dst_sel[1] == 1 && t.dst_sel[2] == 2 && t.dst_sel[3] == 3,
        "tsharp: identity component select decodes");
  w[3] &= 0x0FFFFFFFu;  // clear the type field (invalid image)
  check(!decode_tsharp(w, &t), "tsharp: a descriptor with the valid bit clear is rejected at decode");

  std::string why;
  make_tsharp(w, 0x100000, 64, 32, 64, 0);
  decode_tsharp(w, &t);
  check(validate_tsharp_sample(t, 0x2000000, &why), "tsharp: linear 8_8_8_8 unorm 2D image validates");
  make_tsharp(w, 0x100000, 64, 32, 64, 13);  // 13 is not a PS5-native SW_MODE
  decode_tsharp(w, &t);
  check(!validate_tsharp_sample(t, 0x2000000, &why) && why.find("tiling") != std::string::npos,
        "tsharp: unknown SW_MODE tiling is refused with a named reason");
  make_tsharp(w, 0x100000, 64, 32, 64, 0);
  w[1] = (w[1] & 0xC3FFFFFFu) | (7u << 26);  // num_format = float(7), q0 bits 58-61
  decode_tsharp(w, &t);
  check(!validate_tsharp_sample(t, 0x2000000, &why) && why.find("format") != std::string::npos,
        "tsharp: non-unorm 8_8_8_8 textures are refused");
  make_tsharp(w, 0x100000, 64, 32, 64, 0);
  w[3] = (w[3] & 0xFFF0FFFFu) | (1u << 16);  // last_level = 1, q1 bits 48-51
  decode_tsharp(w, &t);
  check(t.base_level == 0 && t.last_level == 1, "tsharp: mip chain fields decode");
  check(validate_tsharp_sample(t, 0x2000000, &why),
        "tsharp: a mip chain from level 0 validates (S19)");
  w[3] = (w[3] & 0xFFF00FFFu) | (1u << 12) | (1u << 16);  // base_level = 1, last_level = 1
  decode_tsharp(w, &t);
  check(!validate_tsharp_sample(t, 0x2000000, &why) && why.find("base_level") != std::string::npos,
        "tsharp: chains starting above level 0 are refused");

  // ---- slice S23: the extended plain-format matrix ----
  auto set_fmt = [&](std::uint32_t df, std::uint32_t nf) {
    make_tsharp(w, 0x100000, 64, 32, 64, 0);
    w[1] = (w[1] & ~(0x3Fu << 20) & ~(0xFu << 26)) | (df << 20) | (nf << 26);
    decode_tsharp(w, &t);
  };
  set_fmt(1, 0);  check(validate_tsharp_sample(t, 0x2000000, &why), "tsharp: 8 unorm validates (S23)");
  set_fmt(2, 0);  check(validate_tsharp_sample(t, 0x2000000, &why), "tsharp: 16 unorm validates (S23)");
  set_fmt(2, 7);  check(validate_tsharp_sample(t, 0x2000000, &why), "tsharp: 16 half float validates (S23)");
  set_fmt(3, 0);  check(validate_tsharp_sample(t, 0x2000000, &why), "tsharp: 8_8 unorm validates (S23)");
  set_fmt(4, 7);  check(validate_tsharp_sample(t, 0x2000000, &why), "tsharp: 32 float validates (S23)");
  set_fmt(5, 0);  check(validate_tsharp_sample(t, 0x2000000, &why), "tsharp: 16_16 unorm validates (S23)");
  set_fmt(11, 7); check(validate_tsharp_sample(t, 0x2000000, &why), "tsharp: 32_32 float validates (S23)");
  set_fmt(12, 0); check(validate_tsharp_sample(t, 0x2000000, &why), "tsharp: 16_16_16_16 unorm validates (S23)");
  set_fmt(12, 7); check(validate_tsharp_sample(t, 0x2000000, &why), "tsharp: 16_16_16_16 float validates (S23)");
  set_fmt(14, 7); check(validate_tsharp_sample(t, 0x2000000, &why), "tsharp: 32_32_32_32 float validates (S23)");
  set_fmt(16, 0); check(validate_tsharp_sample(t, 0x2000000, &why), "tsharp: 5_6_5 unorm validates (S23)");
  set_fmt(10, 9); check(validate_tsharp_sample(t, 0x2000000, &why), "tsharp: 8_8_8_8 sRGB validates (S23)");
  set_fmt(35, 9); check(validate_tsharp_sample(t, 0x2000000, &why), "tsharp: BC1 sRGB validates (S23)");
  set_fmt(36, 9); check(validate_tsharp_sample(t, 0x2000000, &why), "tsharp: BC2 sRGB validates (S23)");
  set_fmt(37, 9); check(validate_tsharp_sample(t, 0x2000000, &why), "tsharp: BC3 sRGB validates (S23)");
  set_fmt(38, 9);
  check(!validate_tsharp_sample(t, 0x2000000, &why) && why.find("format") != std::string::npos,
        "tsharp: BC4 sRGB is refused with a named reason");
  set_fmt(1, 9);
  check(!validate_tsharp_sample(t, 0x2000000, &why) && why.find("format") != std::string::npos,
        "tsharp: 8 sRGB is refused (only 8_8_8_8/BC1-BC3 go through the sRGB image)");
  set_fmt(3, 7);
  check(!validate_tsharp_sample(t, 0x2000000, &why) && why.find("format") != std::string::npos,
        "tsharp: 8_8 float is refused");
  set_fmt(4, 0);
  check(!validate_tsharp_sample(t, 0x2000000, &why) && why.find("format") != std::string::npos,
        "tsharp: 32 unorm is refused");
  set_fmt(10, 1);
  check(!validate_tsharp_sample(t, 0x2000000, &why) && why.find("format") != std::string::npos,
        "tsharp: 8_8_8_8 snorm is refused");
  set_fmt(9, 0);
  check(!validate_tsharp_sample(t, 0x2000000, &why) && why.find("format") != std::string::npos,
        "tsharp: 2_10_10_10 is refused (packed formats are not implemented)");
  make_tsharp(w, 0x100000, 4, 4, 4, 0);
  w[3] = (w[3] & 0xFFF0FFFFu) | (3u << 16);  // last_level = 3 on a 4x4 (only 3 levels exist)
  decode_tsharp(w, &t);
  check(!validate_tsharp_sample(t, 0x2000000, &why) && why.find("last_level") != std::string::npos,
        "tsharp: last_level beyond the extent's levels is refused");
  make_tsharp(w, 0x100000, 64, 32, 64, 0);
  w[3] = (w[3] & 0xFFF0FFFFu) | (1u << 16);  // chain of 2 levels
  decode_tsharp(w, &t);
  check(!validate_tsharp_sample(t, 0x100000 + 64ull * 32 * 4 + 32ull * 16 * 4 - 1, &why) && why.find("outside") != std::string::npos,
        "tsharp: a mip chain outside the guest address space is refused");
  make_tsharp(w, 0x100000, 64, 32, 64, 0);
  decode_tsharp(w, &t);
  check(!validate_tsharp_sample(t, 0x100000 + 64ull * 32 * 4 - 1, &why) && why.find("outside") != std::string::npos,
        "tsharp: pixels outside the guest address space are refused");

  // S# decode: wrap x, clamp y, bilinear mag, point min, no mips.
  std::uint32_t sw[4] = {0, 0, 0, 0};
  std::uint64_t sq0 = 0 /*wrap x*/ | (std::uint64_t(2) << 3);  // clamp y = ClampLastTexel
  std::uint64_t sq1 = std::uint64_t(1) << 20;                  // mag bilinear
  sw[0] = std::uint32_t(sq0); sw[1] = std::uint32_t(sq0 >> 32);
  sw[2] = std::uint32_t(sq1); sw[3] = std::uint32_t(sq1 >> 32);
  SSharp s;
  check(decode_ssharp(sw, &s) && s.clamp_x == 0 && s.clamp_y == 2 && s.mag_filter == 1 && s.min_filter == 0,
        "ssharp: clamp modes and filters decode");
  check(validate_ssharp_sample(s, &why), "ssharp: wrap/clamp point/bilinear sampler validates");
  sw[2] |= (1u << 26);  // mip_filter = point
  decode_ssharp(sw, &s);
  check(validate_ssharp_sample(s, &why) && s.mip_filter == 1,
        "ssharp: point mip filtering validates (S19)");
  sw[2] = (sw[2] & ~(3u << 26)) | (2u << 26);  // mip_filter = linear
  decode_ssharp(sw, &s);
  check(validate_ssharp_sample(s, &why) && s.mip_filter == 2,
        "ssharp: linear mip filtering validates (S19)");
  sw[1] |= (0x180u);  // min_lod = 1.5 (4.8 fixed), q0 bits 32-43
  sw[1] |= (0x280ull << 12 >> 0) & 0xFFF000u;  // max_lod = 2.5, q0 bits 44-55
  decode_ssharp(sw, &s);
  check(validate_ssharp_sample(s, &why) && s.min_lod == 0x180 && s.max_lod == 0x280,
        "ssharp: min/max lod decode and validate (S19)");
  sw[1] = (sw[1] & 0xFF000FFFu) | (0x100u << 12);  // max_lod = 1.0 < min_lod 1.5
  decode_ssharp(sw, &s);
  check(!validate_ssharp_sample(s, &why) && why.find("max_lod") != std::string::npos,
        "ssharp: max_lod below min_lod is refused");
  sw[1] = 0;
  sw[2] = (sw[2] & ~(3u << 26)) | (3u << 26);  // mip_filter = 3
  decode_ssharp(sw, &s);
  check(!validate_ssharp_sample(s, &why) && why.find("invalid") != std::string::npos,
        "ssharp: mip filter value 3 is refused");
  sw[2] &= ~(3u << 26);
  sw[0] |= (1u << 28);  // mip_point_preclamp
  decode_ssharp(sw, &s);
  check(!validate_ssharp_sample(s, &why) && why.find("preclamp") != std::string::npos,
        "ssharp: mip point preclamp is refused");
  sw[0] &= ~(1u << 28);
  sw[2] |= 0x80;  // lod_bias = 0.5 (6.8 fixed)
  decode_ssharp(sw, &s);
  check(!validate_ssharp_sample(s, &why) && why.find("lod bias") != std::string::npos,
        "ssharp: lod bias is refused");
  sw[2] &= ~0x3FFFu;
  sw[0] |= 6;  // clamp_x = ClampBorder
  decode_ssharp(sw, &s);
  check(!validate_ssharp_sample(s, &why) && why.find("clamp") != std::string::npos,
        "ssharp: border clamp modes are refused");
  sw[0] &= ~7u;
  sw[2] |= (2u << 20);  // mag aniso
  decode_ssharp(sw, &s);
  check(!validate_ssharp_sample(s, &why) && why.find("anisotropic") != std::string::npos,
        "ssharp: anisotropic filtering is refused");
}
