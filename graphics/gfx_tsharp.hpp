// PNR Graphics - T# (image resource) and S# (sampler) descriptor decode,
// GPL-2.0. Experimental. The 256-bit T# layout (base 38 bits <<8, format,
// size, swizzle, tiling index, levels/arrays) and the 128-bit S# layout
// (clamp modes, filters, lod) follow public shadPS4
// src/video_core/amdgpu/resource.h (Image/Sampler), pixel_format.h
// (DataFormat/NumberFormat) and tiling.h (TileMode) - values only, no code.
// This slice (S19) accepts a narrow honest subset: Color2D, 8_8_8_8 unorm,
// linear tiling (DisplayLinearAligned / DisplayLinearGeneral), no arrays,
// and a mip chain from level 0 packed as level 0 (T# pitch) followed by
// tightly pitched smaller levels. Everything else is refused with a named
// reason.
#pragma once
#include <cstdint>
#include <string>

namespace pnr::graphics {

struct TSharp {
  std::uint64_t base = 0;         // byte address (38-bit field <<8)
  std::uint32_t min_lod = 0;      // 12 bits, 4.8 fixed point
  std::uint32_t data_format = 0;  // 6 bits; 10 = 8_8_8_8
  std::uint32_t num_format = 0;   // 4 bits; 0 = unorm
  std::uint32_t width = 0;        // pixels (field + 1)
  std::uint32_t height = 0;
  std::uint32_t dst_sel[4] = {0, 1, 2, 3};
  std::uint32_t base_level = 0, last_level = 0;
  std::uint32_t tiling_index = 0;  // 8 = DisplayLinearAligned, 31 = DisplayLinearGeneral
  std::uint32_t type = 0;          // 9 = Color2D; bit3 set = valid image
  std::uint32_t pitch = 0;         // pixels per row (field + 1)
  std::uint32_t base_array = 0, last_array = 0;
  bool pow2pad = false;
};

struct SSharp {
  std::uint32_t clamp_x = 0, clamp_y = 0, clamp_z = 0;  // ClampMode values
  std::uint32_t depth_compare = 0;
  std::uint32_t filter_mode = 0;      // 0 = blend (normal filtering)
  std::uint32_t mag_filter = 0, min_filter = 0;  // Filter: 0 point, 1 bilinear
  std::uint32_t mip_filter = 0;       // 0 none, 1 point, 2 linear
  std::uint32_t min_lod = 0, max_lod = 0;  // 12 bits, 4.8 fixed point
  std::uint32_t lod_bias = 0;         // 14 bits, signed 6.8 fixed point
  bool mip_point_preclamp = false;
  bool force_unnormalized = false;
};

// Decode the eight T# dwords. Returns true on a structurally valid image
// descriptor (Valid() bit set); use validate_tsharp_sample for slice rules.
inline bool decode_tsharp(const std::uint32_t w[8], TSharp* t) {
  const std::uint64_t q0 = std::uint64_t(w[0]) | (std::uint64_t(w[1]) << 32);
  const std::uint64_t q1 = std::uint64_t(w[2]) | (std::uint64_t(w[3]) << 32);
  const std::uint64_t q2 = std::uint64_t(w[4]) | (std::uint64_t(w[5]) << 32);
  t->base = (q0 & 0x3FFFFFFFFFull) << 8;
  t->min_lod = std::uint32_t((q0 >> 40) & 0xFFFu);
  t->data_format = std::uint32_t((q0 >> 52) & 0x3Fu);
  t->num_format = std::uint32_t((q0 >> 58) & 0xFu);
  t->width = std::uint32_t(q1 & 0x3FFFu) + 1;
  t->height = std::uint32_t((q1 >> 14) & 0x3FFFu) + 1;
  t->dst_sel[0] = std::uint32_t((q1 >> 32) & 7);
  t->dst_sel[1] = std::uint32_t((q1 >> 35) & 7);
  t->dst_sel[2] = std::uint32_t((q1 >> 38) & 7);
  t->dst_sel[3] = std::uint32_t((q1 >> 41) & 7);
  t->base_level = std::uint32_t((q1 >> 44) & 0xF);
  t->last_level = std::uint32_t((q1 >> 48) & 0xF);
  t->tiling_index = std::uint32_t((q1 >> 52) & 0x1F);
  t->pow2pad = ((q1 >> 57) & 1) != 0;
  t->type = std::uint32_t((q1 >> 60) & 0xF);
  t->pitch = std::uint32_t((q2 >> 13) & 0x3FFFu) + 1;
  t->base_array = std::uint32_t((q2 >> 32) & 0x1FFFu);
  t->last_array = std::uint32_t((q2 >> 45) & 0x1FFFu);
  return (t->type & 0x8u) != 0;
}

// Slice S19 sampling rules. guest_bytes = size of the guest address space
// for the bounds check on base + mip chain bytes.
inline bool validate_tsharp_sample(const TSharp& t, std::uint64_t guest_bytes, std::string* why) {
  auto no = [&](const char* m) { *why = m; return false; };
  if (t.type != 9) return no("T# image type is not Color2D (1D/3D/cube/array/MSAA images are not implemented)");
  const bool bc1 = t.data_format == 35, bc3 = t.data_format == 37;  // shadPS4 DataFormat values
  if (!(t.data_format == 10 || bc1 || bc3) || t.num_format != 0)
    return no("T# format is not 8_8_8_8/BC1/BC3 unorm (other texture formats are not implemented)");
  if (t.tiling_index != 8 && t.tiling_index != 31)
    return no("T# tiling is not linear (tiled/swizzled textures are not implemented)");
  if (t.base_level != 0)
    return no("T# base_level is not 0 (mip chains starting above level 0 are not implemented)");
  if (t.last_array != 0 || t.base_array != 0)
    return no("T# texture arrays are not implemented");
  if (t.pow2pad) return no("T# pow2pad images are not supported");
  if (t.width < 1 || t.width > 4096 || t.height < 1 || t.height > 4096)
    return no("T# extent is outside 1..4096");
  if (t.pitch < t.width) return no("T# pitch is smaller than the width");
  if (t.last_level >= 9 || (t.width >> t.last_level) == 0 || (t.height >> t.last_level) == 0)
    return no("T# last_level exceeds the number of levels the extent supports");
  if ((bc1 || bc3) && t.pitch != t.width)
    return no("T# compressed textures must be tightly pitched (pitch must equal the width)");
  if ((bc1 || bc3) && ((t.width & 3) != 0 || (t.height & 3) != 0))
    return no("T# compressed extents must be multiples of 4");
  if (t.dst_sel[0] != 0 || t.dst_sel[1] != 1 || t.dst_sel[2] != 2 || t.dst_sel[3] != 3)
    return no("T# component swizzle is not the identity (dst_sel remapping is not implemented)");
  // Guest layout: level 0 at base with the T# pitch, then each smaller
  // level tightly pitched (pitch = level width) one after another. For
  // compressed formats rows are 4x4 blocks: blocks per row = pitch/4,
  // 8 bytes (BC1) or 16 bytes (BC3) per block.
  const std::uint64_t bb = bc1 ? 8 : bc3 ? 16 : 4;
  std::uint64_t bytes = bc1 || bc3 ? std::uint64_t(t.pitch / 4) * (t.height / 4) * bb
                                   : std::uint64_t(t.pitch) * t.height * bb;
  for (std::uint32_t l = 1; l <= t.last_level; ++l) {
    const std::uint32_t lw = t.width >> l ? t.width >> l : 1;
    const std::uint32_t lh = t.height >> l ? t.height >> l : 1;
    bytes += bc1 || bc3 ? std::uint64_t((lw + 3) / 4) * ((lh + 3) / 4) * bb
                        : std::uint64_t(lw) * lh * bb;
  }
  if (t.base + bytes > guest_bytes) return no("T# pixels lie outside the guest address space");
  return true;
}

// Decode the four S# dwords.
inline bool decode_ssharp(const std::uint32_t w[4], SSharp* s) {
  const std::uint64_t q0 = std::uint64_t(w[0]) | (std::uint64_t(w[1]) << 32);
  const std::uint64_t q1 = std::uint64_t(w[2]) | (std::uint64_t(w[3]) << 32);
  s->clamp_x = std::uint32_t(q0 & 7);
  s->clamp_y = std::uint32_t((q0 >> 3) & 7);
  s->clamp_z = std::uint32_t((q0 >> 6) & 7);
  s->depth_compare = std::uint32_t((q0 >> 12) & 7);
  s->force_unnormalized = ((q0 >> 15) & 1) != 0;
  s->mip_point_preclamp = ((q0 >> 28) & 1) != 0;
  s->filter_mode = std::uint32_t((q0 >> 29) & 3);
  s->min_lod = std::uint32_t((q0 >> 32) & 0xFFFu);
  s->max_lod = std::uint32_t((q0 >> 44) & 0xFFFu);
  s->lod_bias = std::uint32_t(q1 & 0x3FFFu);
  s->mag_filter = std::uint32_t((q1 >> 20) & 3);
  s->min_filter = std::uint32_t((q1 >> 22) & 3);
  s->mip_filter = std::uint32_t((q1 >> 26) & 3);
  return true;
}

// Slice S19 sampler rules: wrap/mirror/clamp addressing, point or bilinear
// filters, mip filtering none/point/linear, min/max lod clamps; no depth
// compare, no min/max filter mode, no lod bias or point preclamp.
inline bool validate_ssharp_sample(const SSharp& s, std::string* why) {
  auto no = [&](const char* m) { *why = m; return false; };
  auto clamp_ok = [](std::uint32_t c) { return c <= 3; };
  if (!clamp_ok(s.clamp_x) || !clamp_ok(s.clamp_y))
    return no("S# clamp mode is not wrap/mirror/clamp (border modes are not implemented)");
  if (s.depth_compare != 0) return no("S# depth comparison is not implemented");
  if (s.force_unnormalized) return no("S# unnormalized coordinates are not implemented");
  if (s.filter_mode != 0) return no("S# min/max filter mode is not implemented");
  if (s.mag_filter > 1 || s.min_filter > 1)
    return no("S# anisotropic filtering is not implemented");
  if (s.mip_filter > 2) return no("S# mip filter value 3 is invalid");
  if (s.mip_point_preclamp) return no("S# mip point preclamp is not implemented");
  if (s.lod_bias != 0) return no("S# lod bias is not implemented");
  if (s.max_lod != 0 && s.max_lod < s.min_lod)
    return no("S# max_lod is smaller than min_lod");
  return true;
}

}  // namespace pnr::graphics
