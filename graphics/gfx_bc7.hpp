// PNR Graphics - BC7 block decode to RGBA8, GPL-2.0.
// Experimental. The bit layout, mode parameters, partition/fixup tables and
// interpolation weights are the public BC7 specification (Khronos Data Format
// Specification; Microsoft's BC7 documentation). The partition, fixup and
// weight tables in gfx_bc7_tables.inc are factual hardware format data
// extracted verbatim by script from microsoft/DirectXTex BC6HBC7.cpp (MIT) -
// see the .inc header. Decode here is our own; it is exact integer arithmetic
// (no floating point, no rounding modes).
#pragma once
#include <cstdint>

#include "graphics/gfx_bc7_tables.inc"

namespace pnr::graphics {

inline constexpr std::uint32_t kFormatBc7 = 41;

namespace bc7_detail {

struct ModeInfo {
  std::uint8_t subsets;
  std::uint8_t partition_bits;
  std::uint8_t rotation_bits;
  std::uint8_t index_sel_bits;
  std::uint8_t color_bits;
  std::uint8_t alpha_bits;
  std::uint8_t pbit_kind;  // 0 none, 1 per endpoint, 2 shared per subset pair
  std::uint8_t color_index_bits;
  std::uint8_t alpha_index_bits;
};

inline constexpr ModeInfo kModes[8] = {
  /* 0 */ {3, 4, 0, 0, 4, 0, 1, 3, 0},
  /* 1 */ {2, 6, 0, 0, 6, 0, 2, 3, 0},
  /* 2 */ {3, 6, 0, 0, 5, 0, 0, 2, 0},
  /* 3 */ {2, 6, 0, 0, 7, 0, 1, 2, 0},
  /* 4 */ {1, 0, 2, 1, 5, 6, 0, 2, 3},
  /* 5 */ {1, 0, 2, 0, 7, 8, 0, 2, 2},
  /* 6 */ {1, 0, 0, 0, 7, 7, 1, 4, 0},
  /* 7 */ {2, 6, 0, 0, 5, 5, 1, 2, 0},
};

// LSB-first bit reader over the 128-bit block.
struct Bits {
  std::uint64_t lo, hi;
  int pos = 0;
  std::uint32_t read(int n) {
    std::uint32_t v = 0;
    for (int i = 0; i < n; ++i, ++pos) {
      const std::uint64_t w = pos < 64 ? lo : hi;
      v |= std::uint32_t((w >> (pos & 63)) & 1) << i;
    }
    return v;
  }
};

inline std::uint8_t interpolate(std::uint32_t e0, std::uint32_t e1, std::uint32_t w) {
  return std::uint8_t(((64 - w) * e0 + w * e1 + 32) >> 6);
}

}  // namespace bc7_detail

// Decode one 16-byte BC7 block into 16 RGBA8 pixels (row major). A block
// whose first byte is 0 has no mode terminator and decodes to all zeros
// (the specification's reserved-block behaviour).
inline void decode_bc7_block(const std::uint8_t* block, std::uint8_t* out) {
  using namespace bc7_detail;
  Bits bits{};
  for (int i = 0; i < 8; ++i) bits.lo |= std::uint64_t(block[i]) << (8 * i);
  for (int i = 0; i < 8; ++i) bits.hi |= std::uint64_t(block[8 + i]) << (8 * i);

  int mode = 0;
  while (mode < 8 && bits.read(1) == 0) ++mode;
  if (mode >= 8) {
    for (int i = 0; i < 64; ++i) out[i] = 0;
    return;
  }
  const ModeInfo& mi = kModes[mode];
  const int subsets = mi.subsets;
  const int endpoints = subsets * 2;

  const std::uint32_t partition = mi.partition_bits ? bits.read(mi.partition_bits) : 0;
  const std::uint32_t rotation = mi.rotation_bits ? bits.read(mi.rotation_bits) : 0;
  const std::uint32_t index_sel = mi.index_sel_bits ? bits.read(1) : 0;

  std::uint32_t ep[6][4] = {};
  for (int c = 0; c < 3; ++c)
    for (int e = 0; e < endpoints; ++e)
      ep[e][c] = bits.read(mi.color_bits);
  if (mi.alpha_bits)
    for (int e = 0; e < endpoints; ++e)
      ep[e][3] = bits.read(mi.alpha_bits);

  // P-bits: every channel of an endpoint shifts left one and takes the bit.
  if (mi.pbit_kind) {
    for (int e = 0; e < endpoints; ++e)
      for (int c = 0; c < 4; ++c) ep[e][c] <<= 1;
    if (mi.pbit_kind == 2) {  // mode 1: one bit per subset, shared by both endpoints
      for (int s = 0; s < subsets; ++s) {
        const std::uint32_t p = bits.read(1);
        for (int c = 0; c < 3; ++c) { ep[s * 2][c] |= p; ep[s * 2 + 1][c] |= p; }
      }
    } else {
      for (int e = 0; e < endpoints; ++e) {
        const std::uint32_t p = bits.read(1);
        for (int c = 0; c < 4; ++c) ep[e][c] |= p;
      }
    }
  }
  // Expand endpoints to 8 bits by MSB replication.
  const int cbits = mi.color_bits + (mi.pbit_kind ? 1 : 0);
  const int abits = mi.alpha_bits + (mi.pbit_kind ? 1 : 0);
  for (int e = 0; e < endpoints; ++e) {
    for (int c = 0; c < 3; ++c) {
      ep[e][c] <<= (8 - cbits);
      ep[e][c] |= ep[e][c] >> cbits;
    }
    if (mi.alpha_bits) {
      ep[e][3] <<= (8 - abits);
      ep[e][3] |= ep[e][3] >> abits;
    } else {
      ep[e][3] = 255;
    }
  }

  const std::uint8_t* weights = mi.color_index_bits == 2 ? kBc7Weights2
                              : mi.color_index_bits == 3 ? kBc7Weights3 : kBc7Weights4;
  const std::uint8_t* weights2 = mi.alpha_index_bits == 2 ? kBc7Weights2 : kBc7Weights3;

  // Colour indices, raster order; anchor pixels carry one bit fewer.
  std::uint32_t cidx[16];
  for (int px = 0; px < 16; ++px) {
    const int subset = subsets == 1 ? 0 : kBc7Partitions[subsets - 1][partition][px];
    const int anchor = subsets == 1 ? 0 : kBc7Fixups[subsets - 1][partition][subset];
    int n = mi.color_index_bits;
    if (px == anchor) --n;
    cidx[px] = bits.read(n);
  }
  // Alpha indices (modes 4/5): pixel 0 carries one bit fewer.
  std::uint32_t aidx[16] = {};
  if (mi.alpha_index_bits)
    for (int px = 0; px < 16; ++px)
      aidx[px] = bits.read(px == 0 ? mi.alpha_index_bits - 1 : mi.alpha_index_bits);

  for (int px = 0; px < 16; ++px) {
    const int subset = subsets == 1 ? 0 : kBc7Partitions[subsets - 1][partition][px];
    const std::uint32_t* e0 = ep[subset * 2];
    const std::uint32_t* e1 = ep[subset * 2 + 1];
    std::uint8_t rgba[4];
    if (!mi.alpha_index_bits) {
      const std::uint32_t w = weights[cidx[px]];
      for (int c = 0; c < 4; ++c) rgba[c] = interpolate(e0[c], e1[c], w);
    } else {
      // Index selection bit (mode 4) swaps which index stream drives colour.
      const std::uint32_t ci = index_sel ? aidx[px] : cidx[px];
      const std::uint32_t ai = index_sel ? cidx[px] : aidx[px];
      const std::uint8_t* cw = index_sel ? weights2 : weights;
      const std::uint8_t* aw = index_sel ? weights : weights2;
      for (int c = 0; c < 3; ++c) rgba[c] = interpolate(e0[c], e1[c], cw[ci]);
      rgba[3] = interpolate(e0[3], e1[3], aw[ai]);
    }
    switch (rotation) {  // modes 4/5: swap alpha with a channel
      case 1: { const std::uint8_t t = rgba[3]; rgba[3] = rgba[0]; rgba[0] = t; break; }
      case 2: { const std::uint8_t t = rgba[3]; rgba[3] = rgba[1]; rgba[1] = t; break; }
      case 3: { const std::uint8_t t = rgba[3]; rgba[3] = rgba[2]; rgba[2] = t; break; }
      default: break;
    }
    out[px * 4 + 0] = rgba[0];
    out[px * 4 + 1] = rgba[1];
    out[px * 4 + 2] = rgba[2];
    out[px * 4 + 3] = rgba[3];
  }
}

}  // namespace pnr::graphics
