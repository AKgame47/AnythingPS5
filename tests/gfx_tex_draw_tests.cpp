// Texture draw tests, end to end through the real PM4 -> execute_draw path
// with guest T#/S# descriptors: plain textures (S17), thin-tiled (S22),
// extended formats + sRGB (S23), mips (S19), BC1-BC7/BC6H (S20/21/24/25),
// 2D arrays (S27), cubemaps (S28), array view-window semantics (S31).
// Split out of gfx_draw_tests.cpp in slice S31 because that file outgrew the
// practical size limit of the tooling that edits it. Synthetic GNM-style
// descriptors only; no real game data. GPL-2.0.
// Core slices C4d/C4e tests: SYNTHETIC GNM command buffer -> one Vulkan draw -> pixel check.
// Skips (with a message) when no Vulkan device exists. No real game data is involved.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "graphics/demo/cliptex_shaders.hpp"
#include "graphics/demo/tex_shaders.hpp"
#include "graphics/gfx_draw.hpp"
#include "graphics/gfx_tsharp.hpp"
#include "graphics/gfx_swizzle.hpp"
#include "graphics/gfx_vsharp.hpp"
#include "graphics/rdna_pixel.hpp"
#include "graphics/gpu_va.hpp"
#include "graphics/pm4.hpp"

