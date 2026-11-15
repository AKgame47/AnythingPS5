// PNR - BC6H block decode (T# data format 40, value from public shadPS4
// pixel_format.h): all 14 modes, unsigned and signed.
//
// The per-mode bit-field layout table (gfx_bc6h_tables.inc) was transcribed
// by script from microsoft/DirectXTex DirectXTex/BC6HBC7.cpp (MIT License) -
// ms_aDesc / ms_aInfo / ms_aModeToInfo - never retyped. The transform,
// unquantize and finish steps follow the public BC6H specification (the same
// arithmetic as iOrange/bcdec, MIT License, which this decoder is
// differential-fuzzed against). The 2-subset partition/fixup tables and the
// 3/4-bit weight tables are shared with BC7 (gfx_bc7_tables.inc).
//
// Output lands in the RGBA8 staging chain: decoded half floats are clamped
// to [0, 1] (HDR values above 1.0 clamp - the frame is 8-bit unorm, slice
// S23 note). Alpha is always 255; BC6H has no alpha channel.
#pragma once

#include <cmath>
#include <cstdint>

#include "graphics/gfx_bc7.hpp"  // bc7_detail::Bits, partition/fixup/weight tables

namespace pnr::graphics {

inline constexpr std::uint32_t kFormatBc6 = 40;

namespace bc6h_detail {

#include "graphics/gfx_bc6h_tables.inc"

inline int extend_sign(int v, int bits) { return (v << (32 - bits)) >> (32 - bits); }

// Specification unquantize to 16 bits (same arithmetic as bcdec__unquantize).
inline int unquantize(int val, int bits, bool is_signed) {
  if (!is_signed) {
    if (bits >= 15) return val;
    if (val == 0) return 0;
    if (val == (1 << bits) - 1) return 0xFFFF;
    return ((val << 16) + 0x8000) >> bits;
  }
  if (bits >= 16) return val;
  int s = 0;
  if (val < 0) { s = 1; val = -val; }
  int unq;
  if (val == 0) unq = 0;
  else if (val >= (1 << (bits - 1)) - 1) unq = 0x7FFF;
  else unq = ((val << 15) + 0x4000) >> (bits - 1);
  return s ? -unq : unq;
}

// Scale the interpolated value into the half-float bit pattern (same
// arithmetic as bcdec__finish_unquantize).
inline std::uint16_t finish_unquantize(int val, bool is_signed) {
  if (!is_signed) return std::uint16_t((val * 31) >> 6);  // 31/64
  val = (val < 0) ? -(((-val) * 31) >> 5) : (val * 31) >> 5;  // 31/32
  const std::uint16_t s = val < 0 ? 0x8000 : 0;
  return std::uint16_t(s | (val < 0 ? -val : val));
}

// Exact half -> float (finite values only; BC6H produces no Inf/NaN).
inline float half_to_float(std::uint16_t h) {
  const std::uint32_t sign = (h & 0x8000u) ? 1u : 0u;
  const std::uint32_t exp = (h >> 10) & 0x1F, man = h & 0x3FF;
  float f;
  if (exp == 0) f = float(man) * (1.0f / 16777216.0f);  // subnormal: man * 2^-24
  else f = std::ldexp(1024.0f + float(man), int(exp) - 25);  // (1 + man/1024) * 2^(exp-15)
  return sign ? -f : f;
}

inline std::uint8_t to_u8(float f) {
  if (!(f > 0.0f)) return 0;  // also NaN-safe
  if (f >= 1.0f) return 255;
  return std::uint8_t(f * 255.0f + 0.5f);
}

}  // namespace bc6h_detail

// Decode one 16-byte BC6H block into 16 RGBA8 pixels (row major).
// is_signed: T# number format Sint (5) vs Uint (4).
inline void decode_bc6h_block(const std::uint8_t* block, std::uint8_t* out, bool is_signed) {
  using namespace bc6h_detail;
  using bc7_detail::Bits;
  Bits bits{};
  for (int i = 0; i < 8; ++i) bits.lo |= std::uint64_t(block[i]) << (8 * i);
  for (int i = 0; i < 8; ++i) bits.hi |= std::uint64_t(block[8 + i]) << (8 * i);

  // Mode: 2 bits; if > 1, three more bits above them.
  std::uint32_t mode_val = bits.read(2);
  if (mode_val > 1) mode_val |= bits.read(3) << 2;
  const int info_idx = kBc6hModeToInfo[mode_val];
  if (info_idx < 0) {
    // Reserved mode: zero in every channel except alpha (specification).
    for (int i = 0; i < 16; ++i) {
      out[i * 4 + 0] = 0; out[i * 4 + 1] = 0; out[i * 4 + 2] = 0; out[i * 4 + 3] = 255;
    }
    return;
  }
  const Bc6hInfo& info = kBc6hInfo[info_idx];

  // Walk the mode's field descriptor from bit 0 (re-reads the mode bits and
  // verifies them against the first read).
  Bits bits2{};
  for (int i = 0; i < 8; ++i) bits2.lo |= std::uint64_t(block[i]) << (8 * i);
  for (int i = 0; i < 8; ++i) bits2.hi |= std::uint64_t(block[8 + i]) << (8 * i);
  std::uint32_t ep[4][3] = {};
  std::uint32_t partition = 0, mode_check = 0;
  for (int i = 0; i < 82; ++i) {
    const std::uint8_t f = kBc6hDesc[info_idx][i][0], b = kBc6hDesc[info_idx][i][1];
    if (f == 2) continue;  // NA: table padding, not a stream bit
    const std::uint32_t v = bits2.read(1);
    if (f == 0) { mode_check |= v << b; continue; }
    if (f == 1) { partition |= v << b; continue; }
    ep[(f - 3) / 3][(f - 3) % 3] |= v << b;
  }
  (void)mode_check;

  const int subsets = info.partitions ? 2 : 1;
  const int wbits[3] = {info.wr, info.wg, info.wb};
  const int dbits[3] = {info.dr, info.dg, info.db};
  const bool transformed = info.transformed != 0;

  int e[4][3] = {};
  for (int c = 0; c < 3; ++c)
    e[0][c] = is_signed ? extend_sign(int(ep[0][c]), wbits[c]) : int(ep[0][c]);
  for (int i = 1; i < subsets * 2; ++i)
    for (int c = 0; c < 3; ++c) {
      int v = int(ep[i][c]);
      if (transformed || is_signed) v = extend_sign(v, dbits[c]);
      if (transformed) {
        v = (v + e[0][c]) & ((1 << wbits[c]) - 1);
        if (is_signed) v = extend_sign(v, wbits[c]);
      }
      e[i][c] = v;
    }
  for (int i = 0; i < subsets * 2; ++i)
    for (int c = 0; c < 3; ++c) e[i][c] = unquantize(e[i][c], wbits[c], is_signed);

  const std::uint8_t* weights =
      info.index_prec == 3 ? kBc7Weights3 : kBc7Weights4;
  for (int px = 0; px < 16; ++px) {
    const int subset = info.partitions ? kBc7Partitions[1][partition][px] : 0;
    const int anchor = info.partitions ? kBc7Fixups[1][partition][subset] : 0;
    const int ibits = info.index_prec - (px == anchor ? 1 : 0);
    const std::uint32_t idx = bits2.read(ibits);
    const int w = weights[idx];
    const int base = subset * 2;
    for (int c = 0; c < 3; ++c) {
      const int v = (e[base][c] * (64 - w) + e[base + 1][c] * w + 32) >> 6;
      out[px * 4 + c] = to_u8(half_to_float(finish_unquantize(v, is_signed)));
    }
    out[px * 4 + 3] = 255;
  }
}

}  // namespace pnr::graphics
