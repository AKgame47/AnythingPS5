// PNR Graphics - plain (uncompressed) sampled texture formats: format info
// and exact conversion to the RGBA8 staging chain, GPL-2.0.
// Experimental. The T# data/number format values follow public shadPS4
// src/video_core/amdgpu/pixel_format.h (DataFormat / NumberFormat) - values
// only, no code. Conversions are exact integer or IEEE bit arithmetic: no
// implementation-defined rounding.
#pragma once
#include <cstdint>
#include "graphics/gfx_bc.hpp"  // rgb565_to_rgb8

namespace pnr::graphics {

// NumberFormat values (shadPS4 pixel_format.h, values only).
inline constexpr std::uint32_t kNumUnorm = 0;
inline constexpr std::uint32_t kNumFloat = 7;
inline constexpr std::uint32_t kNumSrgb = 9;

// One plain-format descriptor for the S23 sampled subset.
struct PlainFormat {
  std::uint32_t element_bytes = 0;  // bytes per texel in guest memory
  std::uint32_t channels = 0;       // meaningful channels; the rest pad to (0,0,1)
  bool is_float = false;            // 16/32-bit IEEE numerics
  bool packed_565 = false;          // df 16: RGB in one 16-bit word
};

// Returns false for data/number format pairs outside the S23 subset:
//   df 1 (8):            unorm
//   df 2 (16):           unorm, float (half)
//   df 3 (8_8):          unorm
//   df 4 (32):           float
//   df 5 (16_16):        unorm, float
//   df 10 (8_8_8_8):     unorm   (sRGB is handled by the host image format)
//   df 11 (32_32):       float
//   df 12 (16_16_16_16): unorm, float
//   df 14 (32_32_32_32): float
//   df 16 (5_6_5):       unorm
inline bool plain_format_info(std::uint32_t data_format, std::uint32_t num_format, PlainFormat& out) {
  out = PlainFormat{};
  switch (data_format) {
    case 1:  // 8
      if (num_format != kNumUnorm) return false;
      out = {1, 1, false, false}; return true;
    case 2:  // 16
      if (num_format == kNumUnorm) { out = {2, 1, false, false}; return true; }
      if (num_format == kNumFloat) { out = {2, 1, true, false}; return true; }
      return false;
    case 3:  // 8_8
      if (num_format != kNumUnorm) return false;
      out = {2, 2, false, false}; return true;
    case 4:  // 32
      if (num_format != kNumFloat) return false;
      out = {4, 1, true, false}; return true;
    case 5:  // 16_16
      if (num_format == kNumUnorm) { out = {4, 2, false, false}; return true; }
      if (num_format == kNumFloat) { out = {4, 2, true, false}; return true; }
      return false;
    case 10:  // 8_8_8_8
      if (num_format != kNumUnorm && num_format != kNumSrgb) return false;
      out = {4, 4, false, false}; return true;
    case 11:  // 32_32
      if (num_format != kNumFloat) return false;
      out = {8, 2, true, false}; return true;
    case 12:  // 16_16_16_16
      if (num_format == kNumUnorm) { out = {8, 4, false, false}; return true; }
      if (num_format == kNumFloat) { out = {8, 4, true, false}; return true; }
      return false;
    case 14:  // 32_32_32_32
      if (num_format != kNumFloat) return false;
      out = {16, 4, true, false}; return true;
    case 16:  // 5_6_5
      if (num_format != kNumUnorm) return false;
      out = {2, 3, false, true}; return true;
    default:
      return false;
  }
}

// True when the pair is sampled through a host sRGB image (bytes pass through
// unchanged; the hardware converts at sample time). BC1-BC3 decode to
// sRGB-encoded RGBA8, so the same image covers them.
inline bool format_uses_srgb_image(std::uint32_t data_format, std::uint32_t num_format) {
  if (num_format != kNumSrgb) return false;
  return data_format == 10 || (data_format >= 35 && data_format <= 37) || data_format == 41;
}

// Exact 16-bit unorm to 8-bit: round(v * 255 / 65535), integer only.
inline std::uint8_t unorm16_to_8(std::uint16_t v) {
  return std::uint8_t((std::uint32_t(v) * 255u + 32767u) / 65535u);
}

// Exact IEEE 754 half to float (subnormals, infinities and NaNs included).
inline float half_to_float(std::uint16_t h) {
  const std::uint32_t sign = std::uint32_t(h & 0x8000u) << 16;
  const std::uint32_t exp = (h >> 10) & 0x1F, man = h & 0x3FF;
  std::uint32_t bits;
  if (exp == 0) {
    if (man == 0) { bits = sign; }
    else {
      // Normalize the subnormal into the float exponent range.
      std::uint32_t m = man; int e = -14;
      while ((m & 0x400) == 0) { m <<= 1; --e; }
      m &= 0x3FF;
      bits = sign | std::uint32_t(e + 127) << 23 | m << 13;
    }
  } else if (exp == 31) {
    bits = sign | 0x7F800000u | man << 13;  // inf / NaN (payload preserved)
  } else {
    bits = sign | (exp + 112) << 23 | man << 13;
  }
  float f;
  __builtin_memcpy(&f, &bits, 4);
  return f;
}

// Exact float to unorm8: NaN and negatives clamp to 0, >= 1 clamps to 255,
// otherwise round-to-nearest (f * 255 + 0.5 truncated).
inline std::uint8_t float_to_unorm8(float f) {
  if (!(f > 0.0f)) return 0;         // also catches NaN
  if (f >= 1.0f) return 255;
  return std::uint8_t(f * 255.0f + 0.5f);
}

// Convert one row of `texels` plain-format texels to RGBA8. src points at the
// row start; df/nf must have passed plain_format_info.
inline void convert_plain_row(std::uint32_t data_format, std::uint32_t num_format,
                              const std::uint8_t* src, std::uint8_t* dst, std::uint32_t texels) {
  PlainFormat pf{};
  if (!plain_format_info(data_format, num_format, pf)) return;
  for (std::uint32_t i = 0; i < texels; ++i) {
    const std::uint8_t* t = src + std::size_t(i) * pf.element_bytes;
    std::uint8_t* o = dst + std::size_t(i) * 4;
    o[0] = 0; o[1] = 0; o[2] = 0; o[3] = 255;
    if (pf.packed_565) {
      const std::uint16_t c = std::uint16_t(t[0] | (t[1] << 8));
      rgb565_to_rgb8(c, o);
      o[3] = 255;
      continue;
    }
    for (std::uint32_t c = 0; c < pf.channels; ++c) {
      std::uint8_t v;
      if (pf.element_bytes / pf.channels == 1) {
        v = t[c];  // 8-bit unorm (and sRGB bytes, unchanged by design)
      } else if (pf.element_bytes / pf.channels == 2) {
        const std::uint16_t w = std::uint16_t(t[c * 2] | (t[c * 2 + 1] << 8));
        v = pf.is_float ? float_to_unorm8(half_to_float(w)) : unorm16_to_8(w);
      } else {
        float f;
        __builtin_memcpy(&f, t + c * 4, 4);
        v = float_to_unorm8(f);
      }
      o[c] = v;
    }
  }
}

}  // namespace pnr::graphics
