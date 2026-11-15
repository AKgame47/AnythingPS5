// PNR - PlayStation Native Runtime
// graphics/gfx_swizzle.hpp - PS5 thin texture tiling: swizzle address equations,
// tile-block geometry, mip-chain layout with tail packing, and CPU detiling.
//
// Ported from AnyPS5 (https://github.com/boykopovar/AnyPS5), GPL - reused under
// the project owner's grant with attribution:
//   core/libs/prx/libSceAgcDriver/Graphics/include/TextureSwizzleEquations.hpp
//   core/libs/prx/libSceAgcDriver/Graphics/src/TextureTiling.cpp (thin path)
//   core/libs/prx/libSceAgcDriver/Graphics/shaders/TextureDetile.comp
//   core/libs/prx/libSceAgcDriver/Graphics/src/GuestTextureResource.cpp (mode map)
// The equation bitmasks are AMD addrlib GFX10 swizzle pattern data (factual
// hardware values), as documented in the AnyPS5 source. Ground truth for the
// tests is AnyPS5's own test vectors (tests/TextureTiling.cpp), whose expected
// addresses were validated against AMD addrlib by the AnyPS5 project.
//
// Scope of this port: thin (2D, single-slice) PS5-native swizzle modes only.
// Thick/3D, multi-slice arrays, PRT, DCC and GCN (PS4) modes are refused.

#ifndef PNR_GRAPHICS_GFX_SWIZZLE_HPP
#define PNR_GRAPHICS_GFX_SWIZZLE_HPP

#include <array>
#include <bit>
#include <cstdint>

