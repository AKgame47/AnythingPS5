// Slice S23 tests: plain texture format info and exact conversion to RGBA8.
// Pure host-side, no Vulkan device needed. Every expected value is
// hand-computed from the format definitions.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "graphics/gfx_formats.hpp"

namespace {
using namespace pnr::graphics;

float f32(std::uint32_t bits) { float f; std::memcpy(&f, &bits, 4); return f; }

}  // namespace

void pnr_gfx_format_tests(int& pass, int& fail) {
  auto check = [&](bool ok, const std::string& l) { if (ok) ++pass; else ++fail; std::printf("%s %s\n", ok ? "PASS" : "FAIL", l.c_str()); };

  // --- format info matrix -------------------------------------------------
  {
    PlainFormat pf{};
    check(plain_format_info(1, 0, pf) && pf.element_bytes == 1 && pf.channels == 1 && !pf.is_float,
          "fmt: 8 unorm is 1 byte, 1 channel");
    check(!plain_format_info(1, 7, pf), "fmt: 8 float is refused");
    check(plain_format_info(2, 0, pf) && pf.element_bytes == 2 && pf.channels == 1 && !pf.is_float &&
          plain_format_info(2, 7, pf) && pf.is_float, "fmt: 16 unorm and half float");
    check(plain_format_info(3, 0, pf) && pf.element_bytes == 2 && pf.channels == 2, "fmt: 8_8 unorm");
    check(plain_format_info(4, 7, pf) && pf.element_bytes == 4 && pf.channels == 1 && pf.is_float,
          "fmt: 32 float");
    check(!plain_format_info(4, 0, pf), "fmt: 32 unorm is refused");
    check(plain_format_info(5, 0, pf) && pf.element_bytes == 4 && pf.channels == 2 &&
          plain_format_info(5, 7, pf) && pf.is_float, "fmt: 16_16 unorm and float");
    check(plain_format_info(10, 0, pf) && pf.element_bytes == 4 && pf.channels == 4 &&
          plain_format_info(10, 9, pf), "fmt: 8_8_8_8 unorm and srgb");
    check(plain_format_info(11, 7, pf) && pf.element_bytes == 8 && pf.channels == 2, "fmt: 32_32 float");
    check(plain_format_info(12, 0, pf) && pf.element_bytes == 8 && pf.channels == 4 &&
          plain_format_info(12, 7, pf) && pf.is_float, "fmt: 16_16_16_16 unorm and float");
    check(plain_format_info(14, 7, pf) && pf.element_bytes == 16 && pf.channels == 4, "fmt: 32_32_32_32 float");
    check(plain_format_info(16, 0, pf) && pf.element_bytes == 2 && pf.channels == 3 && pf.packed_565,
          "fmt: 5_6_5 unorm packed");
    check(!plain_format_info(9, 0, pf) && !plain_format_info(13, 7, pf) && !plain_format_info(35, 0, pf),
          "fmt: 2_10_10_10, 32_32_32 and BC formats are not plain");
    check(!plain_format_info(3, 1, pf) && !plain_format_info(3, 4, pf) && !plain_format_info(10, 1, pf),
          "fmt: snorm/uint numerics are refused");
  }
  {
    check(format_uses_srgb_image(10, 9) && format_uses_srgb_image(35, 9) &&
          format_uses_srgb_image(36, 9) && format_uses_srgb_image(37, 9),
          "fmt: sRGB image covers 8_8_8_8 and BC1-BC3");
    check(!format_uses_srgb_image(38, 9) && !format_uses_srgb_image(39, 9) &&
          !format_uses_srgb_image(1, 9) && !format_uses_srgb_image(10, 0),
          "fmt: sRGB image refuses BC4/BC5, 8 and non-srgb numerics");
  }

  // --- scalar conversions -------------------------------------------------
  {
    check(unorm16_to_8(0) == 0 && unorm16_to_8(65535) == 255, "cvt: unorm16 endpoints");
    // 0x8080 = 32896: 32896*255 = 8388 480; (8388480 + 32767)/65535 = 128.49 -> 128.
    // 0x8081: (32897*255 + 32767)/65535 = (8388735 + 32767)/65535 = 128.50... -> 128
    //         (8421502/65535 = 128.503 -> integer division gives 128).
    // 0x4080 = 16512: (16512*255 + 32767)/65535 = (4210560 + 32767)/65535 = 64.75 -> 64.
    check(unorm16_to_8(0x8080) == 128 && unorm16_to_8(0x4080) == 64, "cvt: unorm16 midpoints round to nearest");
    // 255: (65025 + 32767)/65535 = 1.49 -> 1.  128: (32640 + 32767)/65535 = 0.998 -> 0.
    check(unorm16_to_8(257) == 1 && unorm16_to_8(255) == 1 && unorm16_to_8(128) == 0,
          "cvt: unorm16 257 -> 1, 255 -> 1, 128 -> 0");
  }
  {
    check(half_to_float(0x3C00) == 1.0f && half_to_float(0x0000) == 0.0f && half_to_float(0x8000) == 0.0f,
          "cvt: half 1.0, +0, -0");
    // 0x3555: exp 13 -> 2^-2, man 0x155 = 341/1024 = 0.3330 -> 0.33325...
    check(half_to_float(0x3555) == 0.333251953125f, "cvt: half 0.3333 bit-exact");
    check(half_to_float(0x0001) == 5.9604644775390625e-08f, "cvt: half subnormal bit-exact");
    check(half_to_float(0x7C00) > 3.0e38f, "cvt: half infinity");
    check(half_to_float(0x7C01) != half_to_float(0x7C01), "cvt: half NaN stays NaN");
  }
  {
    check(float_to_unorm8(0.0f) == 0 && float_to_unorm8(1.0f) == 255, "cvt: float endpoints");
    check(float_to_unorm8(0.5f) == 128 && float_to_unorm8(-1.0f) == 0 && float_to_unorm8(2.0f) == 255,
          "cvt: float clamps, 0.5 -> 128");
    check(float_to_unorm8(f32(0x7FC00000)) == 0, "cvt: NaN -> 0");
    // 1/3 as float32 (0x3EAAAAAB): 0.33333334 * 255 + 0.5 = 85.5000.. -> 85 or 86;
    // 0.33333334 * 255 = 85.0000017, + 0.5 = 85.5000017 -> truncates to 85.
    check(float_to_unorm8(f32(0x3EAAAAAB)) == 85, "cvt: float 1/3 -> 85");
  }

  // --- row conversion ------------------------------------------------------
  {
    // R8 unorm: channel 0 carries the value, rgb pad 0, alpha 255 (BC4 rules).
    const std::uint8_t src[3] = {0, 128, 255};
    std::uint8_t out[12] = {};
    convert_plain_row(1, 0, src, out, 3);
    check(out[0] == 0 && out[3] == 255 && out[4] == 128 && out[7] == 255 && out[8] == 255 && out[11] == 255,
          "row: 8 unorm pads to (v,0,0,255)");
  }
  {
    // R8G8 unorm: (r,g,0,255), BC5 rules.
    const std::uint8_t src[4] = {10, 20, 30, 40};
    std::uint8_t out[8] = {};
    convert_plain_row(3, 0, src, out, 2);
    check(out[0] == 10 && out[1] == 20 && out[2] == 0 && out[3] == 255 &&
          out[4] == 30 && out[5] == 40 && out[7] == 255, "row: 8_8 unorm pads to (r,g,0,255)");
  }
  {
    // R16G16 unorm.
    const std::uint8_t src[4] = {0x00, 0x80, 0xFF, 0xFF};  // 0x8000 -> 128, 0xFFFF -> 255
    std::uint8_t out[4] = {};
    convert_plain_row(5, 0, src, out, 1);
    check(out[0] == 128 && out[1] == 255 && out[2] == 0 && out[3] == 255, "row: 16_16 unorm");
  }
  {
    // R16G16B16A16 half float: 1.0, 0.5, 0.0, 1.0.
    const std::uint16_t h[4] = {0x3C00, 0x3800, 0x0000, 0x3C00};
    std::uint8_t out[4] = {};
    convert_plain_row(12, 7, reinterpret_cast<const std::uint8_t*>(h), out, 1);
    check(out[0] == 255 && out[1] == 128 && out[2] == 0 && out[3] == 255, "row: 16_16_16_16 float");
  }
  {
    // R32G32B32A32 float: 0.25, 0.5, 0.75, 1.0 -> 64, 128, 191, 255.
    const std::uint32_t f[4] = {0x3E800000, 0x3F000000, 0x3F400000, 0x3F800000};
    std::uint8_t out[4] = {};
    convert_plain_row(14, 7, reinterpret_cast<const std::uint8_t*>(f), out, 1);
    check(out[0] == 64 && out[1] == 128 && out[2] == 191 && out[3] == 255, "row: 32_32_32_32 float");
  }
  {
    // 5_6_5: pure red, pure green, white.
    const std::uint16_t px[3] = {0xF800, 0x07E0, 0xFFFF};
    std::uint8_t out[12] = {};
    convert_plain_row(16, 0, reinterpret_cast<const std::uint8_t*>(px), out, 3);
    check(out[0] == 255 && out[1] == 0 && out[2] == 0 && out[3] == 255, "row: 5_6_5 red");
    check(out[4] == 0 && out[5] == 255 && out[6] == 0, "row: 5_6_5 green");
    check(out[8] == 255 && out[9] == 255 && out[10] == 255, "row: 5_6_5 white");
  }
  {
    // 8_8_8_8 unorm and sRGB both pass bytes through (the host converts sRGB).
    const std::uint8_t src[4] = {1, 2, 3, 4};
    std::uint8_t out[4] = {}, out2[4] = {};
    convert_plain_row(10, 0, src, out, 1);
    convert_plain_row(10, 9, src, out2, 1);
    check(out[0] == 1 && out[1] == 2 && out[2] == 3 && out[3] == 4, "row: 8_8_8_8 unorm passes through");
    check(out2[0] == 1 && out2[1] == 2 && out2[2] == 3 && out2[3] == 4, "row: 8_8_8_8 srgb bytes pass through");
  }
}
