// PNR Graphics - V# (buffer resource) descriptor decode for vertex fetch,
// GPL-2.0. Experimental. The 128-bit layout (base 44 bits, stride 14,
// num_records, dst_sel/numerics, data formats) follows public shadPS4
// src/video_core/amdgpu/resource.h (Buffer) and pixel_format.h
// (DataFormat/NumberFormat) - values only, no code. A real GNM draw hands
// V# descriptors to a generated fetch shader; PNR's fetch kernels are
// likewise built per layout (make_pnr_vs5_blob) and the header carries the
// compiled layout for validation.
#pragma once
#include <cstdint>
#include <string>

namespace pnr::graphics {

struct VSharp {
  std::uint64_t base = 0;        // 44-bit byte address
  std::uint32_t stride = 0;      // bytes between records (0 = tightly packed)
  std::uint32_t num_records = 0;
  std::uint32_t dst_sel[4] = {0, 1, 2, 3};
  std::uint32_t num_format = 0;  // 7 = float (public GNM NumberFormat)
  std::uint32_t data_format = 0; // 4/11/13/14 = 32/32_32/32_32_32/32_32_32_32
  bool swizzle_enable = false, add_tid = false, type_valid = true;
};

// Float component count of a supported vertex data format (0 = unsupported).
inline unsigned vsharp_components(std::uint32_t data_format) {
  switch (data_format) {
    case 4: return 1;    // Format32
    case 11: return 2;   // Format32_32
    case 13: return 3;   // Format32_32_32
    case 14: return 4;   // Format32_32_32_32
    default: return 0;
  }
}

// Decode the four dwords. Returns true on a structurally valid descriptor;
// use validate_vsharp_fetch for the fetch-stage rules.
inline bool decode_vsharp(const std::uint32_t w[4], VSharp* v) {
  const std::uint64_t w01 = std::uint64_t(w[0]) | (std::uint64_t(w[1]) << 32);
  v->base = w01 & 0x0000FFFFFFFFFFFull;          // 44 bits
  v->stride = std::uint32_t((w01 >> 48) & 0x3FFFu);  // 14 bits
  v->swizzle_enable = (w01 >> 63) & 1;
  v->num_records = w[2];
  v->dst_sel[0] = w[3] & 7;
  v->dst_sel[1] = (w[3] >> 3) & 7;
  v->dst_sel[2] = (w[3] >> 6) & 7;
  v->dst_sel[3] = (w[3] >> 9) & 7;
  v->num_format = (w[3] >> 12) & 7;
  v->data_format = (w[3] >> 15) & 0xF;
  v->add_tid = (w[3] >> 23) & 1;
  v->type_valid = ((w[3] >> 30) & 3) == 0;
  return v->num_records != 0;
}

// Fetch-stage rules for this slice: plain float vertex data, identity
// swizzle, no linear/tiled extras. need = components the stage consumes.
inline bool validate_vsharp_fetch(const VSharp& v, unsigned need, std::uint32_t vertices,
                                  std::uint32_t expect_stride_words, std::string* why) {
  auto no = [&](const char* m) { *why = m; return false; };
  if (!v.type_valid) return no("V# type field is nonzero (a buffer descriptor must be type 0)");
  if (v.swizzle_enable) return no("V# swizzled buffers are not supported");
  if (v.add_tid) return no("V# add_tid_enable buffers are not supported");
  if (v.num_format != 7) return no("V# number format is not float (unorm/snorm/integer vertex data is not implemented)");
  const unsigned comps = vsharp_components(v.data_format);
  if (comps == 0) return no("V# data format is not a supported 32-bit float format (8/16-bit and packed formats are not implemented)");
  if (comps < need) return no("V# data format has fewer components than the stage needs");
  const std::uint32_t stride_words = v.stride ? v.stride / 4 : comps;
  if (v.stride && (v.stride % 4)) return no("V# stride is not a whole number of dwords");
  if (stride_words > 16) return no("V# stride over 64 bytes is not supported");
  if (stride_words != expect_stride_words)
    return no("V# stride does not match the fetch blob's compiled layout (fetch shaders are built per layout)");
  if (v.num_records < vertices) return no("V# num_records is smaller than the draw's vertex count");
  if (v.dst_sel[0] != 0 || v.dst_sel[1] != 1 || v.dst_sel[2] != 2 || v.dst_sel[3] != 3)
    return no("V# component swizzle is not the identity (dst_sel remapping is not implemented)");
  return true;
}

}  // namespace pnr::graphics
