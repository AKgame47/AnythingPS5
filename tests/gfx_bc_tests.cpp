// Slice S20 tests: BC1/BC3 block decode, byte-exact against hand-computed
// S3TC/DXT results. Pure host-side, no Vulkan device needed.
#include <cstdint>
#include <cstdio>
#include <string>

#include "graphics/gfx_bc.hpp"

namespace {
using namespace pnr::graphics;

void put_block(std::uint8_t* b, std::uint16_t c0, std::uint16_t c1, std::uint32_t bits) {
  b[0] = std::uint8_t(c0); b[1] = std::uint8_t(c0 >> 8);
  b[2] = std::uint8_t(c1); b[3] = std::uint8_t(c1 >> 8);
  b[4] = std::uint8_t(bits); b[5] = std::uint8_t(bits >> 8);
  b[6] = std::uint8_t(bits >> 16); b[7] = std::uint8_t(bits >> 24);
}

}  // namespace

void pnr_gfx_bc_tests(int& pass, int& fail) {
  auto check = [&](bool ok, const std::string& l) { if (ok) ++pass; else ++fail; std::printf("%s %s\n", ok ? "PASS" : "FAIL", l.c_str()); };

  // BC1 4-colour ramp: red (0xF800) > blue (0x001F). pal2 = (2r+b)/3,
  // pal3 = (r+2b)/3, computed on the expanded 8-bit channels.
  {
    std::uint8_t blk[8], out[64];
    put_block(blk, 0xF800, 0x001F, 0xE4);  // row 0 indices 0,1,2,3; rest 0
    decode_bc1_block(blk, out);
    check(out[0] == 255 && out[1] == 0 && out[2] == 0 && out[3] == 255, "bc1: index 0 is the first endpoint");
    check(out[4] == 0 && out[5] == 0 && out[6] == 255 && out[7] == 255, "bc1: index 1 is the second endpoint");
    check(out[8] == 170 && out[9] == 0 && out[10] == 85, "bc1: index 2 is (2c0+c1)/3 exactly");
    check(out[12] == 85 && out[13] == 0 && out[14] == 170, "bc1: index 3 is (c0+2c1)/3 exactly");
    check(out[16] == 255 && out[17] == 0 && out[18] == 0, "bc1: later pixels keep index 0");
  }
  // BC1 3-colour + transparent mode (c0 <= c1).
  {
    std::uint8_t blk[8], out[64];
    put_block(blk, 0x001F, 0xF800, 0xE4);
    decode_bc1_block(blk, out);
    check(out[8] == 127 && out[9] == 0 && out[10] == 127 && out[11] == 255,
          "bc1: 3-colour mode index 2 is the half mix");
    check(out[12] == 0 && out[13] == 0 && out[14] == 0 && out[15] == 0,
          "bc1: 3-colour mode index 3 is transparent black");
  }
  // BC1 equal endpoints: solid colour, exact RGB565 round trip.
  {
    std::uint8_t blk[8], out[64];
    put_block(blk, 0x07E0, 0x07E0, 0);  // green 565, all pixels index 0
    decode_bc1_block(blk, out);
    check(out[60] == 0 && out[61] == 255 && out[62] == 0, "bc1: pure green round-trips exactly");
  }
  // BC3 alpha, 8-alpha mode (a0 > a1): 6 exact interpolated values.
  {
    std::uint8_t blk[16], out[64];
    blk[0] = 255; blk[1] = 0;
    std::uint64_t abits = 0;
    for (int i = 0; i < 8; ++i) abits |= std::uint64_t(i) << (3 * i);  // pixels 0..7 = alpha 0..7
    for (int i = 0; i < 6; ++i) blk[2 + i] = std::uint8_t(abits >> (8 * i));
    put_block(blk + 8, 0xFFFF, 0xFFFF, 0);  // white colour
    decode_bc3_block(blk, out);
    const std::uint8_t want[8] = {255, 0, 218, 182, 145, 109, 72, 36};
    bool ok = true;
    for (int i = 0; i < 8; ++i) ok &= out[i * 4 + 3] == want[i];
    check(ok, "bc3: 8-alpha mode interpolation is exact");
    check(out[0] == 255 && out[1] == 255 && out[2] == 255, "bc3: colour half decodes independently");
  }
  // BC3 alpha, 6-alpha mode (a0 <= a1): 4 interpolated + 0 + 255.
  {
    std::uint8_t blk[16], out[64];
    blk[0] = 0; blk[1] = 100;
    std::uint64_t abits = 0;
    for (int i = 0; i < 8; ++i) abits |= std::uint64_t(i) << (3 * i);
    for (int i = 0; i < 6; ++i) blk[2 + i] = std::uint8_t(abits >> (8 * i));
    put_block(blk + 8, 0xFFFF, 0xFFFF, 0);
    decode_bc3_block(blk, out);
    const std::uint8_t want[8] = {0, 100, 20, 40, 60, 80, 0, 255};
    bool ok = true;
    for (int i = 0; i < 8; ++i) ok &= out[i * 4 + 3] == want[i];
    check(ok, "bc3: 6-alpha mode interpolation plus 0 and 255 is exact");
  }
  // Also cover the 6-alpha weights with a nonzero a0 (a0 = 200, a1 = 50).
  {
    std::uint8_t a[16];
    std::uint8_t blk[8] = {50, 200, 0, 0, 0, 0, 0, 0};  // a0 <= a1 -> 6-alpha mode
    std::uint64_t abits = 0;
    for (int i = 0; i < 4; ++i) abits |= std::uint64_t(2 + i) << (3 * i);
    for (int i = 0; i < 6; ++i) blk[2 + i] = std::uint8_t(abits >> (8 * i));
    decode_bc3_alpha(blk, a);
    check(a[0] == 80 && a[1] == 110 && a[2] == 140 && a[3] == 170,
          "bc3: 6-alpha weights are (4a0+a1)/5 .. (a0+4a1)/5");
  }
  // BC3 colour always uses the 4-colour ramp, even when c0 <= c1.
  {
    std::uint8_t blk[16], out[64];
    blk[0] = 255; blk[1] = 255;
    for (int i = 2; i < 8; ++i) blk[i] = 0;  // all alpha index 0
    put_block(blk + 8, 0x001F, 0xF800, 0xE4);  // blue <= red, but still 4-colour
    decode_bc3_block(blk, out);
    check(out[8] == 85 && out[9] == 0 && out[10] == 170, "bc3: colour half always uses the 4-colour ramp");
  }
  // Level decode: a 6x4 BC1 level (2x1 blocks) drops pixels outside w.
  {
    std::uint8_t lvl[16], out[6 * 4 * 4];
    put_block(lvl, 0xF800, 0xF800, 0);      // left block red
    put_block(lvl + 8, 0x001F, 0x001F, 0);  // right block blue
    decode_bc_level(kFormatBc1, lvl, 16, 6, 4, out);
    check(out[(0 * 6 + 3) * 4] == 255 && out[(0 * 6 + 4) * 4 + 2] == 255,
          "bc: level decode maps blocks and drops edge pixels");
  }
}
