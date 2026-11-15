// Slice S22 tests: PS5-native thin texture tiling - block geometry, mip-chain
// layout with tail packing, per-element addresses and CPU detile. Known-answer
// vectors come from AnyPS5's tests/TextureTiling.cpp (its comments record them
// as validated against AMD addrlib); AnyPS5 is reused under the project owner's
// grant with attribution. Pure host-side, no Vulkan device needed.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "graphics/gfx_swizzle.hpp"

namespace {
using namespace pnr::graphics;

struct SwAddr { std::uint32_t mip, x, y; std::uint64_t addr; };

template <typename TReport>
void check_addresses(const char* what, std::uint32_t mode, std::uint32_t elem_bytes,
                     std::uint32_t w, std::uint32_t h, std::uint32_t mips, std::uint64_t guest_bytes,
                     const SwAddr* expected, std::size_t count, TReport&& report) {
  TiledMip layout[kMaxTiledMips];
  std::uint64_t bytes = 0;
  if (!thin_mip_layout(mode, elem_bytes, 1, 1, w, h, mips, layout, kMaxTiledMips, bytes)) {
    report(false, std::string(what) + ": layout refused");
    return;
  }
  report(bytes == guest_bytes, std::string(what) + ": guest size");
  bool ok = true;
  for (std::size_t i = 0; i < count && i < 64; ++i) {
    const auto& e = expected[i];
    const auto got = thin_element_offset(mode, elem_bytes, layout[e.mip], e.x, e.y, 0);
    if (got != e.addr) {
      std::printf("    %s mip %u (%u,%u): got 0x%llx want 0x%llx\n", what, e.mip, e.x, e.y,
                  (unsigned long long)got, (unsigned long long)e.addr);
      ok = false;
    }
  }
  report(ok, std::string(what) + ": element addresses");
}

}  // namespace

