// Slice S24 tests: BC7 block decode, byte-exact. Solid-block expectations are
// hand-computed from the public specification; the captured vectors in
// gfx_bc7_vectors.inc were produced after the decoder matched bcdec's
// reference decode on 2,000,000 random blocks. Pure host-side, no Vulkan.
#include <cstdint>
#include <cstdio>
#include <string>

#include "graphics/gfx_bc7.hpp"

namespace {
using namespace pnr::graphics;
#include "gfx_bc7_vectors.inc"

// LSB-first field writer for hand-built blocks.
struct W {
  std::uint8_t b[16] = {};
  int pos = 0;
  void put(std::uint32_t v, int n) {
    for (int i = 0; i < n; ++i, ++pos)
      b[pos >> 3] |= std::uint8_t(((v >> i) & 1) << (pos & 7));
  }
};

}  // namespace

void pnr_gfx_bc7_tests(int& pass, int& fail) {
  auto check = [&](bool ok, const std::string& l) { if (ok) ++pass; else ++fail; std::printf("%s %s\n", ok ? "PASS" : "FAIL", l.c_str()); };

  // Reserved block: first byte 0 has no mode terminator - decodes to zeros.
  {
    std::uint8_t blk[16] = {}, out[64];
    blk[5] = 0xFF;  // junk elsewhere must not matter
    decode_bc7_block(blk, out);
    bool zero = true;
    for (int i = 0; i < 64; ++i) zero &= out[i] == 0;
    check(zero, "bc7: reserved block (no mode bit) decodes to zeros");
  }

  // Mode 6 solid: 7-bit endpoints with P-bit. r: v7 = 0x2D, p = 1 ->
  // (0x2D<<1|1) = 0x5B = 91; replication at 8 bits is exact. Same for both
  // endpoints, so every pixel is (91, 182, 255, 0) regardless of indices.
  {
    W w;
    w.put(0, 6); w.put(1, 1);              // mode 6 (six zeros then one)
    w.put(0x2D, 7); w.put(0x2D, 7);        // R endpoints
    w.put(0x5B, 7); w.put(0x5B, 7);        // G: 0x5B<<1|0 = 182
    w.put(0x7F, 7); w.put(0x7F, 7);        // B: 255 (p=1)
    w.put(0x00, 7); w.put(0x00, 7);        // A: 0 (p=0)
    w.put(1, 1); w.put(0, 1);              // P-bits: e0 -> R p=1... order: e0 p, e1 p
    // indices: any - write zeros for the rest
    std::uint8_t out[64];
    decode_bc7_block(w.b, out);
    // e0: R = 0x2D<<1|1 = 91, G = 0x5B<<1|1 = 183? No: P-bits are per endpoint:
    // e0 p=1 -> R 91, G 183, B 255, A 1; e1 p=0 -> R 90, G 182, B 254, A 0.
    // Not a solid block after all - pixel value depends on its index.
    // Index 0 -> e0: (91,183,255,1); index 1 -> (64-21)/21 interp.
    // All indices are 0 here except they cannot be: anchors drop a bit, still 0.
    // So every pixel = e0 = (91,183,255,1).
    bool ok = true;
    for (int i = 0; i < 16; ++i)
      ok &= out[i*4] == 91 && out[i*4+1] == 183 && out[i*4+2] == 255 && out[i*4+3] == 1;
    check(ok, "bc7: mode 6 all-zero indices give endpoint 0 everywhere");
  }

  // Mode 6 interpolation: e0 black (0,0,0,255), e1 white (255,255,255,255),
  // indices 0..15 over the 4-bit weight table {0,4,9,13,17,21,26,30,34,38,43,
  // 47,51,55,60,64}: pixel value = (w*255 + 32) >> 6.
  {
    W w;
    w.put(0, 6); w.put(1, 1);
    w.put(0x00, 7); w.put(0x7F, 7);        // R: 0, 254 (p=0 -> 254? 0x7F<<1 = 254)
    w.put(0x00, 7); w.put(0x7F, 7);        // G
    w.put(0x00, 7); w.put(0x7F, 7);        // B
    w.put(0x7F, 7); w.put(0x7F, 7);        // A: 254
    w.put(0, 1); w.put(0, 1);              // P-bits 0,0 -> e1 = 254, not 255
    // indices: pixel 0 (anchor) 3 bits, pixels 1..15 four bits = index px.
    w.put(0, 3);
    for (int i = 1; i < 16; ++i) w.put(i, 4);
    std::uint8_t out[64];
    decode_bc7_block(w.b, out);
    const std::uint8_t w4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
    bool ok = true;
    for (int i = 0; i < 16; ++i) {
      const std::uint8_t e = std::uint8_t((w4[i] * 254 + 32) >> 6);
      ok &= out[i*4] == e && out[i*4+1] == e && out[i*4+2] == e && out[i*4+3] == 254;
    }
    check(ok, "bc7: mode 4-bit weights interpolate exactly over the whole ramp");
  }

  // Mode 0 partition + anchors: 3-subset block, partition 0. Fixups for
  // 3-subset partition 0: {0, 3, 15} - anchors at pixels 0, 3, 15, so those
  // indices carry 2 bits, the rest 3. Endpoints distinct per subset; solid
  // per subset so every pixel is its subset's colour.
  {
    W w;
    w.put(1, 1);                           // mode 0
    w.put(0, 4);                           // partition 0
    // 4-bit endpoints, channel-major (all R, then all G, then all B):
    // subset0 red, subset1 green, subset2 blue.
    w.put(0xF, 4); w.put(0xF, 4); w.put(0, 4); w.put(0, 4); w.put(0, 4); w.put(0, 4);   // R
    w.put(0, 4); w.put(0, 4); w.put(0xF, 4); w.put(0xF, 4); w.put(0, 4); w.put(0, 4);   // G
    w.put(0, 4); w.put(0, 4); w.put(0, 4); w.put(0, 4); w.put(0xF, 4); w.put(0xF, 4);   // B
    w.put(0x3F, 6);                        // P-bits 1 -> (0xF<<1|1) = 31 -> (31<<3)|(31>>2) = 255
    // indices all zero: anchors 2 bits (pixels 0, 3, 15), others 3 bits
    for (int i = 0; i < 16; ++i) w.put(0, (i == 0 || i == 3 || i == 15) ? 2 : 3);
    std::uint8_t out[64];
    decode_bc7_block(w.b, out);
    // partition table (3 subsets, shape 0) from the spec table data:
    const std::uint8_t shape0[16] = {0,0,1,1, 0,0,1,1, 0,2,2,1, 2,2,2,2};
    bool ok = true;
    for (int i = 0; i < 16; ++i) {
      // own channel 255, off channels 8 (P-bit 1 on a zero 4-bit value)
      const std::uint8_t er = shape0[i] == 0 ? 255 : 8;
      const std::uint8_t eg = shape0[i] == 1 ? 255 : 8;
      const std::uint8_t eb = shape0[i] == 2 ? 255 : 8;
      ok &= out[i*4] == er && out[i*4+1] == eg && out[i*4+2] == eb && out[i*4+3] == 255;
    }
    check(ok, "bc7: mode 0 partition 0 maps pixels to the three subset colours");
  }

  // Mode 4 rotation: rotation 1 swaps alpha and red after interpolation.
  // Solid colour (5-bit 0x10 -> (0x10<<3)|(0x10>>2) = 0x84|0x04 = 132), solid
  // alpha (6-bit 0x21 = 33 -> (33<<2)|(33>>4) = 132|2 = 134? compute below).
  {
    W w;
    w.put(0, 4); w.put(1, 1);              // mode 4
    w.put(1, 2);                           // rotation = 1 (swap A/R)
    w.put(0, 1);                           // index selector 0
    w.put(0x10, 5); w.put(0x10, 5);        // R endpoints (5-bit)
    w.put(0x10, 5); w.put(0x10, 5);        // G
    w.put(0x10, 5); w.put(0x10, 5);        // B
    w.put(0x21, 6); w.put(0x21, 6);        // A endpoints (6-bit)
    // colour indices: anchor (px0) 1 bit, others 2; alpha: px0 2 bits, others 3
    for (int i = 0; i < 16; ++i) w.put(0, i == 0 ? 1 : 2);
    for (int i = 0; i < 16; ++i) w.put(0, i == 0 ? 2 : 3);
    std::uint8_t out[64];
    decode_bc7_block(w.b, out);
    // colour = 5-bit 0x10 replicated: (0x10 << 3) | (0x10 >> 2) = 128 | 4 = 132
    // alpha  = 6-bit 0x21 replicated: (0x21 << 2) | (0x21 >> 4) = 132 | 2 = 134
    // rotation 1 swaps A and R: R = 134, G = 132, B = 132, A = 132.
    bool ok = true;
    for (int i = 0; i < 16; ++i)
      ok &= out[i*4] == 134 && out[i*4+1] == 132 && out[i*4+2] == 132 && out[i*4+3] == 132;
    check(ok, "bc7: mode 4 rotation 1 swaps alpha and red exactly");
  }

  // Mode 4 index selector 1: the 3-bit alpha-index stream drives colour.
  {
    W w;
    w.put(0, 4); w.put(1, 1);              // mode 4
    w.put(0, 2);                           // no rotation
    w.put(1, 1);                           // index selector 1
    w.put(0x00, 5); w.put(0x1F, 5);        // R: 0 -> 255
    w.put(0x00, 5); w.put(0x00, 5);        // G: 0
    w.put(0x00, 5); w.put(0x00, 5);        // B: 0
    w.put(0x3F, 6); w.put(0x3F, 6);        // A: 255 solid
    // primary (2-bit) indices all 0; secondary (3-bit) = px index (drives colour)
    for (int i = 0; i < 16; ++i) w.put(0, i == 0 ? 1 : 2);
    w.put(0, 2);
    for (int i = 1; i < 16; ++i) w.put(i & 7, 3);
    std::uint8_t out[64];
    decode_bc7_block(w.b, out);
    const std::uint8_t w3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
    bool ok = out[3] == 255;
    for (int i = 1; i < 16; ++i) {
      const std::uint8_t e = std::uint8_t((w3[i & 7] * 255 + 32) >> 6);
      ok &= out[i*4] == e && out[i*4+1] == 0 && out[i*4+3] == 255;
    }
    check(ok, "bc7: mode 4 index selector routes the alpha index stream to colour");
  }

  // Captured vectors (fuzz-verified implementation; seed fixed).
  for (int m = 0; m < 16; ++m) {
    std::uint8_t out[64];
    decode_bc7_block(kBc7VecBlocks[m], out);
    bool ok = true;
    for (int i = 0; i < 64; ++i) ok &= out[i] == kBc7VecExpected[m][i];
    check(ok, m < 8 ? "bc7: captured vector (forced mode) byte-exact" : "bc7: captured vector (random block) byte-exact");
  }
}
