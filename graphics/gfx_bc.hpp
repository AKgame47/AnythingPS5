// PNR Graphics - BC1/BC2/BC3/BC4/BC5 (DXT1/DXT3/DXT5/LATC1/LATC2) block
// decode to RGBA8, GPL-2.0.
// Experimental. The block bit layout is the public S3TC/DXT specification
// (two little-endian RGB565 endpoints + 2-bit indices for BC1; an 8-byte
// alpha block of two endpoints + 3-bit indices for BC3/BC4/BC5; explicit
// 4-bit alphas for BC2). The T# data format values (Bc1 = 35 .. Bc5 = 39)
// follow public shadPS4 src/video_core/amdgpu/pixel_format.h (DataFormat) -
// values only, no code.
// Decode is exact integer arithmetic: no floating point, no rounding modes.
#pragma once
#include <cstdint>

namespace pnr::graphics {

inline constexpr std::uint32_t kFormatBc1 = 35;
inline constexpr std::uint32_t kFormatBc2 = 36;
inline constexpr std::uint32_t kFormatBc3 = 37;
inline constexpr std::uint32_t kFormatBc4 = 38;
inline constexpr std::uint32_t kFormatBc5 = 39;

// Expand one RGB565 endpoint to 8-bit channels (bit replication).
inline void rgb565_to_rgb8(std::uint16_t c, std::uint8_t* rgb) {
  const std::uint32_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
  rgb[0] = std::uint8_t((r << 3) | (r >> 2));
  rgb[1] = std::uint8_t((g << 2) | (g >> 4));
  rgb[2] = std::uint8_t((b << 3) | (b >> 2));
}

// Decode one 8-byte BC1 colour block into 16 RGBA8 pixels (row major).
// four_colour: the BC1 4-colour ramp; false selects the 3-colour +
// transparent mode used when the endpoints order that way. BC3's colour
// half always uses the 4-colour ramp.
inline void decode_bc1_colour(const std::uint8_t* block, bool four_colour, std::uint8_t* out) {
  const std::uint16_t c0 = std::uint16_t(block[0] | (block[1] << 8));
  const std::uint16_t c1 = std::uint16_t(block[2] | (block[3] << 8));
  const std::uint32_t bits = std::uint32_t(block[4]) | (std::uint32_t(block[5]) << 8) |
                             (std::uint32_t(block[6]) << 16) | (std::uint32_t(block[7]) << 24);
  std::uint8_t pal[4][4];
  rgb565_to_rgb8(c0, pal[0]);
  rgb565_to_rgb8(c1, pal[1]);
  pal[0][3] = 255; pal[1][3] = 255;
  const bool four = four_colour || c0 > c1;
  if (four) {
    for (int ch = 0; ch < 3; ++ch) {
      pal[2][ch] = std::uint8_t((2 * pal[0][ch] + pal[1][ch]) / 3);
      pal[3][ch] = std::uint8_t((pal[0][ch] + 2 * pal[1][ch]) / 3);
    }
    pal[2][3] = 255; pal[3][3] = 255;
  } else {
    for (int ch = 0; ch < 3; ++ch) {
      pal[2][ch] = std::uint8_t((pal[0][ch] + pal[1][ch]) / 2);
      pal[3][ch] = 0;
    }
    pal[2][3] = 255; pal[3][3] = 0;  // index 3 = transparent black
  }
  for (int i = 0; i < 16; ++i) {
    const std::uint32_t idx = (bits >> (2 * i)) & 3;
    out[i * 4 + 0] = pal[idx][0];
    out[i * 4 + 1] = pal[idx][1];
    out[i * 4 + 2] = pal[idx][2];
    out[i * 4 + 3] = pal[idx][3];
  }
}

// Decode one 8-byte BC1 block (endpoint order selects the mode).
inline void decode_bc1_block(const std::uint8_t* block, std::uint8_t* out) {
  decode_bc1_colour(block, false, out);
}

// Decode the 8-byte BC3 alpha block into 16 alpha values (row major).
inline void decode_bc3_alpha(const std::uint8_t* block, std::uint8_t* a) {
  const std::uint32_t a0 = block[0], a1 = block[1];
  std::uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) bits |= std::uint64_t(block[2 + i]) << (8 * i);
  std::uint8_t pal[8];
  pal[0] = std::uint8_t(a0); pal[1] = std::uint8_t(a1);
  if (a0 > a1) {
    for (int i = 1; i <= 6; ++i)
      pal[1 + i] = std::uint8_t(((7 - i) * a0 + i * a1) / 7);
  } else {
    for (int i = 1; i <= 4; ++i)
      pal[1 + i] = std::uint8_t(((5 - i) * a0 + i * a1) / 5);
    pal[6] = 0; pal[7] = 255;
  }
  for (int i = 0; i < 16; ++i) a[i] = pal[(bits >> (3 * i)) & 7];
}

