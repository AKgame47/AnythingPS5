// Slice S30: 3D (volume) texture draw tests, end to end through the real
// PM4 -> execute_draw path with a guest Color3D T#/S#. Kept in its own file
// because gfx_draw_tests.cpp outgrew the web editor's practical size limit.
// Synthetic GNM-style descriptors only; no real game data. GPL-2.0.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "graphics/gfx_draw.hpp"
#include "graphics/gfx_tsharp.hpp"
#include "graphics/gpu_va.hpp"
#include "graphics/pm4.hpp"
#include "graphics/rdna_pixel.hpp"

namespace {
using namespace pnr::graphics;
std::uint32_t f2u(float f) { std::uint32_t u; std::memcpy(&u, &f, 4); return u; }

struct Tex3dScene {
  GpuAddressSpace mem;
  std::vector<std::uint32_t> vs_blob, ps_blob, mat, cmd, tsharp, ssharp;
  std::vector<float> vb;
  std::vector<std::uint8_t> target, texels;
  static constexpr std::uint32_t W = 64, H = 64;

  void make_tdesc(std::uint32_t w[8], std::uint64_t base, std::uint32_t tw,
                  std::uint32_t th, std::uint32_t pitch, std::uint32_t tiling,
                  std::uint32_t last_level, std::uint32_t min_lod,
                  std::uint32_t depth_m1) {
    std::uint64_t q0 = (base >> 8) & 0x3FFFFFFFFFull;
    q0 |= std::uint64_t(min_lod & 0xFFF) << 40;
    q0 |= std::uint64_t(10) << 52;  // data format 8_8_8_8
    // number format 0 (unorm)
    std::uint64_t q1 = std::uint64_t(tw - 1) | (std::uint64_t(th - 1) << 14);
    q1 |= std::uint64_t(1) << 35 | std::uint64_t(2) << 38 | std::uint64_t(3) << 41;
    q1 |= std::uint64_t(last_level & 0xF) << 48;
    q1 |= std::uint64_t(tiling & 0x1F) << 52;
    q1 |= std::uint64_t(10) << 60;  // type 10 = Color3D
    std::uint64_t q2 = std::uint64_t(pitch - 1) << 13;
    q2 |= std::uint64_t(depth_m1 & 0x1FFF);  // depth-1
    w[0] = std::uint32_t(q0); w[1] = std::uint32_t(q0 >> 32);
    w[2] = std::uint32_t(q1); w[3] = std::uint32_t(q1 >> 32);
    w[4] = std::uint32_t(q2); w[5] = std::uint32_t(q2 >> 32);
    w[6] = 0; w[7] = 0;
  }