void pnr_gfx_swizzle_tests(int& pass, int& fail) {
  auto check = [&](bool ok, const std::string& l) { if (ok) ++pass; else ++fail; std::printf("%s %s\n", ok ? "PASS" : "FAIL", l.c_str()); };

  // Mode vocabulary: PS5-native SW_MODE values, GCN index encodings refused.
  check(swizzle_mode_known(kSwLinear) && swizzle_mode_known(kSwZ64KBX) && swizzle_mode_known(kSwR64KBX),
        "swizzle: linear and XOR 64KB modes are known");
  check(!swizzle_mode_known(8) && !swizzle_mode_known(31),
        "swizzle: GCN tiling index encodings 8/31 are not PS5-native SW_MODE values");
  check(!swizzle_mode_known(3) && !swizzle_mode_known(32), "swizzle: unknown modes refused");

  // Block extents (AnyPS5 kLog2BlockThin tables).
  {
    SwizzleBlock b{};
    check(swizzle_block(kSwStandard64KB, 4, b) && b.bytes == 65536 && b.width == 128 && b.height == 128,
          "swizzle: 64KB block is 128x128 at 4 bytes");
    check(swizzle_block(kSwStandard4KB, 4, b) && b.bytes == 4096 && b.width == 32 && b.height == 32,
          "swizzle: 4KB block is 32x32 at 4 bytes");
    check(swizzle_block(kSwD256B, 1, b) && b.bytes == 256 && b.width == 16 && b.height == 16,
          "swizzle: 256B block is 16x16 at 1 byte");
    check(!swizzle_block(kSwLinear, 4, b) && !swizzle_block(kSwStandard4KB, 3, b),
          "swizzle: linear mode and non-power-of-2 elements refused");
  }

  // Known-answer address vectors (AnyPS5 test_TextureTiling.cpp, single slice).
  static constexpr SwAddr atD256B[] = {{0, 0, 0, 0x300}, {0, 39, 19, 0x81f}, {0, 20, 6, 0x42c}, {0, 39, 0, 0x507}, {0, 0, 19, 0x618}, {1, 0, 0, 0x100}, {1, 19, 9, 0x293}, {1, 10, 3, 0x15a}, {1, 19, 0, 0x203}, {1, 0, 9, 0x190}, {2, 0, 0, 0x0}, {2, 9, 4, 0x61}, {2, 5, 1, 0x15}, {2, 9, 0, 0x41}, {2, 0, 4, 0x20}};
  check_addresses("atD256B", kSwD256B, 1, 40, 20, 3, 2304ull, atD256B, std::size(atD256B), check);
  static constexpr SwAddr atD4KB[] = {{0, 0, 0, 0x6000}, {0, 99, 59, 0x154b8}, {0, 50, 20, 0xb920}, {0, 99, 0, 0x9028}, {0, 0, 59, 0x12490}, {1, 0, 0, 0x2000}, {1, 49, 29, 0x5d18}, {1, 25, 10, 0x2e88}, {1, 49, 0, 0x3808}, {1, 0, 29, 0x4510}, {2, 0, 0, 0x1000}, {2, 24, 14, 0x1f80}, {2, 12, 5, 0x1350}, {2, 24, 0, 0x1a00}, {2, 0, 14, 0x1580}, {3, 0, 0, 0x800}, {3, 11, 6, 0xba8}, {3, 6, 2, 0x8e0}, {3, 11, 0, 0xa28}, {3, 0, 6, 0x980}, {4, 0, 0, 0x600}, {4, 5, 2, 0x6c8}, {4, 3, 1, 0x638}, {4, 5, 0, 0x648}, {4, 0, 2, 0x680}, {5, 0, 0, 0x500}, {5, 2, 0, 0x520}, {5, 1, 0, 0x508}, {5, 2, 0, 0x520}, {5, 0, 0, 0x500}, {6, 0, 0, 0x400}, {6, 0, 0, 0x400}, {6, 0, 0, 0x400}, {6, 0, 0, 0x400}, {6, 0, 0, 0x400}};
  check_addresses("atD4KB", kSwD4KB, 8, 100, 60, 7, 90112ull, atD4KB, std::size(atD4KB), check);
  static constexpr SwAddr atD64KB[] = {{0, 0, 0, 0x20000}, {0, 199, 149, 0x584dc}, {0, 100, 50, 0x2b4a0}, {0, 199, 0, 0x3808c}, {0, 0, 149, 0x40450}, {1, 0, 0, 0x10000}, {1, 99, 74, 0x1e12c}, {1, 50, 25, 0x12d18}, {1, 99, 0, 0x1a00c}, {1, 0, 74, 0x14120}, {2, 0, 0, 0x8000}, {2, 49, 36, 0xb844}, {2, 25, 12, 0x8b44}, {2, 49, 0, 0xa804}, {2, 0, 36, 0x9040}, {3, 0, 0, 0x4000}, {3, 24, 17, 0x4e10}, {3, 12, 6, 0x42e0}, {3, 24, 0, 0x4a00}, {3, 0, 17, 0x4410}, {4, 0, 0, 0x2000}, {4, 11, 8, 0x230c}, {4, 6, 3, 0x20b8}, {4, 11, 0, 0x220c}, {4, 0, 8, 0x2100}, {5, 0, 0, 0x1000}, {5, 5, 3, 0x10b4}, {5, 3, 1, 0x101c}, {5, 5, 0, 0x1084}, {5, 0, 3, 0x1030}};
  check_addresses("atD64KB", kSwD64KB, 4, 200, 150, 6, 393216ull, atD64KB, std::size(atD64KB), check);
  static constexpr SwAddr atS64KBT[] = {{0, 0, 0, 0x90000}, {0, 519, 259, 0x1700bc}, {0, 260, 86, 0xb46e0}, {0, 519, 0, 0xd008c}, {0, 0, 259, 0x130030}, {1, 0, 0, 0x30000}, {1, 259, 129, 0x8001c}, {1, 130, 43, 0x41938}, {1, 259, 0, 0x5000c}, {1, 0, 129, 0x60010}, {2, 0, 0, 0x10000}, {2, 129, 64, 0x24204}, {2, 65, 21, 0x18554}, {2, 129, 0, 0x20004}, {2, 0, 64, 0x14200}, {3, 0, 0, 0x0}, {3, 64, 31, 0x8470}, {3, 32, 10, 0x2520}, {3, 64, 0, 0x8100}, {3, 0, 31, 0x570}};
  check_addresses("atS64KBT", kSwS64KBT, 4, 520, 260, 4, 1572864ull, atS64KBT, std::size(atS64KBT), check);
  static constexpr SwAddr atD64KBT[] = {{0, 0, 0, 0x10000}, {0, 63, 39, 0x1ecf0}, {0, 32, 13, 0x18420}, {0, 63, 0, 0x1af50}, {0, 0, 39, 0x143a0}, {1, 0, 0, 0x8100}, {1, 31, 19, 0xb7f0}, {1, 16, 6, 0xa480}, {1, 31, 0, 0xaf50}, {1, 0, 19, 0x99a0}, {2, 0, 0, 0x4200}, {2, 15, 9, 0x4c70}, {2, 8, 3, 0x4aa0}, {2, 15, 0, 0x4850}, {2, 0, 9, 0x4620}, {3, 0, 0, 0x2400}, {3, 7, 4, 0x2750}, {3, 4, 1, 0x2620}, {3, 7, 0, 0x2650}, {3, 0, 4, 0x2500}, {4, 0, 0, 0x1800}, {4, 3, 1, 0x1870}, {4, 2, 0, 0x1840}, {4, 3, 0, 0x1850}, {4, 0, 1, 0x1820}};
  check_addresses("atD64KBT", kSwD64KBT, 16, 64, 40, 5, 131072ull, atD64KBT, std::size(atD64KBT), check);
  static constexpr SwAddr atS4KBX[] = {{0, 0, 0, 0x2000}, {0, 99, 69, 0x5453}, {0, 50, 23, 0x2b72}, {0, 99, 0, 0x3c03}, {0, 0, 69, 0x4850}, {1, 0, 0, 0x1000}, {1, 49, 34, 0x1e21}, {1, 25, 11, 0x12b9}, {1, 49, 0, 0x1a01}, {1, 0, 34, 0x1420}, {2, 0, 0, 0x800}, {2, 24, 16, 0xb08}, {2, 12, 5, 0x85c}, {2, 24, 0, 0xa08}, {2, 0, 16, 0x900}, {3, 0, 0, 0x600}, {3, 11, 7, 0x67b}, {3, 6, 2, 0x626}, {3, 11, 0, 0x60b}, {3, 0, 7, 0x670}, {4, 0, 0, 0x500}, {4, 5, 3, 0x535}, {4, 3, 1, 0x513}, {4, 5, 0, 0x505}, {4, 0, 3, 0x530}, {5, 0, 0, 0x400}, {5, 2, 1, 0x412}, {5, 1, 0, 0x401}, {5, 2, 0, 0x402}, {5, 0, 1, 0x410}};
  check_addresses("atS4KBX", kSwS4KBX, 1, 100, 70, 6, 24576ull, atS4KBX, std::size(atS4KBX), check);
  static constexpr SwAddr atD4KBX[] = {{0, 0, 0, 0x2000}, {0, 32, 19, 0x3030}, {0, 16, 6, 0x2860}, {0, 32, 0, 0x3400}, {0, 0, 19, 0x2430}, {1, 0, 0, 0x1000}, {1, 15, 9, 0x139c}, {1, 8, 3, 0x1230}, {1, 15, 0, 0x128c}, {1, 0, 9, 0x1110}, {2, 0, 0, 0x800}, {2, 7, 4, 0x8cc}, {2, 4, 1, 0x890}, {2, 7, 0, 0x88c}, {2, 0, 4, 0x840}, {3, 0, 0, 0x600}, {3, 3, 1, 0x61c}, {3, 2, 0, 0x608}, {3, 3, 0, 0x60c}, {3, 0, 1, 0x610}};
  check_addresses("atD4KBX", kSwD4KBX, 4, 33, 20, 4, 16384ull, atD4KBX, std::size(atD4KBX), check);
  static constexpr SwAddr atZ64KBX8[] = {{0, 0, 0, 0x20000}, {1, 0, 0, 0x10000}};
  check_addresses("atZ64KBX8", kSwZ64KBX, 1, 256, 256, 9, 196608ull, atZ64KBX8, std::size(atZ64KBX8), check);
  static constexpr SwAddr atZ64KBX16[] = {{0, 0, 0, 0x10000}};
  check_addresses("atZ64KBX16", kSwZ64KBX, 2, 128, 128, 8, 131072ull, atZ64KBX16, std::size(atZ64KBX16), check);

  // Byte-exact detile of a full mip chain: fill the tiled surface with a
  // per-address pattern, detile, and verify the linear image rows.
  {
    const std::uint32_t mode = kSwD64KB, eb = 4, w = 200, h = 150, mips = 6;
    TiledMip layout[kMaxTiledMips];
    std::uint64_t bytes = 0;
    check(thin_mip_layout(mode, eb, 1, 1, w, h, mips, layout, kMaxTiledMips, bytes) && bytes == 393216,
          "detile: D64KB 200x150x6 layout");
    std::vector<std::uint8_t> tiled(bytes);
    for (std::uint32_t level = 0; level < mips; ++level) {
      const auto& mip = layout[level];
      const std::uint32_t ew = std::max(w >> level, 1u), eh = std::max(h >> level, 1u);
      for (std::uint32_t y = 0; y < eh; ++y)
        for (std::uint32_t x = 0; x < ew; ++x) {
          const auto off = thin_element_offset(mode, eb, mip, x, y, 0);
          const std::uint32_t v = (level << 24) ^ (y << 12) ^ x;
          std::memcpy(&tiled[off], &v, 4);
        }
    }
    std::uint64_t linear_total = 0;
    for (const auto& mip : layout) linear_total = std::max(linear_total, mip.linear_offset + mip.linear_size);
    std::vector<std::uint8_t> linear(linear_total, 0);
    for (std::uint32_t level = 0; level < mips; ++level)
      detile_thin_mip(mode, eb, layout[level], std::max(w >> level, 1u), std::max(h >> level, 1u),
                      tiled.data(), linear.data(), 0);
    bool ok = true;
    for (std::uint32_t level = 0; level < mips && ok; ++level) {
      const auto& mip = layout[level];
      const std::uint32_t ew = std::max(w >> level, 1u), eh = std::max(h >> level, 1u);
      for (std::uint32_t y = 0; y < eh && ok; ++y)
        for (std::uint32_t x = 0; x < ew && ok; ++x) {
          std::uint32_t v = 0;
          std::memcpy(&v, &linear[mip.linear_offset + std::uint64_t(y) * mip.pitch_bytes + std::uint64_t(x) * 4], 4);
          ok = v == ((level << 24) ^ (y << 12) ^ x);
        }
    }
    check(ok, "detile: D64KB chain detiles every element to its linear slot");
  }

  // pipe/bank XOR rotation of the surface base (XOR modes only).
  {
    TiledMip layout[kMaxTiledMips];
    std::uint64_t bytes = 0;
    check(thin_mip_layout(kSwZ64KBX, 4, 1, 1, 128, 128, 1, layout, kMaxTiledMips, bytes),
          "xor: layout for pipe/bank rotation");
    const auto plain = thin_element_offset(kSwZ64KBX, 4, layout[0], 0, 0, 0);
    const auto rotated = thin_element_offset(kSwZ64KBX, 4, layout[0], 0, 0, 0x4000);
    check(plain == 0 && rotated == 0x4000, "xor: pipe/bank bits rotate the element address");
    const auto plain_d = thin_element_offset(kSwD64KB, 4, layout[0], 0, 0, 0x4000);
    check(plain_d == 0, "xor: non-XOR display mode ignores pipe/bank bits");
  }
}