// Decode one 16-byte BC3 block (alpha block, then a BC1-style colour block
// that always uses the 4-colour ramp).
inline void decode_bc3_block(const std::uint8_t* block, std::uint8_t* out) {
  decode_bc1_colour(block + 8, true, out);
  std::uint8_t a[16];
  decode_bc3_alpha(block, a);
  for (int i = 0; i < 16; ++i) out[i * 4 + 3] = a[i];
}

// Decode one 16-byte BC2 block: explicit 4-bit alphas (scaled by 17), then
// a BC1-style colour block that always uses the 4-colour ramp.
inline void decode_bc2_block(const std::uint8_t* block, std::uint8_t* out) {
  decode_bc1_colour(block + 8, true, out);
  for (int i = 0; i < 16; ++i) {
    const std::uint32_t v = (block[i / 2] >> (4 * (i & 1))) & 0xF;
    out[i * 4 + 3] = std::uint8_t(v * 17);
  }
}

// Decode one 8-byte BC4 block (same layout as the BC3 alpha block) into 16
// single-channel values.
inline void decode_bc4_values(const std::uint8_t* block, std::uint8_t* v) {
  decode_bc3_alpha(block, v);
}

// Decode one 8-byte BC4 block into RGBA8 (r = value, g = b = 0, a = 255).
inline void decode_bc4_block(const std::uint8_t* block, std::uint8_t* out) {
  std::uint8_t v[16];
  decode_bc4_values(block, v);
  for (int i = 0; i < 16; ++i) {
    out[i * 4 + 0] = v[i]; out[i * 4 + 1] = 0; out[i * 4 + 2] = 0; out[i * 4 + 3] = 255;
  }
}

// Decode one 16-byte BC5 block (two BC4 blocks: red then green).
inline void decode_bc5_block(const std::uint8_t* block, std::uint8_t* out) {
  std::uint8_t r[16], g[16];
  decode_bc4_values(block, r);
  decode_bc4_values(block + 8, g);
  for (int i = 0; i < 16; ++i) {
    out[i * 4 + 0] = r[i]; out[i * 4 + 1] = g[i]; out[i * 4 + 2] = 0; out[i * 4 + 3] = 255;
  }
}

// Decode one level of tightly packed blocks into tight RGBA8 rows.
// w/h in pixels; blocks cover ceil(w/4) x ceil(h/4), edge pixels outside
// w/h are dropped. src_pitch_bytes = bytes per block row in the source.
inline void decode_bc_level(std::uint32_t data_format, const std::uint8_t* src,
                            std::uint64_t src_pitch_bytes, std::uint32_t w, std::uint32_t h,
                            std::uint8_t* dst) {
  const std::uint32_t bb = data_format == kFormatBc1 || data_format == kFormatBc4 ? 8 : 16;
  const std::uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
  std::uint8_t block_px[64];
  for (std::uint32_t by = 0; by < bh; ++by)
    for (std::uint32_t bx = 0; bx < bw; ++bx) {
      const std::uint8_t* blk = src + std::uint64_t(by) * src_pitch_bytes + std::uint64_t(bx) * bb;
      if (data_format == kFormatBc1) decode_bc1_block(blk, block_px);
      else if (data_format == kFormatBc2) decode_bc2_block(blk, block_px);
      else if (data_format == kFormatBc3) decode_bc3_block(blk, block_px);
      else if (data_format == kFormatBc4) decode_bc4_block(blk, block_px);
      else decode_bc5_block(blk, block_px);
      for (std::uint32_t py = 0; py < 4 && by * 4 + py < h; ++py)
        for (std::uint32_t px = 0; px < 4 && bx * 4 + px < w; ++px) {
          const std::uint8_t* s = &block_px[(py * 4 + px) * 4];
          std::uint8_t* d = dst + (std::uint64_t(by * 4 + py) * w + bx * 4 + px) * 4;
          d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        }
    }
}

}  // namespace pnr::graphics
