// Slice S25 tests: BC6H block decode, byte-exact. Structural expectations
// are hand-computed from the public BC6H specification; the captured vectors
// in gfx_bc6h_vectors.inc were produced after the decoder matched bcdec's
// reference decode on 2,000,000 random blocks per sign. Pure host-side, no
// Vulkan.
#include <cstdint>
#include <cstdio>
#include <string>

#include "graphics/gfx_bc6h.hpp"

namespace {
using namespace pnr::graphics;
using namespace pnr::graphics::bc6h_detail;
#include "gfx_bc6h_vectors.inc"

// LSB-first field writer for hand-built blocks.
struct W {
  std::uint8_t b[16] = {};
  int pos = 0;
  void put(std::uint32_t v, int n) {
    for (int i = 0; i < n; ++i, ++pos)
      b[pos >> 3] |= std::uint8_t(((v >> i) & 1) << (pos & 7));
  }
};

// Encode a block through the mode's field descriptor (the same
// script-transcribed table the decoder walks, so the layout cannot drift
// from the spec by a hand mistake): ep[w/x/y/z][r/g/b], partition, then
// caller appends the pixel indices.
void encode(const Bc6hInfo& info, int info_idx, const std::uint32_t ep[4][3],
            std::uint32_t partition, W& w) {
  for (int i = 0; i < 82; ++i) {
    const std::uint8_t f = kBc6hDesc[info_idx][i][0], b = kBc6hDesc[info_idx][i][1];
    if (f == 2) continue;  // NA
    if (f == 0) { w.put((info.mode >> b) & 1, 1); continue; }
    if (f == 1) { w.put((partition >> b) & 1, 1); continue; }
    w.put((ep[(f - 3) / 3][(f - 3) % 3] >> b) & 1, 1);
  }
}

}  // namespace