namespace {
using namespace pnr::graphics;
std::uint32_t f2u(float f) { std::uint32_t u; std::memcpy(&u, &f, 4); return u; }

// Slice S17: textured draw. PVS3 clip blob, PPS2 pixel blob, T#/S# in guest
// memory, full-quad with uv varyings and a white vertex-colour tint.
struct TexScene {
  GpuAddressSpace mem;
  std::vector<std::uint32_t> vs_blob, ps_blob, mat;
  std::vector<float> vb;
  std::vector<std::uint8_t> target;
  std::vector<std::uint32_t> tsharp, ssharp, cmd;
  std::vector<std::uint8_t> texels;
  bool mode_cntl_set = false;        // write PA_SU_SC_MODE_CNTL (slice S18)
  std::uint32_t mode_cntl = 0;
  static constexpr std::uint32_t W = 64, H = 64;
  void make_tdesc(std::uint32_t w[8], std::uint64_t base, std::uint32_t tw, std::uint32_t th,
                  std::uint32_t pitch, std::uint32_t tiling, std::uint32_t last_level = 0,
                  std::uint32_t min_lod = 0, std::uint32_t df = 10, std::uint32_t nf = 0,
                  std::uint32_t type = 9, std::uint32_t last_array = 0,
                  std::uint32_t depth_field = 0xFFFFFFFFu) {
    std::uint64_t q0 = (base >> 8) & 0x3FFFFFFFFFull;
    q0 |= std::uint64_t(min_lod & 0xFFF) << 40;
    q0 |= std::uint64_t(df & 0x3F) << 52;  // data format (10 = 8_8_8_8, 35 = BC1, 37 = BC3)
    q0 |= std::uint64_t(nf & 0xF) << 58;   // number format (0 unorm, 7 float, 9 srgb)
    std::uint64_t q1 = std::uint64_t(tw - 1) | (std::uint64_t(th - 1) << 14);
    q1 |= std::uint64_t(1) << 35 | std::uint64_t(2) << 38 | std::uint64_t(3) << 41;
    q1 |= std::uint64_t(last_level & 0xF) << 48;
    q1 |= std::uint64_t(tiling & 0x1F) << 52;
    q1 |= std::uint64_t(type & 0xF) << 60;  // 9 = Color2D, 13 = Color2DArray
    std::uint64_t q2 = std::uint64_t(pitch - 1) << 13;
    q2 |= std::uint64_t(last_array & 0x1FFF) << 45;
    // Slice S31: the DEPTH field holds the allocation size -1 for arrays
    // (0 = one full cube for cubemaps). Typical descriptors set it equal to
    // last_array for arrays; an explicit argument overrides that default.
    if (depth_field == 0xFFFFFFFFu) depth_field = type == 13 ? last_array : 0;
    q2 |= std::uint64_t(depth_field & 0x1FFF);
    w[0] = std::uint32_t(q0); w[1] = std::uint32_t(q0 >> 32);
    w[2] = std::uint32_t(q1); w[3] = std::uint32_t(q1 >> 32);
    w[4] = std::uint32_t(q2); w[5] = std::uint32_t(q2 >> 32);
    w[6] = 0; w[7] = 0;
  }
  // uv_scale 1 = uv 0..1 across the quad; 2 = 0..2 (addressing test).
  void build(float uv_scale, std::uint32_t clamp_x, std::uint32_t mag_filter,
             std::uint32_t min_filter, std::uint32_t tiling = 0,
             std::uint32_t tex_side = 4, bool user_data = true) {
    vs_blob = make_pnr_vs3_blob(true);
    ps_blob = make_pnr_ps2_blob();
    mat.assign(16, 0); for (int i = 0; i < 4; ++i) mat[i * 5] = f2u(1.0f);
    const float px[6][2] = {{-1,-1},{1,-1},{1,1},{-1,-1},{1,1},{-1,1}};
    const float pu[6][2] = {{0,0},{1,0},{1,1},{0,0},{1,1},{0,1}};
    for (int v = 0; v < 6; ++v) {
      vb.push_back(px[v][0]); vb.push_back(px[v][1]); vb.push_back(0.0f);
      vb.push_back(1.0f); vb.push_back(1.0f); vb.push_back(1.0f);  // white tint
      vb.push_back(pu[v][0] * uv_scale); vb.push_back(pu[v][1] * uv_scale);
    }
    // checker texels: even texels white, odd black
    texels.assign(std::size_t(tex_side) * tex_side * 4, 0);
    for (std::uint32_t y = 0; y < tex_side; ++y)
      for (std::uint32_t x = 0; x < tex_side; ++x) {
        std::uint8_t v = ((x + y) & 1) ? 0 : 255;
        auto* p = &texels[(y * tex_side + x) * 4];
        p[0] = v; p[1] = v; p[2] = v; p[3] = 255;
      }
    std::uint32_t tw[8]; make_tdesc(tw, 0x700000, tex_side, tex_side, tex_side, tiling);
    tsharp.assign(tw, tw + 8);
    std::uint64_t sq0 = std::uint64_t(clamp_x & 7) | (std::uint64_t(2) << 3);  // clamp y
    std::uint64_t sq1 = std::uint64_t(mag_filter) << 20 | std::uint64_t(min_filter) << 22;
    ssharp = {std::uint32_t(sq0), std::uint32_t(sq0 >> 32), std::uint32_t(sq1), std::uint32_t(sq1 >> 32)};
    target.assign(W * H * 4, 0xCD);
    mem.map_external(0x100000, vs_blob.data(), vs_blob.size() * 4, "vs");
    mem.map_external(0x200000, ps_blob.data(), ps_blob.size() * 4, "ps");
    mem.map_external(0x300000, mat.data(), 64, "matrix");
    mem.map_external(0x400000, vb.data(), vb.size() * 4, "vertices");
    mem.map_external(0x500000, target.data(), target.size(), "target");
    mem.map_external(0x700000, texels.data(), texels.size(), "texture");
    mem.map_external(0x710000, tsharp.data(), tsharp.size() * 4, "tsharp");
    mem.map_external(0x711000, ssharp.data(), ssharp.size() * 4, "ssharp");
    pm4::Builder b;
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_VS, {0x100000 >> 8, 0});
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_PS, {0x200000 >> 8, 0});
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_USER_DATA_VS_0, {0x300000, 0, 0x400000, 0});
    if (user_data)
      b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_USER_DATA_PS_0, {0x710000, 0, 0x711000, 0});
    b.set_reg(pm4::SET_CONTEXT_REG, amd::PA_CL_VPORT_XSCALE, {f2u(W / 2.0f), f2u(W / 2.0f), f2u(-0.5f * float(H)), f2u(0.5f * float(H)), f2u(0.5f), f2u(0.5f)});
    b.set_context(amd::CB_COLOR0_BASE, 0x500000 >> 8).set_context(amd::CB_TARGET_MASK, 0xf);
    b.set_context(amd::PA_SC_SCREEN_SCISSOR_BR, (H << 16) | W);
    if (mode_cntl_set) b.set_context(amd::PA_SU_SC_MODE_CNTL, mode_cntl);
    b.set_uconfig(amd::VGT_PRIMITIVE_TYPE, 4).num_instances(1).draw_auto(6);
    cmd = b.dwords();
  }
  // Slice S20: compressed texture (BC1/BC3). side = level-0 side (multiple
  // of 4); each level is solid red, green, then blue cyclically. pitch 0 =
  // tight (pitch = side), as the S20 subset requires.
  static std::uint16_t rgb565(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return std::uint16_t(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
  }
  void bc_solid(std::uint8_t* blk, std::uint32_t df, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    if (df == 41) {  // BC7: mode-6 solid; raw 7-bit = v>>1, shared P-bit = v&1 (needs equal parity)
      for (int i = 0; i < 16; ++i) blk[i] = 0;
      const int raw[4] = {r >> 1, g >> 1, b >> 1, a >> 1};
      const std::uint32_t pb = (r & 1) && (g & 1) && (b & 1) && (a & 1) ? 1 : 0;
      int pos = 0;
      auto put = [&](std::uint64_t v, int n) { for (int i = 0; i < n; ++i, ++pos) blk[pos / 8] |= std::uint8_t(((v >> i) & 1) << (pos % 8)); };
      put(0x40, 7);
      for (int c = 0; c < 4; ++c) { put(raw[c], 7); put(raw[c], 7); }
      put(pb, 1); put(pb, 1);
      for (int i = 0; i < 16; ++i) put(0, i == 0 ? 3 : 4);
      return;
    }
    if (df == 40) {  // BC6H: mode 11 solid, 10-bit unsigned endpoints (callers use 0/255 only)
      for (int i = 0; i < 16; ++i) blk[i] = 0;
      const std::uint32_t w10[3] = {r == 255 ? 1023u : 0u, g == 255 ? 1023u : 0u, b == 255 ? 1023u : 0u};
      int pos = 0;
      auto put = [&](std::uint64_t v, int n) { for (int i = 0; i < n; ++i, ++pos) blk[pos / 8] |= std::uint8_t(((v >> i) & 1) << (pos % 8)); };
      put(0x03, 5);  // mode 11 (0x03)
      for (int c = 0; c < 3; ++c) { put(w10[c], 10); }        // RW GW BW
      for (int c = 0; c < 3; ++c) { put(w10[c], 10); }        // RX GX BX (explicit, untransformed)
      for (int i = 0; i < 16; ++i) put(0, i == 0 ? 3 : 4);    // indices 0 (anchor 3 bits)
      return;
    }
    if (df == 38) { blk[0] = r; blk[1] = r; for (int i = 2; i < 8; ++i) blk[i] = 0; return; }  // BC4: red only
    if (df == 39) {  // BC5: one value block per channel
      blk[0] = r; blk[1] = r; for (int i = 2; i < 8; ++i) blk[i] = 0;
      blk[8] = g; blk[9] = g; for (int i = 10; i < 16; ++i) blk[i] = 0;
      return;
    }
    if (df == 36 || df == 37) { blk[0] = a; blk[1] = a; for (int i = 2; i < 8; ++i) blk[i] = 0; blk += 8; }
    const std::uint16_t c = rgb565(r, g, b);
    blk[0] = std::uint8_t(c); blk[1] = std::uint8_t(c >> 8);
    blk[2] = std::uint8_t(c); blk[3] = std::uint8_t(c >> 8);
    for (int i = 4; i < 8; ++i) blk[i] = 0;
  }
  void build_bc(std::uint32_t df, float uv_scale, std::uint32_t side, std::uint32_t last_level,
                std::uint32_t mip_filter, std::uint32_t pitch = 0,
                std::uint32_t mag_filter = 1, std::uint32_t min_filter = 1, std::uint32_t nf = 0) {
    vs_blob = make_pnr_vs3_blob(true);
    ps_blob = make_pnr_ps2_blob();
    mat.assign(16, 0); for (int i = 0; i < 4; ++i) mat[i * 5] = f2u(1.0f);
    const float px[6][2] = {{-1,-1},{1,-1},{1,1},{-1,-1},{1,1},{-1,1}};
    const float pu[6][2] = {{0,0},{1,0},{1,1},{0,0},{1,1},{0,1}};
    for (int v = 0; v < 6; ++v) {
      vb.push_back(px[v][0]); vb.push_back(px[v][1]); vb.push_back(0.0f);
      vb.push_back(1.0f); vb.push_back(1.0f); vb.push_back(1.0f);  // white tint
      vb.push_back(pu[v][0] * uv_scale); vb.push_back(pu[v][1] * uv_scale);
    }
    const std::uint8_t cols[3][3] = {{255,0,0},{0,255,0},{0,0,255}};
    const std::uint32_t bb = df == 35 || df == 38 ? 8 : 16;
    for (std::uint32_t l = 0; l <= last_level; ++l) {
      const std::uint32_t lw = side >> l ? side >> l : 1;
      const std::uint32_t nb = (lw + 3) / 4;
      const size_t at = texels.size();
      texels.resize(at + std::size_t(nb) * nb * bb);
      for (std::uint32_t i = 0; i < nb * nb; ++i)
        bc_solid(&texels[at + i * bb], df, cols[l % 3][0], cols[l % 3][1], cols[l % 3][2], 255);
    }
    std::uint32_t tw[8]; make_tdesc(tw, 0x700000, side, side, pitch ? pitch : side, 0, last_level, 0, df, nf);
    tsharp.assign(tw, tw + 8);
    std::uint64_t sq0 = 0;  // wrap x and y
    std::uint64_t sq1 = std::uint64_t(mag_filter) << 20 | std::uint64_t(min_filter) << 22;
    sq1 |= std::uint64_t(mip_filter & 3) << 26;
    ssharp = {std::uint32_t(sq0), std::uint32_t(sq0 >> 32), std::uint32_t(sq1), std::uint32_t(sq1 >> 32)};
    target.assign(W * H * 4, 0xCD);
    mem.map_external(0x100000, vs_blob.data(), vs_blob.size() * 4, "vs");
    mem.map_external(0x200000, ps_blob.data(), ps_blob.size() * 4, "ps");
    mem.map_external(0x300000, mat.data(), 64, "matrix");
    mem.map_external(0x400000, vb.data(), vb.size() * 4, "vertices");
    mem.map_external(0x500000, target.data(), target.size(), "target");
    mem.map_external(0x700000, texels.data(), texels.size(), "texture");
    mem.map_external(0x710000, tsharp.data(), tsharp.size() * 4, "tsharp");
    mem.map_external(0x711000, ssharp.data(), ssharp.size() * 4, "ssharp");
    pm4::Builder b;
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_VS, {0x100000 >> 8, 0});
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_PS, {0x200000 >> 8, 0});
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_USER_DATA_VS_0, {0x300000, 0, 0x400000, 0});
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_USER_DATA_PS_0, {0x710000, 0, 0x711000, 0});
    b.set_reg(pm4::SET_CONTEXT_REG, amd::PA_CL_VPORT_XSCALE, {f2u(W / 2.0f), f2u(W / 2.0f), f2u(-0.5f * float(H)), f2u(0.5f * float(H)), f2u(0.5f), f2u(0.5f)});
    b.set_context(amd::CB_COLOR0_BASE, 0x500000 >> 8).set_context(amd::CB_TARGET_MASK, 0xf);
    b.set_context(amd::PA_SC_SCREEN_SCISSOR_BR, (H << 16) | W);
    b.set_uconfig(amd::VGT_PRIMITIVE_TYPE, 4).num_instances(1).draw_auto(6);
    cmd = b.dwords();
  }
  // Slice S19: a mip chain of solid colours - L0 4x4 red, L1 2x2 green,
  // L2 1x1 blue, packed per the S19 guest layout (level 0 at the T# pitch,
  // deeper levels tightly pitched one after another).
  void build_mips(float uv_scale, std::uint32_t mip_filter,
                  std::uint32_t s_min_lod = 0, std::uint32_t s_max_lod = 0,
                  std::uint32_t t_min_lod = 0) {
    vs_blob = make_pnr_vs3_blob(true);
    ps_blob = make_pnr_ps2_blob();
    mat.assign(16, 0); for (int i = 0; i < 4; ++i) mat[i * 5] = f2u(1.0f);
    const float px[6][2] = {{-1,-1},{1,-1},{1,1},{-1,-1},{1,1},{-1,1}};
    const float pu[6][2] = {{0,0},{1,0},{1,1},{0,0},{1,1},{0,1}};
    for (int v = 0; v < 6; ++v) {
      vb.push_back(px[v][0]); vb.push_back(px[v][1]); vb.push_back(0.0f);
      vb.push_back(1.0f); vb.push_back(1.0f); vb.push_back(1.0f);  // white tint
      vb.push_back(pu[v][0] * uv_scale); vb.push_back(pu[v][1] * uv_scale);
    }
    texels.assign(4 * 4 * 4 + 2 * 2 * 4 + 4, 255);
    for (int i = 0; i < 16; ++i) { texels[i * 4 + 1] = 0; texels[i * 4 + 2] = 0; }          // L0 red
    for (int i = 0; i < 4; ++i) { auto* p = &texels[64 + i * 4]; p[0] = 0; p[2] = 0; }        // L1 green
    { auto* p = &texels[80]; p[0] = 0; p[1] = 0; }                                            // L2 blue
    std::uint32_t tw[8]; make_tdesc(tw, 0x700000, 4, 4, 4, 0, 2, t_min_lod);
    tsharp.assign(tw, tw + 8);
    std::uint64_t sq0 = std::uint64_t(s_min_lod & 0xFFF) << 32 |
                        std::uint64_t(s_max_lod & 0xFFF) << 44;  // wrap x and y
    std::uint64_t sq1 = std::uint64_t(1) << 20 | std::uint64_t(1) << 22;  // bilinear mag/min
    sq1 |= std::uint64_t(mip_filter & 3) << 26;
    ssharp = {std::uint32_t(sq0), std::uint32_t(sq0 >> 32), std::uint32_t(sq1), std::uint32_t(sq1 >> 32)};
    target.assign(W * H * 4, 0xCD);
    mem.map_external(0x100000, vs_blob.data(), vs_blob.size() * 4, "vs");
    mem.map_external(0x200000, ps_blob.data(), ps_blob.size() * 4, "ps");
    mem.map_external(0x300000, mat.data(), 64, "matrix");
    mem.map_external(0x400000, vb.data(), vb.size() * 4, "vertices");
    mem.map_external(0x500000, target.data(), target.size(), "target");
    mem.map_external(0x700000, texels.data(), texels.size(), "texture");
    mem.map_external(0x710000, tsharp.data(), tsharp.size() * 4, "tsharp");
    mem.map_external(0x711000, ssharp.data(), ssharp.size() * 4, "ssharp");
    pm4::Builder b;
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_VS, {0x100000 >> 8, 0});
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_PS, {0x200000 >> 8, 0});
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_USER_DATA_VS_0, {0x300000, 0, 0x400000, 0});
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_USER_DATA_PS_0, {0x710000, 0, 0x711000, 0});
    b.set_reg(pm4::SET_CONTEXT_REG, amd::PA_CL_VPORT_XSCALE, {f2u(W / 2.0f), f2u(W / 2.0f), f2u(-0.5f * float(H)), f2u(0.5f * float(H)), f2u(0.5f), f2u(0.5f)});
    b.set_context(amd::CB_COLOR0_BASE, 0x500000 >> 8).set_context(amd::CB_TARGET_MASK, 0xf);
    b.set_context(amd::PA_SC_SCREEN_SCISSOR_BR, (H << 16) | W);
    b.set_uconfig(amd::VGT_PRIMITIVE_TYPE, 4).num_instances(1).draw_auto(6);
    cmd = b.dwords();
  }
  // Slice S22: thin-tiled (swizzled) texture. df 10 = 8_8_8_8 unorm, df 35 =
  // BC1. Single level: checker for df 10, solid red for BC. Chain (last_level
  // > 0, df 10): solid red/green/blue levels like build_mips. The surface is
  // laid out by the hardware thin block geometry (gfx_swizzle.hpp), including
  // tail-packed levels.
  void build_tiled(std::uint32_t mode, std::uint32_t df, std::uint32_t side, std::uint32_t last_level,
                   float uv_scale, std::uint32_t mip_filter, std::uint32_t nf = 0) {
    vs_blob = make_pnr_vs3_blob(true);
    ps_blob = make_pnr_ps2_blob();
    mat.assign(16, 0); for (int i = 0; i < 4; ++i) mat[i * 5] = f2u(1.0f);
    const float px[6][2] = {{-1,-1},{1,-1},{1,1},{-1,-1},{1,1},{-1,1}};
    const float pu[6][2] = {{0,0},{1,0},{1,1},{0,0},{1,1},{0,1}};
    for (int v = 0; v < 6; ++v) {
      vb.push_back(px[v][0]); vb.push_back(px[v][1]); vb.push_back(0.0f);
      vb.push_back(1.0f); vb.push_back(1.0f); vb.push_back(1.0f);  // white tint
      vb.push_back(pu[v][0] * uv_scale); vb.push_back(pu[v][1] * uv_scale);
    }
    const bool bc = df >= 35 && df <= 39;
    PlainFormat pf{};
    if (!bc) plain_format_info(df, nf, pf);
    const std::uint32_t bb = bc ? (df == 35 || df == 38 ? 8 : 16) : pf.element_bytes;
    TiledMip layout[kMaxTiledMips];
    std::uint64_t guest_bytes = 0;
    if (!thin_mip_layout(mode, bb, bc ? 4 : 1, bc ? 4 : 1, side, side, last_level + 1,
                         layout, kMaxTiledMips, guest_bytes)) { cmd.clear(); return; }
    texels.assign(guest_bytes, 0);
    for (std::uint32_t l = 0; l <= last_level; ++l) {
      const std::uint32_t lw = side >> l ? side >> l : 1, lh = lw;
      const std::uint32_t ew = bc ? (lw + 3) / 4 : lw, eh = bc ? (lh + 3) / 4 : lh;
      for (std::uint32_t y = 0; y < eh; ++y)
        for (std::uint32_t x = 0; x < ew; ++x) {
          const auto off = thin_element_offset(mode, bb, layout[l], x, y, 0);
          if (!bc && df != 10) {
            // S23 plain formats: every channel gets on/off max values so the
            // converted RGBA8 is a checker of (255..,0,255) vs (0..,0,255).
            const bool on = ((x + y) & 1) == 0;
            for (std::uint32_t c = 0; c < pf.channels; ++c) {
              if (pf.element_bytes / pf.channels == 2) {
                const std::uint16_t w = on ? (pf.is_float ? 0x3C00 : 0xFFFF) : 0;
                texels[off + c * 2] = std::uint8_t(w); texels[off + c * 2 + 1] = std::uint8_t(w >> 8);
              } else {
                texels[off + c] = on ? 255 : 0;
              }
            }
          } else if (!bc) {
            std::uint8_t r = 255, g = 255, bch = 255;
            if (last_level == 0) { const std::uint8_t v = ((x + y) & 1) ? 0 : 255; r = g = bch = v; }
            else if (l == 0) { g = 0; bch = 0; } else if (l == 1) { r = 0; bch = 0; } else { r = 0; g = 0; }
            texels[off + 0] = r; texels[off + 1] = g; texels[off + 2] = bch; texels[off + 3] = 255;
          } else {
            std::uint8_t blk[16]; bc_solid(blk, df, 255, 0, 0, 255);
            std::memcpy(&texels[off], blk, bb);
          }
        }
    }
    std::uint32_t tw[8]; make_tdesc(tw, 0x700000, side, side, side, mode, last_level, 0, df, nf);
    tsharp.assign(tw, tw + 8);
    std::uint64_t sq0 = 0;  // wrap x and y
    const std::uint32_t mf = last_level == 0 ? 0 : 1;  // point for single-level, bilinear for chains
    std::uint64_t sq1 = std::uint64_t(mf) << 20 | std::uint64_t(mf) << 22;
    sq1 |= std::uint64_t(mip_filter & 3) << 26;
    ssharp = {std::uint32_t(sq0), std::uint32_t(sq0 >> 32), std::uint32_t(sq1), std::uint32_t(sq1 >> 32)};
    target.assign(W * H * 4, 0xCD);
    mem.map_external(0x100000, vs_blob.data(), vs_blob.size() * 4, "vs");
    mem.map_external(0x200000, ps_blob.data(), ps_blob.size() * 4, "ps");
    mem.map_external(0x300000, mat.data(), 64, "matrix");
    mem.map_external(0x400000, vb.data(), vb.size() * 4, "vertices");
    mem.map_external(0x500000, target.data(), target.size(), "target");
    mem.map_external(0x700000, texels.data(), texels.size(), "texture");
    mem.map_external(0x710000, tsharp.data(), tsharp.size() * 4, "tsharp");
    mem.map_external(0x711000, ssharp.data(), ssharp.size() * 4, "ssharp");
    pm4::Builder b;
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_VS, {0x100000 >> 8, 0});
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_PS, {0x200000 >> 8, 0});
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_USER_DATA_VS_0, {0x300000, 0, 0x400000, 0});
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_USER_DATA_PS_0, {0x710000, 0, 0x711000, 0});
    b.set_reg(pm4::SET_CONTEXT_REG, amd::PA_CL_VPORT_XSCALE, {f2u(W / 2.0f), f2u(W / 2.0f), f2u(-0.5f * float(H)), f2u(0.5f * float(H)), f2u(0.5f), f2u(0.5f)});
    b.set_context(amd::CB_COLOR0_BASE, 0x500000 >> 8).set_context(amd::CB_TARGET_MASK, 0xf);
    b.set_context(amd::PA_SC_SCREEN_SCISSOR_BR, (H << 16) | W);
    b.set_uconfig(amd::VGT_PRIMITIVE_TYPE, 4).num_instances(1).draw_auto(6);
    cmd = b.dwords();
  }
  // Slice S23: single-level linear plain-format texture from raw guest bytes.
  void build_plain(std::uint32_t df, std::uint32_t nf, std::uint32_t side,
                   const std::vector<std::uint8_t>& bytes) {
    vs_blob = make_pnr_vs3_blob(true);
    ps_blob = make_pnr_ps2_blob();
    mat.assign(16, 0); for (int i = 0; i < 4; ++i) mat[i * 5] = f2u(1.0f);
    const float pxv[6][2] = {{-1,-1},{1,-1},{1,1},{-1,-1},{1,1},{-1,1}};
    const float pu[6][2] = {{0,0},{1,0},{1,1},{0,0},{1,1},{0,1}};
    for (int v = 0; v < 6; ++v) {
      vb.push_back(pxv[v][0]); vb.push_back(pxv[v][1]); vb.push_back(0.0f);
      vb.push_back(1.0f); vb.push_back(1.0f); vb.push_back(1.0f);  // white tint
      vb.push_back(pu[v][0]); vb.push_back(pu[v][1]);
    }
    texels = bytes;
    std::uint32_t tw[8]; make_tdesc(tw, 0x700000, side, side, side, 0, 0, 0, df, nf);
    tsharp.assign(tw, tw + 8);
    std::uint64_t sq0 = 0;  // wrap x and y
    std::uint64_t sq1 = 0;  // point mag/min, mip filter none
    ssharp = {std::uint32_t(sq0), std::uint32_t(sq0 >> 32), std::uint32_t(sq1), std::uint32_t(sq1 >> 32)};
    target.assign(W * H * 4, 0xCD);
    mem.map_external(0x100000, vs_blob.data(), vs_blob.size() * 4, "vs");
    mem.map_external(0x200000, ps_blob.data(), ps_blob.size() * 4, "ps");
    mem.map_external(0x300000, mat.data(), 64, "matrix");
    mem.map_external(0x400000, vb.data(), vb.size() * 4, "vertices");
    mem.map_external(0x500000, target.data(), target.size(), "target");
    mem.map_external(0x700000, texels.data(), texels.size(), "texture");
    mem.map_external(0x710000, tsharp.data(), tsharp.size() * 4, "tsharp");
    mem.map_external(0x711000, ssharp.data(), ssharp.size() * 4, "ssharp");
    pm4::Builder b;
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_VS, {0x100000 >> 8, 0});
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_PS, {0x200000 >> 8, 0});
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_USER_DATA_VS_0, {0x300000, 0, 0x400000, 0});
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_USER_DATA_PS_0, {0x710000, 0, 0x711000, 0});
    b.set_reg(pm4::SET_CONTEXT_REG, amd::PA_CL_VPORT_XSCALE, {f2u(W / 2.0f), f2u(W / 2.0f), f2u(-0.5f * float(H)), f2u(0.5f * float(H)), f2u(0.5f), f2u(0.5f)});
    b.set_context(amd::CB_COLOR0_BASE, 0x500000 >> 8).set_context(amd::CB_TARGET_MASK, 0xf);
    b.set_context(amd::PA_SC_SCREEN_SCISSOR_BR, (H << 16) | W);
    b.set_uconfig(amd::VGT_PRIMITIVE_TYPE, 4).num_instances(1).draw_auto(6);
    cmd = b.dwords();
  }
  // Slice S27: 2D texture array (PPS2 v1 blob). nslices solid-colour slices
  // per level in the level-major guest layout; one fullscreen quad whose
  // 9-float vertex records carry the slice index after the uv pair. Colour
  // of (level l, slice s) = cols[(s + 2*l) % 4], so slice and level are
  // independently visible. df 10 = 8_8_8_8 unorm, df 35 = BC1.
  // Slice S31: alloc is the ALLOCATION layer count (T# DEPTH field + 1);
  // when it exceeds nslices the extra slices hold distinct colours and the
  // sampled view must stay within the first nslices.
  void build_array(std::uint32_t nslices, float layer, std::uint32_t df = 10,
                   std::uint32_t last_level = 0, std::uint32_t mip_filter = 0,
                   std::uint32_t t_min_lod = 0, std::uint32_t tiling = 0,
                   bool array_blob = true, std::uint32_t alloc = 0) {
    if (alloc == 0) alloc = nslices;
    vs_blob = make_pnr_vs3_blob(true);
    ps_blob = array_blob ? make_pnr_ps2_array_blob() : make_pnr_ps2_blob();
    mat.assign(16, 0); for (int i = 0; i < 4; ++i) mat[i * 5] = f2u(1.0f);
    const float px[6][2] = {{-1,-1},{1,-1},{1,1},{-1,-1},{1,1},{-1,1}};
    const float pu[6][2] = {{0,0},{1,0},{1,1},{0,0},{1,1},{0,1}};
    for (int v = 0; v < 6; ++v) {
      vb.push_back(px[v][0]); vb.push_back(px[v][1]); vb.push_back(0.0f);
      vb.push_back(1.0f); vb.push_back(1.0f); vb.push_back(1.0f);  // white tint
      vb.push_back(pu[v][0]); vb.push_back(pu[v][1]);
      vb.push_back(layer);  // slice index
    }
    const std::uint32_t side = 8;
    const std::uint8_t cols[4][3] = {{255,0,0},{0,255,0},{0,0,255},{255,255,255}};
    for (std::uint32_t l = 0; l <= last_level; ++l) {
      const std::uint32_t lw = side >> l ? side >> l : 1;
      for (std::uint32_t s = 0; s < alloc; ++s) {
        const auto& c = cols[(s + 2 * l) % 4];
        if (df == 35) {
          const std::uint32_t nb = (lw + 3) / 4;
          const size_t at = texels.size();
          texels.resize(at + std::size_t(nb) * nb * 8);
          for (std::uint32_t i = 0; i < nb * nb; ++i)
            bc_solid(&texels[at + i * 8], df, c[0], c[1], c[2], 255);
        } else {
          const size_t at = texels.size();
          texels.resize(at + std::size_t(lw) * lw * 4);
          for (std::uint32_t i = 0; i < lw * lw; ++i) {
            auto* p = &texels[at + i * 4];
            p[0] = c[0]; p[1] = c[1]; p[2] = c[2]; p[3] = 255;
          }
        }
      }
    }
    std::uint32_t tw[8];
    make_tdesc(tw, 0x700000, side, side, side, tiling, last_level, t_min_lod, df, 0, 13, nslices - 1,
               alloc - 1);
    tsharp.assign(tw, tw + 8);
    std::uint64_t sq0 = std::uint64_t(2) << 3;  // wrap x, clamp y
    std::uint64_t sq1 = std::uint64_t(mip_filter & 3) << 26;  // point mag/min
    ssharp = {std::uint32_t(sq0), std::uint32_t(sq0 >> 32), std::uint32_t(sq1), std::uint32_t(sq1 >> 32)};
    target.assign(W * H * 4, 0xCD);
    mem.map_external(0x100000, vs_blob.data(), vs_blob.size() * 4, "vs");
    mem.map_external(0x200000, ps_blob.data(), ps_blob.size() * 4, "ps");
    mem.map_external(0x300000, mat.data(), 64, "matrix");
    mem.map_external(0x400000, vb.data(), vb.size() * 4, "vertices");
    mem.map_external(0x500000, target.data(), target.size(), "target");
    mem.map_external(0x700000, texels.data(), texels.size(), "texture");
    mem.map_external(0x710000, tsharp.data(), tsharp.size() * 4, "tsharp");
    mem.map_external(0x711000, ssharp.data(), ssharp.size() * 4, "ssharp");
    pm4::Builder b;
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_VS, {0x100000 >> 8, 0});
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_PS, {0x200000 >> 8, 0});
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_USER_DATA_VS_0, {0x300000, 0, 0x400000, 0});
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_USER_DATA_PS_0, {0x710000, 0, 0x711000, 0});
    b.set_reg(pm4::SET_CONTEXT_REG, amd::PA_CL_VPORT_XSCALE, {f2u(W / 2.0f), f2u(W / 2.0f), f2u(-0.5f * float(H)), f2u(0.5f * float(H)), f2u(0.5f), f2u(0.5f)});
    b.set_context(amd::CB_COLOR0_BASE, 0x500000 >> 8).set_context(amd::CB_TARGET_MASK, 0xf);
    b.set_context(amd::PA_SC_SCREEN_SCISSOR_BR, (H << 16) | W);
    b.set_uconfig(amd::VGT_PRIMITIVE_TYPE, 4).num_instances(1).draw_auto(6);
    cmd = b.dwords();
  }
  // Slice S28: cubemap (PPS2 v2 blob). Six solid-colour faces per level in
  // the S27 level-major layout (+X red, -X green, +Y blue, -Y white,
  // +Z yellow, -Z magenta, halved per mip level); one fullscreen quad whose
  // 9-float records carry a constant sampling direction. blob_v picks the
  // PPS2 blob version for the mismatch refusals.
  void build_cube(float dx, float dy, float dz, std::uint32_t last_level = 0,
                  std::uint32_t mip_filter = 0, std::uint32_t t_min_lod = 0,
                  std::uint32_t tiling = 0, std::uint32_t blob_v = 2) {
    vs_blob = make_pnr_vs3_blob(true);
    ps_blob = blob_v == 2 ? make_pnr_ps2_cube_blob() : blob_v == 1 ? make_pnr_ps2_array_blob() : make_pnr_ps2_blob();
    mat.assign(16, 0); for (int i = 0; i < 4; ++i) mat[i * 5] = f2u(1.0f);
    const float px[6][2] = {{-1,-1},{1,-1},{1,1},{-1,-1},{1,1},{-1,1}};
    for (int v = 0; v < 6; ++v) {
      vb.push_back(px[v][0]); vb.push_back(px[v][1]); vb.push_back(0.0f);
      vb.push_back(1.0f); vb.push_back(1.0f); vb.push_back(1.0f);  // white tint
      vb.push_back(dx); vb.push_back(dy); vb.push_back(dz);
    }
    const std::uint32_t side = 8;
    const std::uint8_t cols[6][3] = {{255,0,0},{0,255,0},{0,0,255},{255,255,255},{255,255,0},{255,0,255}};
    for (std::uint32_t l = 0; l <= last_level; ++l) {
      const std::uint32_t lw = side >> l ? side >> l : 1;
      for (std::uint32_t s = 0; s < 6; ++s) {
        const size_t at = texels.size();
        texels.resize(at + std::size_t(lw) * lw * 4);
        for (std::uint32_t i = 0; i < lw * lw; ++i) {
          auto* p = &texels[at + i * 4];
          p[0] = std::uint8_t(cols[s][0] >> l); p[1] = std::uint8_t(cols[s][1] >> l);
          p[2] = std::uint8_t(cols[s][2] >> l); p[3] = 255;
        }
      }
    }
    std::uint32_t tw[8];
    make_tdesc(tw, 0x700000, side, side, side, tiling, last_level, t_min_lod, 10, 0, 11, 5);
    tsharp.assign(tw, tw + 8);
    std::uint64_t sq0 = std::uint64_t(2) << 3;  // wrap x, clamp y
    std::uint64_t sq1 = std::uint64_t(mip_filter & 3) << 26;  // point mag/min
    ssharp = {std::uint32_t(sq0), std::uint32_t(sq0 >> 32), std::uint32_t(sq1), std::uint32_t(sq1 >> 32)};
    target.assign(W * H * 4, 0xCD);
    mem.map_external(0x100000, vs_blob.data(), vs_blob.size() * 4, "vs");
    mem.map_external(0x200000, ps_blob.data(), ps_blob.size() * 4, "ps");
    mem.map_external(0x300000, mat.data(), 64, "matrix");
    mem.map_external(0x400000, vb.data(), vb.size() * 4, "vertices");
    mem.map_external(0x500000, target.data(), target.size(), "target");
    mem.map_external(0x700000, texels.data(), texels.size(), "texture");
    mem.map_external(0x710000, tsharp.data(), tsharp.size() * 4, "tsharp");
    mem.map_external(0x711000, ssharp.data(), ssharp.size() * 4, "ssharp");
    pm4::Builder b;
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_VS, {0x100000 >> 8, 0});
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_PS, {0x200000 >> 8, 0});
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_USER_DATA_VS_0, {0x300000, 0, 0x400000, 0});
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_USER_DATA_PS_0, {0x710000, 0, 0x711000, 0});
    b.set_reg(pm4::SET_CONTEXT_REG, amd::PA_CL_VPORT_XSCALE, {f2u(W / 2.0f), f2u(W / 2.0f), f2u(-0.5f * float(H)), f2u(0.5f * float(H)), f2u(0.5f), f2u(0.5f)});
    b.set_context(amd::CB_COLOR0_BASE, 0x500000 >> 8).set_context(amd::CB_TARGET_MASK, 0xf);
    b.set_context(amd::PA_SC_SCREEN_SCISSOR_BR, (H << 16) | W);
    b.set_uconfig(amd::VGT_PRIMITIVE_TYPE, 4).num_instances(1).draw_auto(6);
    cmd = b.dwords();
  }
  DrawResult run(VulkanBackend& vk) {
    auto sum = pm4::interpret(cmd.data(), cmd.size());
    if (sum.draws.size() != 1) { DrawResult r; r.error = "no draw parsed"; return r; }
    return execute_draw(vk, mem, sum.draws[0]);
  }
};
}  // namespace