  // 8x8xndepth volume, solid z-planes red/green/blue/white cycling per level
  // (+2 per mip level), level-major; one fullscreen quad with constant w.
  void build_3d(std::uint32_t ndepth, float wcoord, std::uint32_t last_level = 0,
                std::uint32_t t_min_lod = 0, std::uint32_t tiling = 0,
                std::uint32_t blob_v = 3) {
    vs_blob = make_pnr_vs3_blob(true);
    ps_blob = blob_v == 3 ? make_pnr_ps2_3d_blob() : blob_v == 2 ? make_pnr_ps2_cube_blob()
              : blob_v == 1 ? make_pnr_ps2_array_blob() : make_pnr_ps2_blob();
    mat.assign(16, 0); for (int i = 0; i < 4; ++i) mat[i * 5] = f2u(1.0f);
    const float px[6][2] = {{-1,-1},{1,-1},{1,1},{-1,-1},{1,1},{-1,1}};
    const float pu[6][2] = {{0,0},{1,0},{1,1},{0,0},{1,1},{0,1}};
    for (int v = 0; v < 6; ++v) {
      vb.push_back(px[v][0]); vb.push_back(px[v][1]); vb.push_back(0.0f);
      vb.push_back(1.0f); vb.push_back(1.0f); vb.push_back(1.0f);  // white tint
      vb.push_back(pu[v][0]); vb.push_back(pu[v][1]);
      vb.push_back(wcoord);
    }
    const std::uint32_t side = 8;
    const std::uint8_t cols[4][3] = {{255,0,0},{0,255,0},{0,0,255},{255,255,255}};
    for (std::uint32_t l = 0; l <= last_level; ++l) {
      const std::uint32_t lw = side >> l ? side >> l : 1;
      const std::uint32_t ld = ndepth >> l ? ndepth >> l : 1;
      for (std::uint32_t s = 0; s < ld; ++s) {
        const auto& c = cols[(s + 2 * l) % 4];
        const size_t at = texels.size();
        texels.resize(at + std::size_t(lw) * lw * 4);
        for (std::uint32_t i = 0; i < lw * lw; ++i) {
          auto* p = &texels[at + i * 4];
          p[0] = c[0]; p[1] = c[1]; p[2] = c[2]; p[3] = 255;
        }
      }
    }
    std::uint32_t tw[8];
    make_tdesc(tw, 0x700000, side, side, side, tiling, last_level, t_min_lod, ndepth - 1);
    tsharp.assign(tw, tw + 8);
    std::uint64_t sq0 = std::uint64_t(2) << 3;  // wrap x, clamp y; point filters
    ssharp = {std::uint32_t(sq0), std::uint32_t(sq0 >> 32), 0, 0};
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
    b.set_reg(pm4::SET_CONTEXT_REG, amd::PA_CL_VPORT_XSCALE,
              {f2u(W / 2.0f), f2u(W / 2.0f), f2u(-0.5f * float(H)), f2u(0.5f * float(H)), f2u(0.5f), f2u(0.5f)});
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

void pnr_gfx_tex3d_tests(int& pass, int& fail) {
  auto check = [&](bool ok, const std::string& l) { if (ok) ++pass; else ++fail; std::printf("%s %s\n", ok ? "PASS" : "FAIL", l.c_str()); };
  VulkanBackend vk;
  if (!vk.open() || !vk.create_instance("pnr-tests") || vk.devices().empty() ||
      !vk.create_device(0)) {
    std::printf("SKIP gfx tex3d tests (no Vulkan device)\n");
    return;
  }
  auto solid = [&](const DrawResult& r, std::uint8_t cr, std::uint8_t cg, std::uint8_t cb) {
    for (int i = 0; i < 64 * 64; ++i) {
      const auto* p = &r.rgba[i * 4];
      if (p[0] != cr || p[1] != cg || p[2] != cb || p[3] != 255) return false;
    }
    return true;
  };
  // 8x8x4 volume: z-plane colours red, green, blue, white; point sampling
  // with w at a plane centre must return that plane's colour exactly.
  { Tex3dScene s; s.build_3d(4, 0.125f);
    auto r = s.run(vk);
    if (!r.ok) std::printf("  3D draw error: %s\n", r.error.c_str());
    check(r.ok && r.notes.find("3D texture") != std::string::npos,
          "3D: PPS2 v3 draw ok, result states the 3D texture stage (S30)");
    check(r.ok && solid(r, 255, 0, 0), "3D: z-plane 0 draws solid red byte-exact (S30)"); }
  { Tex3dScene s; s.build_3d(4, 0.375f);
    auto r = s.run(vk);
    check(r.ok && solid(r, 0, 255, 0), "3D: z-plane 1 draws solid green byte-exact (S30)"); }
  { Tex3dScene s; s.build_3d(4, 0.625f);
    auto r = s.run(vk);
    check(r.ok && solid(r, 0, 0, 255), "3D: z-plane 2 draws solid blue byte-exact (S30)"); }
  { Tex3dScene s; s.build_3d(4, 0.875f);
    auto r = s.run(vk);
    check(r.ok && solid(r, 255, 255, 255), "3D: z-plane 3 draws solid white byte-exact (S30)"); }
  // Mips: level 1 of a depth-4 volume is 4x4x2 (blue, white).
  { Tex3dScene s; s.build_3d(4, 0.25f, 1, 256);
    auto r = s.run(vk);
    check(r.ok && solid(r, 0, 0, 255), "3D: mip level 1 z-plane 0 draws solid blue byte-exact (S30)"); }
  { Tex3dScene s; s.build_3d(4, 0.75f, 1, 256);
    auto r = s.run(vk);
    check(r.ok && solid(r, 255, 255, 255), "3D: mip level 1 z-plane 1 draws solid white byte-exact (S30)"); }
  // Refusals.
  { Tex3dScene s; s.build_3d(4, 0.125f, 0, 0, 0, 0);  // v0 blob, 3D T#
    auto r = s.run(vk);
    check(!r.ok && r.error.find("does not match the T# image type") != std::string::npos,
          "3D: v0 blob with a 3D T# is refused (S30)"); }
  { Tex3dScene s; s.build_3d(4, 0.125f);  // v3 blob, swap in a Color2D T#
    std::uint32_t tw[8];
    s.make_tdesc(tw, 0x700000, 8, 8, 8, 0, 0, 0, 0);
    tw[3] = (tw[3] & 0x0FFFFFFFu) | (9u << 28);  // type 9 = Color2D
    s.tsharp.assign(tw, tw + 8);
    s.mem.unmap(0x710000);
    s.mem.map_external(0x710000, s.tsharp.data(), s.tsharp.size() * 4, "tsharp2d");
    auto r = s.run(vk);
    check(!r.ok && r.error.find("does not match the T# image type") != std::string::npos,
          "3D: v3 blob with a 2D T# is refused (S30)"); }
  { Tex3dScene s; s.build_3d(4, 0.125f, 0, 0, 9);  // tiled 3D T#
    auto r = s.run(vk);
    check(!r.ok && r.error.find("3D") != std::string::npos,
          "3D: tiled 3D texture is refused (S30)"); }
  { Tex3dScene s; s.build_3d(4, 0.125f);  // depth field 4096 -> depth 4097 > 256
    std::uint32_t tw[8];
    s.make_tdesc(tw, 0x700000, 8, 8, 8, 0, 0, 0, 4096);
    s.tsharp.assign(tw, tw + 8);
    s.mem.unmap(0x710000);
    s.mem.map_external(0x710000, s.tsharp.data(), s.tsharp.size() * 4, "tsharpdeep");
    auto r = s.run(vk);
    check(!r.ok && r.error.find("depth") != std::string::npos,
          "3D: depth beyond the 256 cap is refused (S30)"); }
}