void pnr_gfx_bc6h_tests(int& pass, int& fail) {
  auto check = [&](bool ok, const std::string& l) { if (ok) ++pass; else ++fail; std::printf("%s %s\n", ok ? "PASS" : "FAIL", l.c_str()); };

  // Reserved mode (0x13 = 10011): zeros in every channel except alpha.
  {
    std::uint8_t blk[16] = {}, out[64];
    blk[0] = 0x13; blk[5] = 0xFF;  // junk elsewhere must not matter
    decode_bc6h_block(blk, out, false);
    bool ok = true;
    for (int i = 0; i < 16; ++i)
      ok &= out[i*4] == 0 && out[i*4+1] == 0 && out[i*4+2] == 0 && out[i*4+3] == 255;
    check(ok, "bc6h: reserved mode decodes to black with full alpha");
  }

  // Mode 11 (0x03, one subset, untransformed, 10-bit endpoints, 4-bit
  // indices): solid max endpoints. unquantize(1023) = 0xFFFF, interpolate is
  // the identity on equal endpoints, finish scales by 31/64 -> 0x7BFF, the
  // largest finite half (65504) -> clamps to 255.
  {
    const int info_idx = 10;  // mode 0x03
    const Bc6hInfo& info = kBc6hInfo[info_idx];
    std::uint32_t ep[4][3] = {};
    ep[0][0] = ep[0][1] = ep[0][2] = 1023;
    ep[1][0] = ep[1][1] = ep[1][2] = 1023;
    W w; encode(info, info_idx, ep, 0, w);
    for (int i = 0; i < 16; ++i) w.put(0, i == 0 ? 3 : 4);
    std::uint8_t out[64];
    decode_bc6h_block(w.b, out, false);
    bool ok = true;
    for (int i = 0; i < 64; ++i) ok &= out[i] == 255;
    check(ok, "bc6h: mode 11 solid max endpoints decode to white");
  }

  // Same block, zero endpoints -> black, alpha 255.
  {
    const int info_idx = 10;
    const Bc6hInfo& info = kBc6hInfo[info_idx];
    std::uint32_t ep[4][3] = {};
    W w; encode(info, info_idx, ep, 0, w);
    for (int i = 0; i < 16; ++i) w.put(0, i == 0 ? 3 : 4);
    std::uint8_t out[64];
    decode_bc6h_block(w.b, out, false);
    bool ok = true;
    for (int i = 0; i < 16; ++i)
      ok &= out[i*4] == 0 && out[i*4+1] == 0 && out[i*4+2] == 0 && out[i*4+3] == 255;
    check(ok, "bc6h: mode 11 zero endpoints decode to black with full alpha");
  }

  // Signed decode of the same shape with w = -1 (sign bit set in 10 bits):
  // unquantize(-1) -> small negative -> negative half -> clamps to 0.
  {
    const int info_idx = 10;
    const Bc6hInfo& info = kBc6hInfo[info_idx];
    std::uint32_t ep[4][3] = {};
    ep[0][0] = ep[0][1] = ep[0][2] = 1023;  // -1 in signed 10-bit
    ep[1][0] = ep[1][1] = ep[1][2] = 1023;
    W w; encode(info, info_idx, ep, 0, w);
    for (int i = 0; i < 16; ++i) w.put(0, i == 0 ? 3 : 4);
    std::uint8_t out[64];
    decode_bc6h_block(w.b, out, true);
    bool ok = true;
    for (int i = 0; i < 16; ++i)
      ok &= out[i*4] == 0 && out[i*4+1] == 0 && out[i*4+2] == 0 && out[i*4+3] == 255;
    check(ok, "bc6h: signed negative endpoints clamp to black");
  }

  // Mode 1 (0x00, two subsets, transformed, 10-bit w + 5-bit deltas),
  // partition 0 = {0,0,1,1, 0,0,1,1, 0,0,1,1, 0,0,1,1} with anchors at
  // pixels 0 and 15. Subset 0 solid red (w = (1023,0,0), delta x = 0),
  // subset 1 solid blue via delta wrap: y = (1 + 1023) & 1023 = 0 red,
  // by = 1023 with bw = 0 -> blue; delta z = 0 -> z = y.
  {
    const int info_idx = 0;  // mode 0x00
    const Bc6hInfo& info = kBc6hInfo[info_idx];
    std::uint32_t ep[4][3] = {};
    ep[0][0] = 1023;                    // w = red max
    ep[1][0] = 0; ep[1][1] = 0; ep[1][2] = 0;   // x delta 0 -> x = w
    ep[2][0] = 1; ep[2][1] = 0; ep[2][2] = 1023;  // y: r wraps to 0, b max
    ep[3][0] = 0; ep[3][1] = 0; ep[3][2] = 0;     // z delta 0 -> z = y
    W w; encode(info, info_idx, ep, 0, w);
    for (int i = 0; i < 16; ++i) w.put(0, (i == 0 || i == 15) ? 2 : 3);
    std::uint8_t out[64];
    decode_bc6h_block(w.b, out, false);
    const std::uint8_t shape0[16] = {0,0,1,1, 0,0,1,1, 0,0,1,1, 0,0,1,1};
    bool ok = true;
    for (int i = 0; i < 16; ++i) {
      const std::uint8_t er = shape0[i] == 0 ? 255 : 0;
      const std::uint8_t eb = shape0[i] == 1 ? 255 : 0;
      ok &= out[i*4] == er && out[i*4+1] == 0 && out[i*4+2] == eb && out[i*4+3] == 255;
    }
    check(ok, "bc6h: mode 1 partition 0 maps pixels to the two subset colours (delta wrap)");
  }

  // Mode 14 (0x0f, one subset, 16-bit endpoints): full-precision white.
  {
    const int info_idx = 13;  // mode 0x0f
    const Bc6hInfo& info = kBc6hInfo[info_idx];
    std::uint32_t ep[4][3] = {};
    ep[0][0] = ep[0][1] = ep[0][2] = 0xFFFF;
    ep[1][0] = ep[1][1] = ep[1][2] = 0xFFFF;
    W w; encode(info, info_idx, ep, 0, w);
    for (int i = 0; i < 16; ++i) w.put(0, i == 0 ? 3 : 4);
    std::uint8_t out[64];
    decode_bc6h_block(w.b, out, false);
    bool ok = true;
    for (int i = 0; i < 64; ++i) ok &= out[i] == 255;
    check(ok, "bc6h: mode 14 full-precision solid decodes to white");
  }

  // Captured known-answer vectors (all 14 modes, both signs, plus random).
  {
    bool ok = true;
    for (int v = 0; v < 32; ++v) {
      std::uint8_t out[64];
      decode_bc6h_block(kBc6hVecBlocks[v], out, kBc6hVecSigned[v]);
      for (int i = 0; i < 64; ++i)
        if (out[i] != kBc6hVecExpected[v][i]) { ok = false; std::printf("vector %d byte %d: got %d want %d\n", v, i, out[i], kBc6hVecExpected[v][i]); }
    }
    check(ok, "bc6h: 32 captured vectors (14 modes x unsigned/signed + 4 random) decode byte-exact");
  }
}