void pnr_gfx_tex_draw_tests(int& pass, int& fail) {
  auto check = [&](bool ok, const std::string& l) { if (ok) ++pass; else ++fail; std::printf("%s %s\n", ok ? "PASS" : "FAIL", l.c_str()); };
  VulkanBackend vk;
  auto st = vk.open();
  if (!st) { std::printf("SKIP gfx-tex-draw: no Vulkan loader: %s\n", st.error.message.c_str()); return; }
  st = vk.create_instance("pnr-gfx-tex-draw-tests");
  if (!st || vk.devices().empty()) { std::printf("SKIP gfx-tex-draw: no Vulkan device\n"); return; }
  st = vk.create_device(0);
  if (!st) { std::printf("SKIP gfx-tex-draw: device creation failed\n"); return; }
  // Slice S17: textured draws (PPS2 blob, T#/S# in guest memory).
  {
    TexScene s; s.build(1.0f, 0, 0, 0);  // uv 0..1, wrap x, point filters
    auto r = s.run(vk);
    if (!r.ok) std::printf("  pps2 draw error: %s\n", r.error.c_str());
    check(r.ok && r.notes.find("PPS2") != std::string::npos, "pps2: textured draw ok, result states the PPS2 pixel stage");
    if (r.ok) {
      // 4x4 checker over 64x64 with nearest sampling: exact 16x16 blocks,
      // row order either direct or flipped by the clip-y chain.
      std::vector<std::uint8_t> want(64 * 64 * 4), wantf(64 * 64 * 4);
      for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
          std::uint8_t v = (((x / 16) + (y / 16)) & 1) ? 0 : 255;
          std::uint8_t vf = (((x / 16) + ((63 - y) / 16)) & 1) ? 0 : 255;
          auto* p = &want[(y * 64 + x) * 4]; p[0] = v; p[1] = v; p[2] = v; p[3] = 255;
          auto* q = &wantf[(y * 64 + x) * 4]; q[0] = vf; q[1] = vf; q[2] = vf; q[3] = 255;
        }
      const bool direct = std::memcmp(r.rgba.data(), want.data(), want.size()) == 0;
      const bool flipped = std::memcmp(r.rgba.data(), wantf.data(), wantf.size()) == 0;
      check(direct || flipped, "pps2: 4x4 checker byte-exact in 16x16 blocks (nearest)");
      check(std::memcmp(s.target.data(), r.rgba.data(), r.rgba.size()) == 0, "pps2: target memory holds the textured pixels");
    }
    TexScene sw; sw.build(2.0f, 0, 0, 0);  // uv 0..2 with wrap x: checker repeats every 8 px in x
    auto rw = sw.run(vk);
    check(rw.ok, "pps2: wrap-addressed draw ok");
    if (rw.ok) {
      // uv 0..2 wraps once: 8-pixel texel blocks, full pattern repeats at 32.
      bool block8 = true, period32 = true, alternate = true;
      for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
          const auto* a = &rw.rgba[(y * 64 + x) * 4];
          if (x % 8 != 7 && a[0] != rw.rgba[(y * 64 + x + 1) * 4]) block8 = false;
          if (x + 32 < 64 && a[0] != rw.rgba[(y * 64 + x + 32) * 4]) period32 = false;
          if (x % 16 < 8 && x + 8 < 64 && a[0] == rw.rgba[(y * 64 + x + 8) * 4]) alternate = false;
        }
      check(block8, "pps2: wrap draw keeps 8-pixel texel blocks");
      check(period32 && alternate, "pps2: wrap addressing repeats the checker pattern every 32 pixels in x");
    }
    // uv scale puts pixel (8,8) exactly on texel (0,0)'s centre and the
    // target centre on the four-texel midpoint.
    TexScene sl; sl.build(16.0f / 8.5f, 0, 1, 1, 0, 2);  // 2x2 checker, bilinear
    auto rl = sl.run(vk);
    TexScene sm; sm.build(32.0f / 32.5f, 0, 1, 1, 0, 2);  // pixel (32,32) at uv (0.5,0.5) exactly
    auto rm = sm.run(vk);
    if (rm.ok) {
      const auto* mid = &rm.rgba[(32 * 64 + 32) * 4];
      check(mid[0] >= 120 && mid[0] <= 135 && mid[1] == mid[0] && mid[2] == mid[0],
            "pps2: bilinear midpoint is the four-texel average (127/128)");
    } else check(false, "pps2: bilinear midpoint draw ok");
    if (rl.ok) {
      check(rl.ok, "pps2: bilinear draw ok");
      // Pixel (8,8) lands exactly on texel (0,0)'s centre (either
      // orientation): the bilinear sample must be that texel exactly.
      const auto* ctr = &rl.rgba[(8 * 64 + 8) * 4];
      check(((ctr[0] == 0 && ctr[1] == 0 && ctr[2] == 0) || (ctr[0] == 255 && ctr[1] == 255 && ctr[2] == 255)),
            "pps2: bilinear keeps texel centres exact");
    }
    // Refusals.
    { TexScene b1; b1.build(1.0f, 0, 0, 0, 0, 4, false);   // no PS user data
      auto r1 = b1.run(vk);
      check(!r1.ok && r1.error.find("user data") != std::string::npos, "pps2: missing T#/S# pointers refused"); }
    { TexScene b2; b2.build(1.0f, 0, 0, 0, 13);              // unknown SW_MODE T#
      auto r2 = b2.run(vk);
      check(!r2.ok && r2.error.find("tiling") != std::string::npos, "pps2: tiled texture refused"); }
    { TexScene b3; b3.build(1.0f, 6, 0, 0);                  // border clamp mode
      auto r3 = b3.run(vk);
      check(!r3.ok && r3.error.find("clamp") != std::string::npos, "pps2: border clamp refused"); }
    { TexScene b4; b4.build(1.0f, 0, 0, 0, 0, 4);
      b4.vs_blob = make_pnr_vs4_blob();                       // PPS2 + PVS4 not implemented
      b4.mem.unmap(0x100000);
      b4.mem.map_external(0x100000, b4.vs_blob.data(), b4.vs_blob.size() * 4, "vs4");
      auto r4 = b4.run(vk);
      check(!r4.ok && r4.error.find("not implemented") != std::string::npos, "pps2: PVS4 combination refused"); }
  }

  // ---- slice S27: 2D texture arrays (Color2DArray T#, PPS2 v1 blob) ----
  {
    auto solid = [&](const DrawResult& r, std::uint8_t cr, std::uint8_t cg, std::uint8_t cb) {
      for (int i = 0; i < 64 * 64; ++i) {
        const auto* p = &r.rgba[i * 4];
        if (p[0] != cr || p[1] != cg || p[2] != cb || p[3] != 255) return false;
      }
      return true;
    };
    { TexScene s; s.build_array(2, 0.0f);
      auto r = s.run(vk);
      if (!r.ok) std::printf("  array draw error: %s\n", r.error.c_str());
      check(r.ok && r.notes.find("2D texture array") != std::string::npos,
            "array: PPS2 v1 draw ok, result states the 2D array stage (S27)");
      check(r.ok && solid(r, 255, 0, 0), "array: slice 0 draws solid red byte-exact (S27)"); }
    { TexScene s; s.build_array(2, 1.0f);
      auto r = s.run(vk);
      check(r.ok && solid(r, 0, 255, 0), "array: slice 1 draws solid green byte-exact (S27)"); }
    { TexScene s; s.build_array(2, 1.0f, 35);  // BC1 slices
      auto r = s.run(vk);
      check(r.ok && solid(r, 0, 255, 0), "array: BC1 slice 1 draws solid green byte-exact (S27)"); }
    { TexScene s; s.build_array(2, 1.0f, 10, 1, 1, 256);  // point mips, T# min_lod 1.0
      auto r = s.run(vk);
      check(r.ok && solid(r, 255, 255, 255), "array: mip level 1 slice 1 draws solid white byte-exact (S27)"); }
    // Refusals.
    { TexScene s; s.build_array(2, 0.0f, 10, 0, 0, 0, 0, false);  // v0 blob, array T#
      auto r = s.run(vk);
      check(!r.ok && r.error.find("does not match the T# image type") != std::string::npos,
            "array: v0 blob with an array T# is refused (S27)"); }
    { TexScene s; s.build_array(2, 0.0f);  // v1 blob, swap in a Color2D T#
      std::uint32_t tw[8]; s.make_tdesc(tw, 0x700000, 8, 8, 8, 0);
      s.tsharp.assign(tw, tw + 8);
      s.mem.unmap(0x710000);
      s.mem.map_external(0x710000, s.tsharp.data(), s.tsharp.size() * 4, "tsharp2d");
      auto r = s.run(vk);
      check(!r.ok && r.error.find("does not match the T# image type") != std::string::npos,
            "array: v1 blob with a 2D T# is refused (S27)"); }
    { TexScene s; s.build_array(2, 0.0f, 10, 0, 0, 0, 9);  // tiled array T#
      auto r = s.run(vk);
      check(!r.ok && r.error.find("array") != std::string::npos,
            "array: tiled texture array is refused (S27)"); }
  }

  // ---- slice S28: cubemaps (Cube T#, PPS2 v2 blob) ----
  {
    auto solid = [&](const DrawResult& r, std::uint8_t cr, std::uint8_t cg, std::uint8_t cb) {
      for (int i = 0; i < 64 * 64; ++i) {
        const auto* p = &r.rgba[i * 4];
        if (p[0] != cr || p[1] != cg || p[2] != cb || p[3] != 255) return false;
      }
      return true;
    };
    const struct { float d[3]; std::uint8_t c[3]; const char* name; } faces[6] = {
      {{1,0,0},  {255,0,0},     "+X"}, {{-1,0,0}, {0,255,0},     "-X"},
      {{0,1,0},  {0,0,255},     "+Y"}, {{0,-1,0}, {255,255,255}, "-Y"},
      {{0,0,1},  {255,255,0},   "+Z"}, {{0,0,-1}, {255,0,255},   "-Z"},
    };
    { TexScene s; s.build_cube(1, 0, 0);
      auto r = s.run(vk);
      if (!r.ok) std::printf("  cube draw error: %s\n", r.error.c_str());
      check(r.ok && r.notes.find("cubemap") != std::string::npos,
            "cube: PPS2 v2 draw ok, result states the cubemap stage (S28)"); }
    for (int k = 0; k < 6; ++k) {
      TexScene s; s.build_cube(faces[k].d[0], faces[k].d[1], faces[k].d[2]);
      auto r = s.run(vk);
      check(r.ok && solid(r, faces[k].c[0], faces[k].c[1], faces[k].c[2]),
            std::string("cube: face ") + faces[k].name + " draws its solid colour byte-exact (S28)");
    }
    { TexScene s; s.build_cube(1, 0, 0, 1, 1, 256);  // point mips, T# min_lod 1.0
      auto r = s.run(vk);
      check(r.ok && solid(r, 127, 0, 0), "cube: mip level 1 face +X draws half-red byte-exact (S28)"); }
    // Refusals.
    { TexScene s; s.build_cube(1, 0, 0, 0, 0, 0, 0, 0);  // v0 blob, cube T#
      auto r = s.run(vk);
      check(!r.ok && r.error.find("does not match the T# image type") != std::string::npos,
            "cube: v0 blob with a cube T# is refused (S28)"); }
    { TexScene s; s.build_cube(1, 0, 0, 0, 0, 0, 0, 1);  // v1 blob, cube T#
      auto r = s.run(vk);
      check(!r.ok && r.error.find("does not match the T# image type") != std::string::npos,
            "cube: v1 blob with a cube T# is refused (S28)"); }
    { TexScene s; s.build_cube(1, 0, 0);  // v2 blob, swap in a Color2D T#
      std::uint32_t tw[8]; s.make_tdesc(tw, 0x700000, 8, 8, 8, 0);
      s.tsharp.assign(tw, tw + 8);
      s.mem.unmap(0x710000);
      s.mem.map_external(0x710000, s.tsharp.data(), s.tsharp.size() * 4, "tsharp2d");
      auto r = s.run(vk);
      check(!r.ok && r.error.find("does not match the T# image type") != std::string::npos,
            "cube: v2 blob with a 2D T# is refused (S28)"); }
    { TexScene s; s.build_cube(1, 0, 0, 0, 0, 0, 9);  // tiled cube T#
      auto r = s.run(vk);
      check(!r.ok && r.error.find("cubemap") != std::string::npos,
            "cube: tiled cubemap is refused (S28)"); }
  }

  // Slice S18 (moved with TexScene in the S31 file split): the textured
  // full-screen quad winds screen-CW, so cull-front keeps it and cull-back
  // removes it.
  {
    auto lit2 = [](const DrawResult& r, std::uint32_t x, std::uint32_t y) {
      const auto* p = &r.rgba[(y * 64 + x) * 4];
      return p[0] > 16 || p[1] > 16 || p[2] > 16;   // clear is black
    };
    { TexScene s; s.mode_cntl_set = true; s.mode_cntl = 1; s.build(1.0f, 0, 0, 0);
      auto r = s.run(vk);
      check(r.ok && lit2(r, 32, 32), "cull: textured quad survives cull-front"); }
    { TexScene s; s.mode_cntl_set = true; s.mode_cntl = 2; s.build(1.0f, 0, 0, 0);
      auto r = s.run(vk);
      check(r.ok && !lit2(r, 32, 32), "cull: textured quad is removed by cull-back"); }
  }

  // ---- slice S31: array allocation vs sampled view window ----
  {
    // An 8-slice allocation whose view window is the first 4 slices
    // (last_array = 3, DEPTH field = 7): the repack must read each level's
    // view slices out of the wider allocation stride.
    auto solid = [&](const DrawResult& r, std::uint8_t cr, std::uint8_t cg, std::uint8_t cb) {
      for (int i = 0; i < 64 * 64; ++i) {
        const auto* p = &r.rgba[i * 4];
        if (p[0] != cr || p[1] != cg || p[2] != cb || p[3] != 255) return false;
      }
      return true;
    };
    { TexScene s; s.build_array(4, 2.0f, 10, 0, 0, 0, 0, true, 8);
      auto r = s.run(vk);
      if (!r.ok) std::printf("  array view error: %s\n", r.error.c_str());
      check(r.ok && solid(r, 0, 0, 255),
            "array-view: slice 2 of a 4-slice view inside an 8-slice allocation draws solid blue byte-exact (S31)"); }
    { TexScene s; s.build_array(4, 0.0f, 10, 0, 0, 0, 0, true, 8);
      auto r = s.run(vk);
      check(r.ok && solid(r, 255, 0, 0),
            "array-view: slice 0 of the windowed allocation draws solid red byte-exact (S31)"); }
    // Mip through a windowed allocation: level 1 starts after ALL 8 level-0
    // slices; slice 1 of level 1 is cols[(1 + 2) % 4] = white.
    { TexScene s; s.build_array(4, 1.0f, 10, 1, 1, 256, 0, true, 8);
      auto r = s.run(vk);
      check(r.ok && solid(r, 255, 255, 255),
            "array-view: mip level 1 slice 1 reads past the full 8-slice level 0 (S31)"); }
    // A cube with more than one cube in the DEPTH field is refused.
    { TexScene s; s.build_cube(1.0f, 0.0f, 0.0f);
      std::uint32_t tw[8]; s.make_tdesc(tw, 0x700000, 8, 8, 8, 0, 0, 0, 10, 0, 11, 5, 1);  // 2 cubes
      s.tsharp.assign(tw, tw + 8);
      s.mem.unmap(0x710000);
      s.mem.map_external(0x710000, s.tsharp.data(), s.tsharp.size() * 4, "tsharp2cube");
      auto r = s.run(vk);
      check(!r.ok && r.error.find("cube") != std::string::npos,
            "array-view: a two-cube T# is refused (cube arrays not implemented) (S31)"); }
  }

  // ---- slice S22: thin-tiled (swizzled) textures, end to end ----
  {
    auto px = [&](const std::vector<std::uint8_t>& t, int x, int y) { return &t[(y * 64 + x) * 4]; };
    // Standard family (SW_64KB_S): checkerboard survives the detile byte-exact.
    { TexScene s; s.build_tiled(9, 10, 4, 0, 1.0f, 0);
      auto r = s.run(vk);
      check(r.ok && px(r.rgba, 8, 8)[0] == 255 && px(r.rgba, 24, 8)[0] == 0 &&
            px(r.rgba, 24, 24)[0] == 255 && px(r.rgba, 40, 40)[0] == 255,
            "tiled: SW_64KB_S checkerboard draws byte-exact"); }
    // XOR equation family (SW_64KB_S_X, mode 25).
    { TexScene s; s.build_tiled(25, 10, 4, 0, 1.0f, 0);
      auto r = s.run(vk);
      check(r.ok && px(r.rgba, 8, 8)[0] == 255 && px(r.rgba, 24, 8)[0] == 0 &&
            px(r.rgba, 24, 24)[0] == 255 && px(r.rgba, 40, 40)[0] == 255,
            "tiled: SW_64KB_S_X checkerboard draws byte-exact"); }
    // Display equation mode (SW_64KB_D, mode 10) with a BC1 solid.
    { TexScene s; s.build_tiled(10, 35, 8, 0, 1.0f, 0);
      auto r = s.run(vk);
      check(r.ok && px(r.rgba, 0, 0)[0] == 255 && px(r.rgba, 0, 0)[1] == 0 &&
            px(r.rgba, 63, 63)[0] == 255 && px(r.rgba, 63, 63)[3] == 255,
            "tiled: SW_64KB_D BC1 solid draws red byte-exact"); }
    // Tail-packed chain (all three levels share one 64KB block): the mip
    // filter still picks the right level.
    { TexScene s; s.build_tiled(9, 10, 4, 2, 32.0f, 1);   // point, lod 1
      auto r = s.run(vk);
      check(r.ok && px(r.rgba, 32, 32)[1] == 255 && px(r.rgba, 32, 32)[0] == 0,
            "tiled: point mip filter picks tail-packed level 1 exactly"); }
    { TexScene s; s.build_tiled(9, 10, 4, 2, 64.0f, 1);   // point, lod 2
      auto r = s.run(vk);
      check(r.ok && px(r.rgba, 32, 32)[2] == 255 && px(r.rgba, 32, 32)[0] == 0,
            "tiled: point mip filter picks tail-packed level 2 exactly"); }
  }

  // ---- slice S23: extended plain formats + sRGB ----
  {
    auto px = [](const DrawResult& r, int x, int y) { return &r.rgba[(y * 64 + x) * 4]; };
    // R8G8 unorm checker: red and green channels carry the checker, blue pads 0.
    { TexScene s; std::vector<std::uint8_t> b(4 * 4 * 2);
      for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
        const std::uint8_t v = ((x + y) & 1) ? 0 : 255;
        b[(y * 4 + x) * 2] = v; b[(y * 4 + x) * 2 + 1] = v;
      }
      s.build_plain(3, 0, 4, b);
      auto r = s.run(vk);
      check(r.ok && px(r, 8, 8)[0] == 255 && px(r, 8, 8)[1] == 255 && px(r, 8, 8)[2] == 0 &&
            px(r, 24, 8)[0] == 0 && px(r, 24, 24)[0] == 255,
            "plain: 8_8 unorm checker draws byte-exact"); }
    // R16G16B16A16 unorm solid: 0xFFFF, 0x8080, 0, 0xFFFF -> (255,128,0,255).
    { TexScene s; std::vector<std::uint8_t> b(16 * 16 * 8);
      for (int i = 0; i < 16 * 16; ++i) {
        b[i*8+0]=0xFF; b[i*8+1]=0xFF; b[i*8+2]=0x80; b[i*8+3]=0x80;
        b[i*8+4]=0; b[i*8+5]=0; b[i*8+6]=0xFF; b[i*8+7]=0xFF;
      }
      s.build_plain(12, 0, 16, b);
      auto r = s.run(vk);
      check(r.ok && px(r, 0, 0)[0] == 255 && px(r, 0, 0)[1] == 128 && px(r, 0, 0)[2] == 0 &&
            px(r, 63, 63)[0] == 255 && px(r, 63, 63)[3] == 255,
            "plain: 16_16_16_16 unorm solid draws byte-exact"); }
    // R16 half float solid: 0.5 -> 128.
    { TexScene s; std::vector<std::uint8_t> b(16 * 16 * 2);
      for (int i = 0; i < 16 * 16; ++i) { b[i*2] = 0x00; b[i*2+1] = 0x38; }
      s.build_plain(2, 7, 16, b);
      auto r = s.run(vk);
      check(r.ok && px(r, 32, 32)[0] == 128 && px(r, 32, 32)[1] == 0 && px(r, 32, 32)[3] == 255,
            "plain: 16 half float solid draws byte-exact"); }
    // R32G32B32A32 float solid: 1.0, 0.5, 0.25, 1.0 -> (255,128,64,255).
    { TexScene s; std::vector<std::uint8_t> b(8 * 8 * 16);
      const std::uint32_t f[4] = {0x3F800000, 0x3F000000, 0x3E800000, 0x3F800000};
      for (int i = 0; i < 8 * 8; ++i) std::memcpy(&b[i * 16], f, 16);
      s.build_plain(14, 7, 8, b);
      auto r = s.run(vk);
      check(r.ok && px(r, 32, 32)[0] == 255 && px(r, 32, 32)[1] == 128 && px(r, 32, 32)[2] == 64 &&
            px(r, 32, 32)[3] == 255, "plain: 32_32_32_32 float solid draws byte-exact"); }
    // 5_6_5 unorm solid red.
    { TexScene s; std::vector<std::uint8_t> b(16 * 16 * 2);
      for (int i = 0; i < 16 * 16; ++i) { b[i*2] = 0x00; b[i*2+1] = 0xF8; }
      s.build_plain(16, 0, 16, b);
      auto r = s.run(vk);
      check(r.ok && px(r, 32, 32)[0] == 255 && px(r, 32, 32)[1] == 0 && px(r, 32, 32)[3] == 255,
            "plain: 5_6_5 solid red draws byte-exact"); }
    // 8_8_8_8 sRGB solid (55,188,255): the host sampler converts to linear;
    // the spec formula gives 10, 128, 255.
    { TexScene s; std::vector<std::uint8_t> b(16 * 16 * 4);
      for (int i = 0; i < 16 * 16; ++i) { b[i*4] = 55; b[i*4+1] = 188; b[i*4+2] = 255; b[i*4+3] = 255; }
      s.build_plain(10, 9, 16, b);
      auto r = s.run(vk);
      check(r.ok && px(r, 32, 32)[0] == 10 && px(r, 32, 32)[1] == 128 && px(r, 32, 32)[2] == 255,
            "plain: 8_8_8_8 sRGB converts through the host sampler byte-exact"); }
    // BC1 sRGB solid: a 57-red block; sRGB(57) -> 10 through the host sampler.
    { TexScene s; std::vector<std::uint8_t> b(8);
      s.bc_solid(b.data(), 35, 57, 0, 0, 255);
      s.build_plain(35, 9, 4, b);
      auto r = s.run(vk);
      check(r.ok && px(r, 32, 32)[0] == 10 && px(r, 32, 32)[1] == 0 && px(r, 32, 32)[3] == 255,
            "bc: BC1 sRGB solid converts through the host sampler byte-exact"); }
    // BC7 sRGB solid: the same mode-6 block; sRGB(201,101,51) -> (149,33,4).
    { TexScene s; std::vector<std::uint8_t> b(16);
      int pos = 0;
      auto put = [&](std::uint64_t v, int n) { for (int i = 0; i < n; ++i, ++pos) b[pos / 8] |= std::uint8_t(((v >> i) & 1) << (pos % 8)); };
      put(0x40, 7);
      const int raw[4] = {100, 50, 25, 127};
      for (int c = 0; c < 4; ++c) { put(raw[c], 7); put(raw[c], 7); }
      put(1, 1); put(1, 1);
      for (int i = 0; i < 16; ++i) put(0, i == 0 ? 3 : 4);
      s.build_plain(41, 9, 4, b);
      auto r = s.run(vk);
      check(r.ok && px(r, 32, 32)[0] == 149 && px(r, 32, 32)[1] == 33 && px(r, 32, 32)[2] == 8,
            "bc: BC7 sRGB solid converts through the host sampler byte-exact"); }
    // Tiled R16G16 unorm checker (SW_64KB_S, mode 9).
    { TexScene s; s.build_tiled(9, 5, 4, 0, 1.0f, 0);
      auto r = s.run(vk);
      check(r.ok && px(r, 8, 8)[0] == 255 && px(r, 8, 8)[1] == 255 &&
            px(r, 24, 8)[0] == 0 && px(r, 24, 24)[0] == 255,
            "tiled: SW_64KB_S 16_16 unorm checker draws byte-exact"); }
  }

  // ---- slice S19: mipmapped textures (mip chains, mip filters, lod clamps) ----
  // Solid-colour levels (L0 red, L1 green, L2 blue) make sampled colours
  // exact under both point and linear filtering. uv scale s over the 64x64
  // target gives a texel footprint of 4*s/64 per pixel: s=32 -> lod 1,
  // s=64 -> lod 2, s=48 -> lod ~1.585, s=1 -> magnification (lod 0).
  {
    auto centre = [](const DrawResult& r) -> const std::uint8_t* {
      return &r.rgba[(32 * 64 + 32) * 4];
    };
    auto is = [](const std::uint8_t* p, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
      return p[0] == r && p[1] == g && p[2] == b;
    };
    { TexScene s; s.build_mips(32.0f, 1);   // point, lod 1
      auto r = s.run(vk);
      check(r.ok && is(centre(r), 0, 255, 0), "mips: point mip filter picks level 1 exactly"); }
    { TexScene s; s.build_mips(64.0f, 1);   // point, lod 2
      auto r = s.run(vk);
      check(r.ok && is(centre(r), 0, 0, 255), "mips: point mip filter picks level 2 exactly"); }
    { TexScene s; s.build_mips(1.0f, 1);    // magnification, lod 0
      auto r = s.run(vk);
      check(r.ok && is(centre(r), 255, 0, 0), "mips: magnification samples level 0"); }
    { TexScene s; s.build_mips(32.0f, 2);   // linear, lod exactly 1
      auto r = s.run(vk);
      check(r.ok && is(centre(r), 0, 255, 0), "mips: linear mip filter is exact at an integer lod"); }
    { TexScene s; s.build_mips(48.0f, 2);   // linear, lod ~1.585: green/blue mix
      auto r = s.run(vk);
      const std::uint8_t* p = r.ok ? centre(r) : nullptr;
      check(r.ok && p[0] < 16 && p[2] > p[1] && p[2] > 120 && p[2] < 180 && p[1] > 70 && p[1] < 135,
            "mips: linear mip filter blends levels 1 and 2 inside the tolerance band"); }
    { TexScene s; s.build_mips(64.0f, 1, 0, 256);   // S# max_lod 1.0 clamps lod 2 -> 1
      auto r = s.run(vk);
      check(r.ok && is(centre(r), 0, 255, 0), "mips: S# max_lod clamps the sampled level"); }
    { TexScene s; s.build_mips(1.0f, 1, 256);       // S# min_lod 1.0 clamps lod 0 -> 1
      auto r = s.run(vk);
      check(r.ok && is(centre(r), 0, 255, 0), "mips: S# min_lod clamps the sampled level"); }
    { TexScene s; s.build_mips(32.0f, 1, 0, 0, 512);  // T# min_lod 2.0 clamps lod 1 -> 2
      auto r = s.run(vk);
      check(r.ok && is(centre(r), 0, 0, 255), "mips: T# min_lod folds into the lod clamp"); }
    { TexScene s; s.build_mips(64.0f, 0);   // mip_filter none: only level 0
      auto r = s.run(vk);
      check(r.ok && is(centre(r), 255, 0, 0), "mips: mip filter none keeps level 0 on a chain"); }
    { TexScene s; s.build_mips(32.0f, 1);   // point mip filter on a single-level texture
      std::uint32_t tw[8]; s.make_tdesc(tw, 0x700000, 4, 4, 4, 0, 0);
      s.mem.unmap(0x710000);
      s.tsharp.assign(tw, tw + 8);
      s.mem.map_external(0x710000, s.tsharp.data(), s.tsharp.size() * 4, "tsharp1");
      auto r = s.run(vk);
      check(!r.ok && r.error.find("single-level") != std::string::npos,
            "mips: mip filtering on a single-level texture is refused"); }
  }

  // ---- slice S20: compressed textures (BC1/BC3 decoded to RGBA8) ----
  {
    auto centre = [](const DrawResult& r) -> const std::uint8_t* {
      return &r.rgba[(32 * 64 + 32) * 4];
    };
    { TexScene s; s.build_bc(35, 1.0f, 4, 0, 0);   // BC1 solid red
      auto r = s.run(vk);
      const std::uint8_t* p = r.ok ? centre(r) : nullptr;
      check(r.ok && p[0] == 255 && p[1] == 0 && p[2] == 0, "bc: BC1 solid draw samples the decoded red"); }
    { TexScene s; s.build_bc(35, 1.0f, 4, 0, 0, 0, 0, 0);   // BC1 red/black checker block, point
      s.texels[0] = 0x00; s.texels[1] = 0xF8; s.texels[2] = 0x00; s.texels[3] = 0x00;
      s.texels[4] = 0x44; s.texels[5] = 0x11; s.texels[6] = 0x44; s.texels[7] = 0x11;
      auto r = s.run(vk);
      auto px = [&](int x, int y) { return &r.rgba[(y * 64 + x) * 4]; };
      check(r.ok && px(8, 8)[0] == 255 && px(24, 8)[0] == 0 && px(24, 24)[0] == 255 && px(40, 40)[0] == 255,
            "bc: BC1 checker block draws in exact 16-pixel blocks"); }
    { TexScene s; s.build_bc(37, 1.0f, 4, 0, 0, 0, 0, 0);   // BC3 white + alpha gradient, point
      std::uint64_t abits = 0;
      for (int i = 0; i < 8; ++i) abits |= std::uint64_t(i) << (3 * i);
      s.texels[0] = 255; s.texels[1] = 0;
      for (int i = 0; i < 6; ++i) s.texels[2 + i] = std::uint8_t(abits >> (8 * i));
      s.texels[8] = 0xFF; s.texels[9] = 0xFF; s.texels[10] = 0xFF; s.texels[11] = 0xFF;  // white c0 = c1
      auto r = s.run(vk);
      auto al = [&](int x, int y) { return r.rgba[(y * 64 + x) * 4 + 3]; };
      // Row order may be direct or flipped by the clip-y chain (as in S17).
      const bool direct = r.ok && al(24, 8) == 0;
      const int y0 = direct ? 8 : 55, y1 = direct ? 24 : 39;
      check(r.ok && al(8, y0) == 255 && al(24, y0) == 0 && al(40, y0) == 218 && al(8, y1) == 145,
            "bc: BC3 alpha gradient reaches the target byte-exact");
      const std::uint8_t* p = r.ok ? centre(r) : nullptr;
      check(r.ok && p[0] == 255 && p[1] == 255 && p[2] == 255, "bc: BC3 colour half decodes white"); }
    { TexScene s; s.build_bc(35, 32.0f, 8, 2, 1);  // BC1 chain, point, footprint 4 -> lod 2
      auto r = s.run(vk);
      const std::uint8_t* p = r.ok ? centre(r) : nullptr;
      check(r.ok && p[0] == 0 && p[1] == 0 && p[2] == 255, "bc: BC1 mip chain selects level 2"); }
    { TexScene s; s.build_bc(37, 16.0f, 8, 2, 1);  // BC3 chain, point, footprint 2 -> lod 1
      auto r = s.run(vk);
      const std::uint8_t* p = r.ok ? centre(r) : nullptr;
      check(r.ok && p[0] == 0 && p[1] == 255 && p[2] == 0 && p[3] == 255,
            "bc: BC3 mip chain selects level 1 with full alpha"); }
    { TexScene s; s.build_bc(35, 1.0f, 4, 0, 0, 8);   // pitch 8 != width 4
      auto r = s.run(vk);
      check(!r.ok && r.error.find("tightly pitched") != std::string::npos,
            "bc: compressed textures with a loose pitch are refused"); }
    { TexScene s; s.build_bc(35, 1.0f, 6, 0, 0);      // 6x6 extent
      auto r = s.run(vk);
      check(!r.ok && r.error.find("multiples of 4") != std::string::npos,
            "bc: compressed extents not divisible by 4 are refused"); }
    // Slice S21: BC2/BC4/BC5 end to end.
    { TexScene s; s.build_bc(36, 1.0f, 4, 0, 0, 0, 0, 0);   // BC2: white + nibble alpha ramp, point
      for (int i = 0; i < 8; ++i) s.texels[i] = std::uint8_t((2 * i) | ((2 * i + 1) << 4));
      s.texels[8] = 0xFF; s.texels[9] = 0xFF; s.texels[10] = 0xFF; s.texels[11] = 0xFF;  // white c0 = c1
      auto r = s.run(vk);
      auto al = [&](int x, int y) { return r.rgba[(y * 64 + x) * 4 + 3]; };
      const bool direct = r.ok && al(8, 8) == 0;
      const int y0 = direct ? 8 : 55, y1 = direct ? 40 : 23;  // texel rows 0 and 2 either way
      // texel (0,0) alpha 0, (1,0) 17, (2,0) 34; row 2 -> texels 8..11: 136, 153, ...
      check(r.ok && al(8, y0) == 0 && al(24, y0) == 17 && al(40, y0) == 34 &&
            al(8, y1) == 136 && al(24, y1) == 153,
            "bc: BC2 explicit alphas reach the target byte-exact"); }
    { TexScene s; s.build_bc(38, 1.0f, 4, 0, 0, 0, 0, 0);   // BC4 solid red 218, point
      s.texels[0] = 218; s.texels[1] = 218;
      auto r = s.run(vk);
      const std::uint8_t* p = r.ok ? &r.rgba[(32 * 64 + 32) * 4] : nullptr;
      check(r.ok && p[0] == 218 && p[1] == 0 && p[2] == 0 && p[3] == 255,
            "bc: BC4 decodes into the red channel only"); }
    { TexScene s; s.build_bc(39, 1.0f, 4, 0, 0, 0, 0, 0);   // BC5 solid (80, 160), point
      s.texels[0] = 80; s.texels[1] = 80; s.texels[8] = 160; s.texels[9] = 160;
      auto r = s.run(vk);
      const std::uint8_t* p = r.ok ? &r.rgba[(32 * 64 + 32) * 4] : nullptr;
      check(r.ok && p[0] == 80 && p[1] == 160 && p[2] == 0 && p[3] == 255,
            "bc: BC5 decodes into red and green channels"); }
    // Slice S24: BC7 end to end. Hand-built mode-6 solid block: raw 7-bit
    // endpoints (100, 50, 25, 127) + P-bit 1 decode to (201, 101, 51, 255).
    { TexScene s; s.build_bc(41, 1.0f, 4, 0, 0, 0, 0, 0);   // BC7 solid, point
      std::uint8_t blk[16] = {}; int pos = 0;
      auto put = [&](std::uint64_t v, int n) { for (int i = 0; i < n; ++i, ++pos) blk[pos / 8] |= std::uint8_t(((v >> i) & 1) << (pos % 8)); };
      put(0x40, 7);                        // mode 6
      const int raw[4] = {100, 50, 25, 127};
      for (int c = 0; c < 4; ++c) { put(raw[c], 7); put(raw[c], 7); }  // both endpoints, channel-major
      put(1, 1); put(1, 1);                // P-bits
      for (int i = 0; i < 16; ++i) put(0, i == 0 ? 3 : 4);  // weights 0 (anchor 3 bits)
      for (int i = 0; i < 16; ++i) s.texels[i] = blk[i];
      auto r = s.run(vk);
      const std::uint8_t* p = r.ok ? centre(r) : nullptr;
      check(r.ok && p[0] == 201 && p[1] == 101 && p[2] == 51 && p[3] == 255,
            "bc: BC7 mode 6 solid draw decodes byte-exact"); }
    { TexScene s; s.build_bc(41, 16.0f, 8, 2, 1);  // BC7 chain, point, footprint 2 -> lod 1
      auto r = s.run(vk);
      const std::uint8_t* p = r.ok ? centre(r) : nullptr;
      check(r.ok && p[0] == 0 && p[1] == 254 && p[2] == 0,
            "bc: BC7 mip chain selects level 1"); }  // 255 quantizes to 254 in mode 6 (P-bit 0)
    // Slice S25: BC6H end to end (mode 11 solid blocks, nf Uint = 4).
    { TexScene s; s.build_bc(40, 1.0f, 4, 0, 0, 0, 0, 0, 4);   // BC6H solid red, point
      auto r = s.run(vk);
      const std::uint8_t* p = r.ok ? centre(r) : nullptr;
      check(r.ok && p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] == 255,
            "bc: BC6H solid draw decodes byte-exact"); }
    { TexScene s; s.build_bc(40, 16.0f, 8, 2, 1, 0, 1, 1, 4);  // BC6H chain, footprint 2 -> lod 1
      auto r = s.run(vk);
      const std::uint8_t* p = r.ok ? centre(r) : nullptr;
      check(r.ok && p[0] == 0 && p[1] == 255 && p[2] == 0,
            "bc: BC6H mip chain selects level 1"); }
    { TexScene s; s.build_bc(40, 1.0f, 4, 0, 0, 0, 0, 0, 5);   // BC6H signed (nf Sint = 5): 10-bit 1023 = -1 -> black
      auto r = s.run(vk);
      const std::uint8_t* p = r.ok ? centre(r) : nullptr;
      check(r.ok && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 255,
            "bc: BC6H signed draw clamps negative endpoints to black"); }
  }

}