namespace pnr::graphics {

// PS5-native T# tiling field (descriptor word 3, bits 20-24) = hardware SW_MODE.
// Values from AnyPS5's resolveTileMode/XorSwizzleMode/EquationSwizzleMode.
inline constexpr std::uint32_t kSwLinear = 0;
inline constexpr std::uint32_t kSwStandard256B = 1;
inline constexpr std::uint32_t kSwD256B = 2;
inline constexpr std::uint32_t kSwStandard4KB = 5;
inline constexpr std::uint32_t kSwD4KB = 6;
inline constexpr std::uint32_t kSwStandard64KB = 9;
inline constexpr std::uint32_t kSwD64KB = 10;
inline constexpr std::uint32_t kSwS64KBT = 17;
inline constexpr std::uint32_t kSwD64KBT = 18;
inline constexpr std::uint32_t kSwS4KBX = 21;
inline constexpr std::uint32_t kSwD4KBX = 22;
inline constexpr std::uint32_t kSwZ64KBX = 24;
inline constexpr std::uint32_t kSwS64KBX = 25;
inline constexpr std::uint32_t kSwD64KBX = 26;
inline constexpr std::uint32_t kSwR64KBX = 27;

inline bool swizzle_mode_known(std::uint32_t mode) {
  switch (mode) {
    case kSwLinear: case kSwStandard256B: case kSwD256B: case kSwStandard4KB:
    case kSwD4KB: case kSwStandard64KB: case kSwD64KB: case kSwS64KBT:
    case kSwD64KBT: case kSwS4KBX: case kSwD4KBX: case kSwZ64KBX:
    case kSwS64KBX: case kSwD64KBX: case kSwR64KBX: return true;
    default: return false;
  }
}

// Modes whose low base-address bits XOR into block offsets (pipe/bank rotation);
// the base address is normalized down and the dropped bits become pipe_bank_xor.
inline bool swizzle_mode_xor(std::uint32_t mode) {
  switch (mode) {
    case kSwS64KBT: case kSwD64KBT: case kSwS4KBX: case kSwD4KBX:
    case kSwZ64KBX: case kSwS64KBX: case kSwD64KBX: case kSwR64KBX: return true;
    default: return false;
  }
}

// Modes addressed with an address equation (TILE_FAMILY 2 in AnyPS5's detiler);
// the remaining tiled modes use the standard micro-tile offset formulas.
inline bool swizzle_mode_equation(std::uint32_t mode) {
  switch (mode) {
    case kSwD256B: case kSwD4KB: case kSwD64KB: return true;
    default: return swizzle_mode_xor(mode);
  }
}

inline std::uint32_t swizzle_block_bytes(std::uint32_t mode) {
  switch (mode) {
    case kSwStandard256B: case kSwD256B: return 256;
    case kSwStandard4KB: case kSwD4KB: case kSwS4KBX: case kSwD4KBX: return 4096;
    case kSwStandard64KB: case kSwD64KB: case kSwS64KBT: case kSwD64KBT:
    case kSwZ64KBX: case kSwS64KBX: case kSwD64KBX: case kSwR64KBX: return 65536;
    default: return 0;
  }
}

// Address equations of the 64 KiB XOR swizzle modes for the PS5 GPU, a
// Navi1x-class part with 16 pipes, plus the 256B/4KB/64KB display (D) modes and
// the 4KB S/D XOR variants. Each entry gives, for byte-address bit 0..15 inside
// a block, the element-coordinate bits XORed into it: bits 0-11 select x bits,
// bits 12-23 select y bits and bits 24-31 select slice (or depth) bits.
struct SwizzleEquation {
  std::uint32_t mode;
  std::uint32_t element_bytes;
  std::array<std::uint32_t, 16> bits;
};

inline constexpr SwizzleEquation kSwizzleEquations[] = {
    {24, 1, {0x00000001u, 0x00001000u, 0x00000002u, 0x00002000u, 0x00000004u, 0x00004000u, 0x00000008u, 0x00010000u, 0x08008008u, 0x04010010u, 0x02020040u, 0x01040020u, 0x00040000u, 0x00000040u, 0x00080000u, 0x00000080u}},
    {24, 2, {0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00002000u, 0x00000004u, 0x00004000u, 0x00000008u, 0x08008008u, 0x04010010u, 0x02020040u, 0x01040020u, 0x00010000u, 0x00000040u, 0x00040000u, 0x00000080u}},
    {24, 4, {0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00002000u, 0x00000004u, 0x00004000u, 0x08008008u, 0x04010010u, 0x02020040u, 0x01040020u, 0x00008000u, 0x00000010u, 0x00040000u, 0x00000040u}},
    {24, 8, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00002000u, 0x00000004u, 0x08008008u, 0x04010010u, 0x02020040u, 0x01040020u, 0x00004000u, 0x00000008u, 0x00010000u, 0x00000040u}},
    {24, 16, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00002000u, 0x08008008u, 0x04010010u, 0x02020040u, 0x01040020u, 0x00004000u, 0x00000004u, 0x00008000u, 0x00000010u}},
    {25, 1, {0x00000001u, 0x00000002u, 0x00000004u, 0x00000008u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00008000u, 0x08010080u, 0x04080010u, 0x02020040u, 0x01040020u, 0x00040000u, 0x00000040u, 0x00080000u, 0x00000080u}},
    {25, 2, {0x00000000u, 0x00000001u, 0x00000002u, 0x00000004u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000008u, 0x08008080u, 0x04040010u, 0x02010040u, 0x01020020u, 0x00020000u, 0x00000040u, 0x00040000u, 0x00000080u}},
    {25, 4, {0x00000000u, 0x00000000u, 0x00000001u, 0x00000002u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000004u, 0x08008040u, 0x04040008u, 0x02010020u, 0x01020010u, 0x00020000u, 0x00000020u, 0x00040000u, 0x00000040u}},
    {25, 8, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00002000u, 0x00000002u, 0x00000004u, 0x08004040u, 0x04020008u, 0x02008020u, 0x01010010u, 0x00010000u, 0x00000020u, 0x00020000u, 0x00000040u}},
    {25, 16, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00001000u, 0x00002000u, 0x00000001u, 0x00000002u, 0x08004020u, 0x04020004u, 0x02008010u, 0x01010008u, 0x00010000u, 0x00000010u, 0x00020000u, 0x00000020u}},
    {26, 1, {0x00000001u, 0x00000002u, 0x00000004u, 0x00002000u, 0x00001000u, 0x00004000u, 0x00000008u, 0x00008000u, 0x08010080u, 0x04080010u, 0x02020040u, 0x01040020u, 0x00040000u, 0x00000040u, 0x00080000u, 0x00000080u}},
    {26, 2, {0x00000000u, 0x00000001u, 0x00000002u, 0x00000004u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000008u, 0x08008080u, 0x04040010u, 0x02010040u, 0x01020020u, 0x00020000u, 0x00000040u, 0x00040000u, 0x00000080u}},
    {26, 4, {0x00000000u, 0x00000000u, 0x00000001u, 0x00000002u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000004u, 0x08008040u, 0x04040008u, 0x02010020u, 0x01020010u, 0x00020000u, 0x00000020u, 0x00040000u, 0x00000040u}},
    {26, 8, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00000004u, 0x00002000u, 0x08004040u, 0x04020008u, 0x02008020u, 0x01010010u, 0x00010000u, 0x00000020u, 0x00020000u, 0x00000040u}},
    {26, 16, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00002000u, 0x08004020u, 0x04020004u, 0x02008010u, 0x01010008u, 0x00010000u, 0x00000010u, 0x00020000u, 0x00000020u}},
    {27, 1, {0x00000001u, 0x00000002u, 0x00000004u, 0x00002000u, 0x00001000u, 0x00004000u, 0x00000008u, 0x00010000u, 0x08008008u, 0x04010010u, 0x02020040u, 0x01040020u, 0x00040000u, 0x00000040u, 0x00080000u, 0x00000080u}},
    {27, 2, {0x00000000u, 0x00000001u, 0x00000002u, 0x00000004u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000008u, 0x08008008u, 0x04010010u, 0x02020040u, 0x01040020u, 0x00010000u, 0x00000040u, 0x00040000u, 0x00000080u}},
    {27, 4, {0x00000000u, 0x00000000u, 0x00000001u, 0x00000002u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000004u, 0x08008008u, 0x04010010u, 0x02020040u, 0x01040020u, 0x00008000u, 0x00000010u, 0x00040000u, 0x00000040u}},
    {27, 8, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00000004u, 0x00002000u, 0x08008008u, 0x04010010u, 0x02020040u, 0x01040020u, 0x00004000u, 0x00000008u, 0x00010000u, 0x00000040u}},
    {27, 16, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00002000u, 0x08008008u, 0x04010010u, 0x02020040u, 0x01040020u, 0x00004000u, 0x00000004u, 0x00008000u, 0x00000010u}},
    {2, 1, {0x00000001u, 0x00000002u, 0x00000004u, 0x00002000u, 0x00001000u, 0x00004000u, 0x00000008u, 0x00008000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
    {2, 2, {0x00000000u, 0x00000001u, 0x00000002u, 0x00000004u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000008u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
    {2, 4, {0x00000000u, 0x00000000u, 0x00000001u, 0x00000002u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000004u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
    {2, 8, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00000004u, 0x00002000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
    {2, 16, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00002000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
    {6, 1, {0x00000001u, 0x00000002u, 0x00000004u, 0x00002000u, 0x00001000u, 0x00004000u, 0x00000008u, 0x00008000u, 0x00010000u, 0x00000010u, 0x00020000u, 0x00000020u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
    {6, 2, {0x00000000u, 0x00000001u, 0x00000002u, 0x00000004u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000008u, 0x00008000u, 0x00000010u, 0x00010000u, 0x00000020u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
    {6, 4, {0x00000000u, 0x00000000u, 0x00000001u, 0x00000002u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000004u, 0x00008000u, 0x00000008u, 0x00010000u, 0x00000010u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
    {6, 8, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00000004u, 0x00002000u, 0x00004000u, 0x00000008u, 0x00008000u, 0x00000010u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
    {6, 16, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00002000u, 0x00004000u, 0x00000004u, 0x00008000u, 0x00000008u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
    {10, 1, {0x00000001u, 0x00000002u, 0x00000004u, 0x00002000u, 0x00001000u, 0x00004000u, 0x00000008u, 0x00008000u, 0x00010000u, 0x00000010u, 0x00020000u, 0x00000020u, 0x00040000u, 0x00000040u, 0x00080000u, 0x00000080u}},
    {10, 2, {0x00000000u, 0x00000001u, 0x00000002u, 0x00000004u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000008u, 0x00008000u, 0x00000010u, 0x00010000u, 0x00000020u, 0x00020000u, 0x00000040u, 0x00040000u, 0x00000080u}},
    {10, 4, {0x00000000u, 0x00000000u, 0x00000001u, 0x00000002u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000004u, 0x00008000u, 0x00000008u, 0x00010000u, 0x00000010u, 0x00020000u, 0x00000020u, 0x00040000u, 0x00000040u}},
    {10, 8, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00000004u, 0x00002000u, 0x00004000u, 0x00000008u, 0x00008000u, 0x00000010u, 0x00010000u, 0x00000020u, 0x00020000u, 0x00000040u}},
    {10, 16, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00002000u, 0x00004000u, 0x00000004u, 0x00008000u, 0x00000008u, 0x00010000u, 0x00000010u, 0x00020000u, 0x00000020u}},
    {17, 1, {0x00000001u, 0x00000002u, 0x00000004u, 0x00000008u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00008000u, 0x00010080u, 0x00080010u, 0x00020040u, 0x00040020u, 0x00040000u, 0x00000040u, 0x00080000u, 0x00000080u}},
    {17, 2, {0x00000000u, 0x00000001u, 0x00000002u, 0x00000004u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000008u, 0x00008080u, 0x00040010u, 0x00010040u, 0x00020020u, 0x00020000u, 0x00000040u, 0x00040000u, 0x00000080u}},
    {17, 4, {0x00000000u, 0x00000000u, 0x00000001u, 0x00000002u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000004u, 0x00008040u, 0x00040008u, 0x00010020u, 0x00020010u, 0x00020000u, 0x00000020u, 0x00040000u, 0x00000040u}},
    {17, 8, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00002000u, 0x00000002u, 0x00000004u, 0x00004040u, 0x00020008u, 0x00008020u, 0x00010010u, 0x00010000u, 0x00000020u, 0x00020000u, 0x00000040u}},
    {17, 16, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00001000u, 0x00002000u, 0x00000001u, 0x00000002u, 0x00004020u, 0x00020004u, 0x00008010u, 0x00010008u, 0x00010000u, 0x00000010u, 0x00020000u, 0x00000020u}},
    {18, 1, {0x00000001u, 0x00000002u, 0x00000004u, 0x00002000u, 0x00001000u, 0x00004000u, 0x00000008u, 0x00008000u, 0x00010080u, 0x00080010u, 0x00020040u, 0x00040020u, 0x00040000u, 0x00000040u, 0x00080000u, 0x00000080u}},
    {18, 2, {0x00000000u, 0x00000001u, 0x00000002u, 0x00000004u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000008u, 0x00008080u, 0x00040010u, 0x00010040u, 0x00020020u, 0x00020000u, 0x00000040u, 0x00040000u, 0x00000080u}},
    {18, 4, {0x00000000u, 0x00000000u, 0x00000001u, 0x00000002u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000004u, 0x00008040u, 0x00040008u, 0x00010020u, 0x00020010u, 0x00020000u, 0x00000020u, 0x00040000u, 0x00000040u}},
    {18, 8, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00000004u, 0x00002000u, 0x00004040u, 0x00020008u, 0x00008020u, 0x00010010u, 0x00010000u, 0x00000020u, 0x00020000u, 0x00000040u}},
    {18, 16, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00002000u, 0x00004020u, 0x00020004u, 0x00008010u, 0x00010008u, 0x00010000u, 0x00000010u, 0x00020000u, 0x00000020u}},
    {21, 1, {0x00000001u, 0x00000002u, 0x00000004u, 0x00000008u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00008000u, 0x08010080u, 0x04080010u, 0x02020040u, 0x01040020u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
    {21, 2, {0x00000000u, 0x00000001u, 0x00000002u, 0x00000004u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000008u, 0x08008080u, 0x04040010u, 0x02010040u, 0x01020020u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
    {21, 4, {0x00000000u, 0x00000000u, 0x00000001u, 0x00000002u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000004u, 0x08008040u, 0x04040008u, 0x02010020u, 0x01020010u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
    {21, 8, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00002000u, 0x00000002u, 0x00000004u, 0x08004040u, 0x04020008u, 0x02008020u, 0x01010010u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
    {21, 16, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00001000u, 0x00002000u, 0x00000001u, 0x00000002u, 0x08004020u, 0x04020004u, 0x02008010u, 0x01010008u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
    {22, 1, {0x00000001u, 0x00000002u, 0x00000004u, 0x00002000u, 0x00001000u, 0x00004000u, 0x00000008u, 0x00008000u, 0x08010080u, 0x04080010u, 0x02020040u, 0x01040020u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
    {22, 2, {0x00000000u, 0x00000001u, 0x00000002u, 0x00000004u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000008u, 0x08008080u, 0x04040010u, 0x02010040u, 0x01020020u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
    {22, 4, {0x00000000u, 0x00000000u, 0x00000001u, 0x00000002u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000004u, 0x08008040u, 0x04040008u, 0x02010020u, 0x01020010u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
    {22, 8, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00000004u, 0x00002000u, 0x08004040u, 0x04020008u, 0x02008020u, 0x01010010u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
    {22, 16, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00002000u, 0x08004020u, 0x04020004u, 0x02008010u, 0x01010008u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
    {0x105, 1, {0x00000001u, 0x00000002u, 0x01000000u, 0x00001000u, 0x02000000u, 0x00002000u, 0x00000004u, 0x04000000u, 0x00004000u, 0x00000008u, 0x08000000u, 0x00008000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
    {0x105, 2, {0x00000000u, 0x00000001u, 0x01000000u, 0x00001000u, 0x02000000u, 0x00002000u, 0x00000002u, 0x04000000u, 0x00004000u, 0x00000004u, 0x08000000u, 0x00008000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
    {0x105, 4, {0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x01000000u, 0x00002000u, 0x00000002u, 0x02000000u, 0x00004000u, 0x00000004u, 0x04000000u, 0x00008000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
    {0x105, 8, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x01000000u, 0x00001000u, 0x00000002u, 0x02000000u, 0x00002000u, 0x00000004u, 0x04000000u, 0x00004000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
    {0x105, 16, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x01000000u, 0x00001000u, 0x00000001u, 0x02000000u, 0x00002000u, 0x00000002u, 0x04000000u, 0x00004000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
    {0x109, 1, {0x00000001u, 0x00000002u, 0x01000000u, 0x00001000u, 0x02000000u, 0x00002000u, 0x00000004u, 0x04000000u, 0x00004000u, 0x00000008u, 0x08000000u, 0x00008000u, 0x00000010u, 0x10000000u, 0x00010000u, 0x00000020u}},
    {0x109, 2, {0x00000000u, 0x00000001u, 0x01000000u, 0x00001000u, 0x02000000u, 0x00002000u, 0x00000002u, 0x04000000u, 0x00004000u, 0x00000004u, 0x08000000u, 0x00008000u, 0x00000008u, 0x10000000u, 0x00010000u, 0x00000010u}},
    {0x109, 4, {0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x01000000u, 0x00002000u, 0x00000002u, 0x02000000u, 0x00004000u, 0x00000004u, 0x04000000u, 0x00008000u, 0x00000008u, 0x08000000u, 0x00010000u, 0x00000010u}},
    {0x109, 8, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x01000000u, 0x00001000u, 0x00000002u, 0x02000000u, 0x00002000u, 0x00000004u, 0x04000000u, 0x00004000u, 0x00000008u, 0x08000000u, 0x00008000u, 0x00000010u}},
    {0x109, 16, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x01000000u, 0x00001000u, 0x00000001u, 0x02000000u, 0x00002000u, 0x00000002u, 0x04000000u, 0x00004000u, 0x00000004u, 0x08000000u, 0x00008000u, 0x00000008u}},
    {0x115, 1, {0x00000001u, 0x00000002u, 0x01000000u, 0x00001000u, 0x02000000u, 0x00002000u, 0x00000004u, 0x04000000u, 0x40004040u, 0x20020008u, 0x08010020u, 0x10008010u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
    {0x115, 2, {0x00000000u, 0x00000001u, 0x01000000u, 0x00001000u, 0x02000000u, 0x00002000u, 0x00000002u, 0x04000000u, 0x40004020u, 0x20020004u, 0x08010010u, 0x10008008u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
    {0x115, 4, {0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x01000000u, 0x00002000u, 0x00000002u, 0x02000000u, 0x20004020u, 0x10020004u, 0x04010010u, 0x08008008u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
    {0x115, 8, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x01000000u, 0x00001000u, 0x00000002u, 0x02000000u, 0x20002020u, 0x10010004u, 0x04008010u, 0x08004008u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
    {0x115, 16, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x01000000u, 0x00001000u, 0x00000001u, 0x02000000u, 0x20002010u, 0x10010002u, 0x04008008u, 0x08004004u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
    {0x119, 1, {0x00000001u, 0x00000002u, 0x01000000u, 0x00001000u, 0x02000000u, 0x00002000u, 0x00000004u, 0x04000000u, 0x40004040u, 0x20020008u, 0x08010020u, 0x10008010u, 0x00000010u, 0x10000000u, 0x00010000u, 0x00000020u}},
    {0x119, 2, {0x00000000u, 0x00000001u, 0x01000000u, 0x00001000u, 0x02000000u, 0x00002000u, 0x00000002u, 0x04000000u, 0x40004020u, 0x20020004u, 0x08010010u, 0x10008008u, 0x00000008u, 0x10000000u, 0x00010000u, 0x00000010u}},
    {0x119, 4, {0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x01000000u, 0x00002000u, 0x00000002u, 0x02000000u, 0x20004020u, 0x10020004u, 0x04010010u, 0x08008008u, 0x00000008u, 0x08000000u, 0x00010000u, 0x00000010u}},
    {0x119, 8, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x01000000u, 0x00001000u, 0x00000002u, 0x02000000u, 0x20002020u, 0x10010004u, 0x04008010u, 0x08004008u, 0x00000008u, 0x08000000u, 0x00008000u, 0x00000010u}},
    {0x119, 16, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x01000000u, 0x00001000u, 0x00000001u, 0x02000000u, 0x20002010u, 0x10010002u, 0x04008008u, 0x08004004u, 0x00000004u, 0x08000000u, 0x00008000u, 0x00000008u}},
    {0x11a, 1, {0x00000001u, 0x00000002u, 0x01000000u, 0x00001000u, 0x00002000u, 0x02000000u, 0x00000004u, 0x04000000u, 0x00008008u, 0x00010010u, 0x00024040u, 0x00040020u, 0x08000000u, 0x00008000u, 0x00000010u, 0x10000000u}},
    {0x11a, 2, {0x00000000u, 0x00000001u, 0x01000000u, 0x00001000u, 0x00000002u, 0x02000000u, 0x00002000u, 0x04000000u, 0x00008008u, 0x00010010u, 0x08020040u, 0x00044020u, 0x00000004u, 0x10000000u, 0x00008000u, 0x00000010u}},
    {0x11a, 4, {0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x01000000u, 0x00002000u, 0x02000000u, 0x00008008u, 0x00010010u, 0x04020040u, 0x00044020u, 0x00000004u, 0x08000000u, 0x00008000u, 0x00000010u}},
    {0x11a, 8, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x01000000u, 0x00000002u, 0x02000000u, 0x00008008u, 0x00010010u, 0x04020040u, 0x00042020u, 0x00000004u, 0x08000000u, 0x00004000u, 0x00000008u}},
    {0x11a, 16, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x01000000u, 0x00001000u, 0x02000000u, 0x00008008u, 0x00010012u, 0x04020040u, 0x00042020u, 0x00000004u, 0x08000000u, 0x00004000u, 0x00000008u}},
};

inline const SwizzleEquation* find_swizzle_equation(std::uint32_t mode, std::uint32_t element_bytes) {
  for (const auto& eq : kSwizzleEquations) {
    if (eq.mode == mode && eq.element_bytes == element_bytes) return &eq;
  }
  return nullptr;
}

inline std::uint32_t equation_bit(std::uint32_t mask, std::uint32_t x, std::uint32_t y, std::uint32_t slice) {
  const std::uint32_t selected = (x & (mask & 0xfffu)) ^ ((y << 12) & (mask & 0xfff000u)) ^ ((slice << 24) & (mask & 0xff000000u));
  return std::popcount(selected) & 1u;
}

inline std::uint32_t equation_offset(const SwizzleEquation& eq, std::uint32_t x, std::uint32_t y, std::uint32_t slice) {
  std::uint32_t offset = 0;
  for (std::uint32_t bit = 0; bit < 16; ++bit) offset |= equation_bit(eq.bits[bit], x, y, slice) << bit;
  return offset;
}

// Standard (non-equation) micro-tile offset within a block; port of the
// TILE_FAMILY 1 path of AnyPS5's TextureDetile.comp.
inline std::uint32_t standard_offset(std::uint32_t element_bytes, std::uint32_t x, std::uint32_t y) {
  switch (element_bytes) {
    case 1:
      return ((y << 4) & 0x1f0u) ^ ((y << 5) & 0x400u) ^ (x & 0x00fu) ^
             ((x << 5) & 0x200u) ^ ((x << 6) & 0x800u);
    case 2:
      return ((y << 4) & 0x070u) ^ ((y << 5) & 0x100u) ^ ((y << 6) & 0x400u) ^
             ((x << 1) & 0x00eu) ^ ((x << 4) & 0x080u) ^
             ((x << 5) & 0x200u) ^ ((x << 6) & 0x800u);
    case 4:
      return ((y << 4) & 0x070u) ^ ((y << 5) & 0x100u) ^ ((y << 6) & 0x400u) ^
             ((x << 2) & 0x00cu) ^ ((x << 5) & 0x080u) ^
             ((x << 6) & 0x200u) ^ ((x << 7) & 0x800u);
    case 8:
      return ((y << 4) & 0x030u) ^ ((y << 6) & 0x100u) ^ ((y << 7) & 0x400u) ^
             ((x << 3) & 0x008u) ^ ((x << 5) & 0x0c0u) ^
             ((x << 6) & 0x200u) ^ ((x << 7) & 0x800u);
    default:
      return ((y << 4) & 0x030u) ^ ((y << 6) & 0x100u) ^ ((y << 7) & 0x400u) ^
             ((x << 6) & 0x0c0u) ^ ((x << 7) & 0x200u) ^ ((x << 8) & 0x800u);
  }
}

inline std::uint32_t standard_64kb_extra(std::uint32_t element_bytes, std::uint32_t x, std::uint32_t y) {
  switch (element_bytes) {
    case 1:
      return ((x << 7) & 0x2000u) ^ ((x << 8) & 0x8000u) ^
             ((y << 6) & 0x1000u) ^ ((y << 7) & 0x4000u);
    case 2:
      return ((x << 7) & 0x2000u) ^ ((x << 8) & 0x8000u) ^
             ((y << 7) & 0x1000u) ^ ((y << 8) & 0x4000u);
    case 4:
      return ((x << 8) & 0x2000u) ^ ((x << 9) & 0x8000u) ^
             ((y << 7) & 0x1000u) ^ ((y << 8) & 0x4000u);
    case 8:
      return ((x << 8) & 0x2000u) ^ ((x << 9) & 0x8000u) ^
             ((y << 8) & 0x1000u) ^ ((y << 9) & 0x4000u);
    default:
      return ((x << 9) & 0x2000u) ^ ((x << 10) & 0x8000u) ^
             ((y << 8) & 0x1000u) ^ ((y << 9) & 0x4000u);
  }
}

struct SwizzleBlock {
  std::uint32_t bytes;         // 256, 4096 or 65536
  std::uint32_t width;         // block extent in elements
  std::uint32_t height;
};

// Block extents for the thin modes, indexed by log2(element_bytes)
// (AnyPS5's kLog2BlockThin tables).
inline bool swizzle_block(std::uint32_t mode, std::uint32_t element_bytes, SwizzleBlock& out) {
  const std::uint32_t bytes = swizzle_block_bytes(mode);
  if (bytes == 0 || element_bytes == 0 || (element_bytes & (element_bytes - 1)) != 0 || element_bytes > 16) return false;
  const auto index = static_cast<std::size_t>(std::countr_zero(element_bytes));
  struct Log2 { std::uint8_t w, h; };
  static constexpr Log2 k256B[] = {{4, 4}, {4, 3}, {3, 3}, {3, 2}, {2, 2}};
  static constexpr Log2 k4KB[] = {{6, 6}, {6, 5}, {5, 5}, {5, 4}, {4, 4}};
  static constexpr Log2 k64KB[] = {{8, 8}, {8, 7}, {7, 7}, {7, 6}, {6, 6}};
  const Log2& d = bytes == 256 ? k256B[index] : bytes == 4096 ? k4KB[index] : k64KB[index];
  out.bytes = bytes;
  out.width = 1u << d.w;
  out.height = 1u << d.h;
  return true;
}

struct TiledMip {
  std::uint64_t tiled_offset = 0;
  std::uint64_t tiled_size = 0;
  std::uint64_t linear_offset = 0;
  std::uint64_t linear_size = 0;
  std::uint32_t width = 0;   // texels
  std::uint32_t height = 0;  // texels
  std::uint32_t blocks_per_row = 0;
  std::uint32_t pitch_bytes = 0;
  bool tail = false;
  std::uint32_t tail_x = 0;
  std::uint32_t tail_y = 0;
};

inline constexpr std::uint32_t kMaxTiledMips = 16;

// One mip level of a thin tiled surface: block layout, tail packing and the
// surface offset of every level. texel_w/h are 4 for BC formats, 1 otherwise.
// Returns false (nothing written) for linear/unknown modes or bad arguments.
inline bool thin_mip_layout(std::uint32_t mode, std::uint32_t element_bytes,
                            std::uint32_t texel_w, std::uint32_t texel_h,
                            std::uint32_t width, std::uint32_t height, std::uint32_t mip_count,
                            TiledMip* mips, std::uint32_t max_mips, std::uint64_t& guest_bytes) {
  if (mode == kSwLinear || !swizzle_mode_known(mode) || width == 0 || height == 0 ||
      mip_count == 0 || mip_count > max_mips || mip_count > kMaxTiledMips) return false;
  SwizzleBlock block{};
  if (!swizzle_block(mode, element_bytes, block)) return false;

  // Tail-packing tables (AnyPS5 kMipTailThin*): sub-block element locations of
  // the tail levels, ordered from the largest tail level down.
  struct TailXY { std::uint32_t x, y; };
  static constexpr TailXY kTail4KB[5][8] = {
    {{32, 0}, {16, 32}, {0, 48}, {0, 32}, {16, 16}, {16, 0}, {0, 16}, {0, 0}},
    {{32, 0}, {16, 16}, {0, 24}, {0, 16}, {16, 8}, {16, 0}, {0, 8}, {0, 0}},
    {{16, 0}, {8, 16}, {0, 24}, {0, 16}, {8, 8}, {8, 0}, {0, 8}, {0, 0}},
    {{16, 0}, {8, 8}, {0, 12}, {0, 8}, {8, 4}, {8, 0}, {0, 4}, {0, 0}},
    {{8, 0}, {4, 8}, {0, 12}, {0, 8}, {4, 4}, {4, 0}, {0, 4}, {0, 0}},
  };
  static constexpr TailXY kTail64KB[5][12] = {
    {{128, 0}, {0, 128}, {64, 0}, {0, 64}, {32, 0}, {16, 32}, {0, 48}, {0, 32}, {16, 16}, {16, 0}, {0, 16}, {0, 0}},
    {{128, 0}, {0, 64}, {64, 0}, {0, 32}, {32, 0}, {16, 16}, {0, 24}, {0, 16}, {16, 8}, {16, 0}, {0, 8}, {0, 0}},
    {{64, 0}, {0, 64}, {32, 0}, {0, 32}, {16, 0}, {8, 16}, {0, 24}, {0, 16}, {8, 8}, {8, 0}, {0, 8}, {0, 0}},
    {{64, 0}, {0, 32}, {32, 0}, {0, 16}, {16, 0}, {8, 8}, {0, 12}, {0, 8}, {8, 4}, {8, 0}, {0, 4}, {0, 0}},
    {{32, 0}, {0, 32}, {16, 0}, {0, 16}, {8, 0}, {4, 8}, {0, 12}, {0, 8}, {4, 4}, {4, 0}, {0, 4}, {0, 0}},
  };
  const auto index = static_cast<std::size_t>(std::countr_zero(element_bytes));
  const bool tail_possible = block.bytes != 256;
  const TailXY* locations = block.bytes == 4096 ? kTail4KB[index] : kTail64KB[index];
  const std::uint32_t max_tail_levels = block.bytes == 4096 ? 8 : 12;
  std::uint32_t width_limit = block.width >> 1;
  std::uint32_t height_limit = block.height;
  if (mode == kSwZ64KBX && element_bytes <= 2) {
    width_limit >>= 1;
    if (element_bytes == 1) height_limit >>= 1;
  }

  const auto align_up = [](std::uint32_t v, std::uint32_t a) { return (v + a - 1) / a * a; };
  const auto shift_ceil = [](std::uint32_t v, std::uint32_t s) {
    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(v) + (1ull << s) - 1) >> s);
  };
  const auto texel_dim = [](std::uint32_t guest, std::uint32_t level, std::uint32_t scale) {
    return std::max((std::max(guest >> level, 1u) + scale - 1) / scale, 1u);
  };
  const std::uint32_t elem_w0 = (width + texel_w - 1) / texel_w;
  const std::uint32_t elem_h0 = (height + texel_h - 1) / texel_h;

  std::uint32_t first_tail = mip_count;
  if (tail_possible && mip_count > 1) {
    for (std::uint32_t level = 0; level < mip_count; ++level) {
      if (shift_ceil(elem_w0, level) <= width_limit && shift_ceil(elem_h0, level) <= height_limit &&
          mip_count - level <= max_tail_levels) {
        first_tail = level;
        break;
      }
    }
  }

  std::uint64_t block_slice = 0;
  for (std::uint32_t level = 0; level < first_tail; ++level) {
    auto& mip = mips[level];
    mip.width = texel_dim(width, level, texel_w);
    mip.height = texel_dim(height, level, texel_h);
    const std::uint32_t padded_w = align_up(std::max(shift_ceil(elem_w0, level), 1u), block.width);
    const std::uint32_t padded_h = align_up(std::max(shift_ceil(elem_h0, level), 1u), block.height);
    mip.blocks_per_row = padded_w / block.width;
    mip.pitch_bytes = padded_w * element_bytes;
    mip.tiled_size = static_cast<std::uint64_t>(padded_w) * padded_h * element_bytes;
    mip.linear_size = static_cast<std::uint64_t>(mip.pitch_bytes) * mip.height;
    block_slice += mip.tiled_size;
  }
  if (first_tail < mip_count) block_slice += block.bytes;
  for (std::uint32_t level = first_tail; level < mip_count; ++level) {
    auto& mip = mips[level];
    mip.width = texel_dim(width, level, texel_w);
    mip.height = texel_dim(height, level, texel_h);
    mip.blocks_per_row = 1;
    mip.pitch_bytes = block.width * element_bytes;
    mip.tiled_size = block.bytes;
    mip.linear_size = static_cast<std::uint64_t>(mip.pitch_bytes) * mip.height;
    mip.tail = true;
    mip.tail_x = locations[level - first_tail].x;
    mip.tail_y = locations[level - first_tail].y;
  }
  // Tail block first, then the other levels from the smallest to level 0.
  std::uint64_t offset = first_tail < mip_count ? block.bytes : 0;
  for (std::uint32_t level = first_tail; level-- > 0;) {
    mips[level].tiled_offset = offset;
    mips[level].linear_offset = offset;
    offset += mips[level].tiled_size;
  }
  std::uint64_t linear_cursor = 0;
  for (std::uint32_t level = 0; level < first_tail; ++level)
    linear_cursor = std::max(linear_cursor, mips[level].linear_offset + mips[level].linear_size);
  linear_cursor = std::max(linear_cursor, offset);
  for (std::uint32_t level = first_tail; level < mip_count; ++level) {
    mips[level].tiled_offset = 0;
    mips[level].linear_offset = linear_cursor;
    linear_cursor += mips[level].linear_size;
  }
  if (offset != block_slice) return false;
  guest_bytes = block_slice;
  return true;
}

// Tiled byte offset of one element of one mip (single-slice). x/y are element
// coordinates within the mip (for BC formats, block coordinates).
inline std::uint64_t thin_element_offset(std::uint32_t mode, std::uint32_t element_bytes,
                                         const TiledMip& mip, std::uint32_t x, std::uint32_t y,
                                         std::uint32_t pipe_bank_xor) {
  SwizzleBlock block{};
  if (!swizzle_block(mode, element_bytes, block)) return 0;
  std::uint64_t block_index = 0;
  std::uint32_t sx = x, sy = y;
  if (mip.tail) {
    sx += mip.tail_x;
    sy += mip.tail_y;
  } else {
    block_index = static_cast<std::uint64_t>(y / block.height) * mip.blocks_per_row + x / block.width;
  }
  std::uint32_t within;
  if (swizzle_mode_equation(mode)) {
    const SwizzleEquation* eq = find_swizzle_equation(mode, element_bytes);
    if (eq == nullptr) return 0;
    within = equation_offset(*eq, sx, sy, 0) ^ (swizzle_mode_xor(mode) ? pipe_bank_xor : 0);
  } else {
    within = standard_offset(element_bytes, sx, sy);
    if (block.bytes > 4096) within ^= standard_64kb_extra(element_bytes, sx, sy);
    within &= block.bytes - 1;
  }
  return mip.tiled_offset + block_index * block.bytes + within;
}

// Detile one mip of a thin tiled surface into its linear region (rows of
// mip.pitch_bytes). src/dst are the surface bases of the guest copy and the
// linear staging buffer. elem_w/elem_h are the mip's size in elements (for BC
// formats, 4x4 blocks; otherwise texels).
inline void detile_thin_mip(std::uint32_t mode, std::uint32_t element_bytes, const TiledMip& mip,
                            std::uint32_t elem_w, std::uint32_t elem_h,
                            const std::uint8_t* src, std::uint8_t* dst, std::uint32_t pipe_bank_xor) {
  for (std::uint32_t y = 0; y < elem_h; ++y) {
    for (std::uint32_t x = 0; x < elem_w; ++x) {
      const std::uint64_t from = thin_element_offset(mode, element_bytes, mip, x, y, pipe_bank_xor);
      const std::uint64_t to = mip.linear_offset + static_cast<std::uint64_t>(y) * mip.pitch_bytes + static_cast<std::uint64_t>(x) * element_bytes;
      for (std::uint32_t b = 0; b < element_bytes; ++b) dst[to + b] = src[from + b];
    }
  }
}

}  // namespace pnr::graphics

#endif
