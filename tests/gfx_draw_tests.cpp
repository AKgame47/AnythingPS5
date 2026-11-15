// Core slices C4d/C4e tests: SYNTHETIC GNM command buffer -> one Vulkan draw -> pixel check.
// Skips (with a message) when no Vulkan device exists. No real game data is involved.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "graphics/gfx_draw.hpp"
#include "graphics/gfx_vsharp.hpp"
#include "graphics/rdna_pixel.hpp"
#include "graphics/gpu_va.hpp"
#include "graphics/pm4.hpp"

namespace {
using namespace pnr::graphics;
std::uint32_t f2u(float f) { std::uint32_t u; std::memcpy(&u, &f, 4); return u; }

struct Scene {
  GpuAddressSpace mem;
  std::vector<std::uint32_t> vs_blob, ps_blob{0xbf810000}, mat;
  std::vector<float> vb;
  std::vector<std::uint8_t> target;
  std::vector<std::uint32_t> cmd;
  static constexpr std::uint32_t W = 64, H = 64;
  void build(std::uint32_t vp_w = W, std::uint32_t prim = 4, bool with_vs_data = true) {
    vs_blob = make_pnr_vs_blob(3);
    mat.assign(16, 0); for (int i = 0; i < 4; ++i) mat[i * 5] = f2u(1.0f);  // identity
    vb = {-0.8f, -0.8f, 0, 1, 0, 0,   0.8f, -0.8f, 0, 1, 0, 0,   0, 0.8f, 0, 1, 0, 0};
    target.assign(W * H * 4, 0xCD);
    mem.map_external(0x100000, vs_blob.data(), vs_blob.size() * 4, "vs");
    mem.map_external(0x200000, ps_blob.data(), 4, "ps");
    mem.map_external(0x300000, mat.data(), 64, "matrix");
    mem.map_external(0x400000, vb.data(), vb.size() * 4, "vertices");
    mem.map_external(0x500000, target.data(), target.size(), "target");
    pm4::Builder b;
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_VS, {0x100000 >> 8, 0});
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_PS, {0x200000 >> 8, 0});
    if (with_vs_data) b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_USER_DATA_VS_0, {0x300000, 0, 0x400000, 0});
    b.set_reg(pm4::SET_CONTEXT_REG, amd::PA_CL_VPORT_XSCALE, {f2u(vp_w / 2.0f), f2u(vp_w / 2.0f), f2u(-0.5f * float(H)), f2u(0.5f * float(H)), f2u(0.5f), f2u(0.5f)});
    b.set_context(amd::CB_COLOR0_BASE, 0x500000 >> 8).set_context(amd::CB_TARGET_MASK, 0xf);
    b.set_context(amd::PA_SC_SCREEN_SCISSOR_BR, (H << 16) | W);
    b.set_uconfig(amd::VGT_PRIMITIVE_TYPE, prim).num_instances(1).draw_auto(3);
    cmd = b.dwords();
  }
};
struct IndexedScene {
  GpuAddressSpace mem;
  std::vector<std::uint32_t> vs_blob, ps_blob{0xbf810000}, mat;
  std::vector<float> vb;
  std::vector<std::uint8_t> ibuf;
  std::vector<std::uint8_t> target;
  std::vector<std::uint32_t> cmd;
  static constexpr std::uint32_t W = 64, H = 64;
  // Square from 4 vertices + 6 indices; u16=false uses a 32-bit index buffer.
  void build(bool u16, bool pvs2 = false, bool with_index_type = true) {
    vs_blob = pvs2 ? make_pnr_vs2_blob(4) : make_pnr_vs_blob(4);
    mat.assign(16, 0); for (int i = 0; i < 4; ++i) mat[i * 5] = f2u(1.0f);  // identity
    vb = {-0.8f, -0.8f, 0, 1, 0, 0,   0.8f, -0.8f, 0, 1, 0, 0,
           0.8f,  0.8f, 0, 1, 0, 0,  -0.8f,  0.8f, 0, 1, 0, 0};
    const std::uint32_t ix[6] = {0, 1, 2, 0, 2, 3};
    if (u16) {
      ibuf.resize(12);
      for (int i = 0; i < 6; ++i) { std::uint16_t v = std::uint16_t(ix[i]); std::memcpy(&ibuf[2 * i], &v, 2); }
    } else {
      ibuf.resize(24);
      for (int i = 0; i < 6; ++i) std::memcpy(&ibuf[4 * i], &ix[i], 4);
    }
    target.assign(W * H * 4, 0xCD);
    mem.map_external(0x100000, vs_blob.data(), vs_blob.size() * 4, "vs");
    mem.map_external(0x200000, ps_blob.data(), 4, "ps");
    mem.map_external(0x300000, mat.data(), 64, "matrix");
    mem.map_external(0x400000, vb.data(), vb.size() * 4, "vertices");
    mem.map_external(0x500000, target.data(), target.size(), "target");
    mem.map_external(0x600000, ibuf.data(), ibuf.size(), "indices");
    pm4::Builder b;
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_VS, {0x100000 >> 8, 0});
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_PS, {0x200000 >> 8, 0});
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_USER_DATA_VS_0, {0x300000, 0, 0x400000, 0});
    b.set_reg(pm4::SET_CONTEXT_REG, amd::PA_CL_VPORT_XSCALE, {f2u(W / 2.0f), f2u(W / 2.0f), f2u(-0.5f * float(H)), f2u(0.5f * float(H)), f2u(0.5f), f2u(0.5f)});
    b.set_context(amd::CB_COLOR0_BASE, 0x500000 >> 8).set_context(amd::CB_TARGET_MASK, 0xf);
    b.set_context(amd::PA_SC_SCREEN_SCISSOR_BR, (H << 16) | W);
    b.set_uconfig(amd::VGT_PRIMITIVE_TYPE, 4).num_instances(1);
    if (with_index_type) b.index_type(u16 ? 0u : 1u);
    b.draw_indexed(0x600000, 6, 6);
    cmd = b.dwords();
  }
  DrawResult run(VulkanBackend& vk) {
    auto sum = pm4::interpret(cmd.data(), cmd.size());
    if (sum.draws.size() != 1) { DrawResult r; r.error = "no draw parsed"; return r; }
    return execute_draw(vk, mem, sum.draws[0]);
  }
};
// Slice S10: strip/fan topology. Fixed vertex list; indexed (u32) or auto draws.
// Slice S15: PVS5 scene - vertex data fetched through a two-entry V# table
// (position + colour) instead of a raw float pointer. The same interleaved
// floats back both paths so results can be compared byte-for-byte.
struct VsharpScene {
  GpuAddressSpace mem;
  std::vector<std::uint32_t> vs_blob, ps_blob{0xbf810000}, mat, vtable;
  std::vector<float> vb;
  std::vector<std::uint8_t> target;
  std::vector<std::uint32_t> cmd;
  std::uint32_t pos_fmt = 13, col_fmt = 13;   // Format32_32_32
  std::uint32_t pos_stride = 24, col_stride = 24;  // bytes (interleaved x,y,z,r,g,b)
  std::uint64_t pos_base = 0x400000, col_base = 0x40000C;
  std::uint32_t num_records = 0;              // 0 -> vertex count
  std::uint32_t pos_w3 = 0, col_w3 = 0;       // overrides (0 -> built from fmt)
  std::uint32_t vcount = 0;                   // 0 -> floats/6 (interleaved layout)
  static constexpr std::uint32_t W = 64, H = 64;
  static std::uint32_t vsel(std::uint32_t fmt) {
    return 0x688u | (7u << 12) | (fmt << 15);  // identity dst_sel, float
  }
  void vsharp(std::uint32_t* w, std::uint64_t base, std::uint32_t stride, std::uint32_t nrec, std::uint32_t w3) {
    const std::uint64_t w01 = (base & 0xFFFFFFFFFFFull) | (std::uint64_t(stride) << 48);
    w[0] = std::uint32_t(w01); w[1] = std::uint32_t(w01 >> 32); w[2] = nrec; w[3] = w3;
  }
  void build(const std::vector<float>& verts, std::uint32_t prim) {
    const std::uint32_t nv = vcount ? vcount : std::uint32_t(verts.size() / 6);
    const std::uint32_t nrec = num_records ? num_records : nv;
    vs_blob = make_pnr_vs5_blob(pos_fmt, col_fmt,
                                pos_stride ? pos_stride / 4 : vsharp_components(pos_fmt),
                                col_stride ? col_stride / 4 : vsharp_components(col_fmt));
    vb = verts;
    mat.assign(16, 0); for (int i = 0; i < 4; ++i) mat[i * 5] = f2u(1.0f);
    target.assign(W * H * 4, 0xCD);
    vtable.resize(8);
    vsharp(&vtable[0], pos_base, pos_stride, nrec, pos_w3 ? pos_w3 : vsel(pos_fmt));
    vsharp(&vtable[4], col_base, col_stride, nrec, col_w3 ? col_w3 : vsel(col_fmt));
    mem.map_external(0x100000, vs_blob.data(), vs_blob.size() * 4, "vs");
    mem.map_external(0x200000, ps_blob.data(), 4, "ps");
    mem.map_external(0x300000, mat.data(), 64, "matrix");
    mem.map_external(0x400000, vb.data(), vb.size() * 4, "vertices");
    mem.map_external(0x500000, target.data(), target.size(), "target");
    mem.map_external(0x700000, vtable.data(), vtable.size() * 4, "vtable");
    pm4::Builder b;
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_VS, {0x100000 >> 8, 0});
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_PS, {0x200000 >> 8, 0});
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_USER_DATA_VS_0, {0x300000, 0, 0x700000, 0});
    b.set_reg(pm4::SET_CONTEXT_REG, amd::PA_CL_VPORT_XSCALE, {f2u(W / 2.0f), f2u(W / 2.0f), f2u(-0.5f * float(H)), f2u(0.5f * float(H)), f2u(0.5f), f2u(0.5f)});
    b.set_context(amd::CB_COLOR0_BASE, 0x500000 >> 8).set_context(amd::CB_TARGET_MASK, 0xf);
    b.set_context(amd::PA_SC_SCREEN_SCISSOR_BR, (H << 16) | W);
    b.set_uconfig(amd::VGT_PRIMITIVE_TYPE, prim).num_instances(1);
    b.draw_auto(nv);
    cmd = b.dwords();
  }
  DrawResult run(VulkanBackend& vk) {
    auto sum = pm4::interpret(cmd.data(), cmd.size());
    if (sum.draws.size() != 1) { DrawResult r; r.error = "no draw parsed"; return r; }
    return execute_draw(vk, mem, sum.draws[0]);
  }
};

struct TopoScene {
  GpuAddressSpace mem;
  std::vector<std::uint32_t> vs_blob, ps_blob{0xbf810000}, mat;
  std::vector<float> vb;
  std::vector<std::uint32_t> ib;
  std::vector<std::uint8_t> target;
  std::vector<std::uint32_t> cmd;
  std::uint32_t depth_control = 0;   // DB_DEPTH_CONTROL value when nonzero (slice S12)
  std::uint64_t z_base = 0;          // bound DB depth buffer address when nonzero (slice S13)
  std::uint32_t zinfo = 3;           // DB_Z_INFO value (3 = Z32 float linear)
  std::uint32_t stencil_info = 0;    // DB_STENCIL_INFO value when nonzero
  std::uint64_t z_write_base = 0;    // separate write base when nonzero
  std::uint32_t blend0 = 0;          // CB_BLEND0_CONTROL value when nonzero (slice S14)
  static constexpr std::uint32_t W = 64, H = 64;
  void build(const std::vector<float>& verts, const std::vector<std::uint32_t>& indices, std::uint32_t prim) {
    const std::uint32_t nv = std::uint32_t(verts.size() / 6);
    vs_blob = make_pnr_vs_blob(nv);
    vb = verts;
    ib = indices;
    mat.assign(16, 0); for (int i = 0; i < 4; ++i) mat[i * 5] = f2u(1.0f);
    target.assign(W * H * 4, 0xCD);
    mem.map_external(0x100000, vs_blob.data(), vs_blob.size() * 4, "vs");
    mem.map_external(0x200000, ps_blob.data(), 4, "ps");
    mem.map_external(0x300000, mat.data(), 64, "matrix");
    mem.map_external(0x400000, vb.data(), vb.size() * 4, "vertices");
    mem.map_external(0x500000, target.data(), target.size(), "target");
    pm4::Builder b;
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_VS, {0x100000 >> 8, 0});
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_PS, {0x200000 >> 8, 0});
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_USER_DATA_VS_0, {0x300000, 0, 0x400000, 0});
    b.set_reg(pm4::SET_CONTEXT_REG, amd::PA_CL_VPORT_XSCALE, {f2u(W / 2.0f), f2u(W / 2.0f), f2u(-0.5f * float(H)), f2u(0.5f * float(H)), f2u(0.5f), f2u(0.5f)});
    b.set_context(amd::CB_COLOR0_BASE, 0x500000 >> 8).set_context(amd::CB_TARGET_MASK, 0xf);
    b.set_context(amd::PA_SC_SCREEN_SCISSOR_BR, (H << 16) | W);
    if (depth_control) b.set_context(amd::DB_DEPTH_CONTROL, depth_control);
    if (z_base) {
      b.set_context(amd::DB_Z_INFO, zinfo);
      b.set_context(amd::DB_Z_READ_BASE, std::uint32_t(z_base >> 8));
      b.set_context(amd::DB_Z_WRITE_BASE, std::uint32_t((z_write_base ? z_write_base : z_base) >> 8));
      b.set_context(amd::DB_DEPTH_SIZE, ((H / 8 - 1) << 11) | (W / 8 - 1));
      if (stencil_info) b.set_context(amd::DB_STENCIL_INFO, stencil_info);
    }
    if (blend0) b.set_context(amd::CB_BLEND0_CONTROL, blend0);
    b.set_uconfig(amd::VGT_PRIMITIVE_TYPE, prim).num_instances(1);
    if (ib.empty()) {
      b.draw_auto(nv);
    } else {
      mem.map_external(0x600000, ib.data(), ib.size() * 4, "indices");
      b.index_type(1u);
      b.draw_indexed(0x600000, std::uint32_t(ib.size()), std::uint32_t(ib.size()));
    }
    cmd = b.dwords();
  }
  DrawResult run(VulkanBackend& vk) {
    auto sum = pm4::interpret(cmd.data(), cmd.size());
    if (sum.draws.size() != 1) { DrawResult r; r.error = "no draw parsed"; return r; }
    return execute_draw(vk, mem, sum.draws[0]);
  }
  // Slice S13: run into a caller-owned frame cache (shared render targets).
  DrawResult run(VulkanBackend& vk, DrawFrameCache& frame) {
    auto sum = pm4::interpret(cmd.data(), cmd.size());
    if (sum.draws.size() != 1) { DrawResult r; r.error = "no draw parsed"; return r; }
    return execute_draw(vk, mem, sum.draws[0], frame);
  }
};
// Slice S11: PVS4 Gouraud shade. Vertex layout is 9 floats (x y z r g b nx ny nz).
struct ShadeScene {
  GpuAddressSpace mem;
  std::vector<std::uint32_t> vs_blob, ps_blob{0xbf810000}, mat;
  std::vector<float> vb;
  std::vector<std::uint8_t> target;
  std::vector<std::uint32_t> cmd;
  static constexpr std::uint32_t W = 64, H = 64;
  void build(const std::vector<float>& verts9, const float lp[6], bool with_params = true) {
    const std::uint32_t nv = std::uint32_t(verts9.size() / 9);
    vs_blob = make_pnr_vs4_blob();
    vb = verts9;
    mat.assign(16, 0); for (int i = 0; i < 4; ++i) mat[i * 5] = f2u(1.0f);
    target.assign(W * H * 4, 0xCD);
    mem.map_external(0x100000, vs_blob.data(), vs_blob.size() * 4, "vs");
    mem.map_external(0x200000, ps_blob.data(), 4, "ps");
    mem.map_external(0x300000, mat.data(), 64, "matrix");
    mem.map_external(0x400000, vb.data(), vb.size() * 4, "vertices");
    mem.map_external(0x500000, target.data(), target.size(), "target");
    pm4::Builder b;
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_VS, {0x100000 >> 8, 0});
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_PS, {0x200000 >> 8, 0});
    std::vector<std::uint32_t> ud = {0x300000, 0, 0x400000, 0};
    if (with_params) for (int k = 0; k < 6; ++k) ud.push_back(f2u(lp[k]));
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_USER_DATA_VS_0, ud);
    b.set_reg(pm4::SET_CONTEXT_REG, amd::PA_CL_VPORT_XSCALE, {f2u(W / 2.0f), f2u(W / 2.0f), f2u(-0.5f * float(H)), f2u(0.5f * float(H)), f2u(0.5f), f2u(0.5f)});
    b.set_context(amd::CB_COLOR0_BASE, 0x500000 >> 8).set_context(amd::CB_TARGET_MASK, 0xf);
    b.set_context(amd::PA_SC_SCREEN_SCISSOR_BR, (H << 16) | W);
    b.set_uconfig(amd::VGT_PRIMITIVE_TYPE, 4).num_instances(1).draw_auto(nv);
    cmd = b.dwords();
  }
  DrawResult run(VulkanBackend& vk) {
    auto sum = pm4::interpret(cmd.data(), cmd.size());
    if (sum.draws.size() != 1) { DrawResult r; r.error = "no draw parsed"; return r; }
    return execute_draw(vk, mem, sum.draws[0]);
  }
};
// Slice S9: PVS3 batched draws. nverts/3 triangles, all red at the standard
// position except the LAST triangle, green and mirrored (apex down).
struct BatchScene {
  GpuAddressSpace mem;
  std::vector<std::uint32_t> vs_blob, ps_blob{0xbf810000}, mat;
  std::vector<float> vb;
  std::vector<std::uint32_t> ib;
  std::vector<std::uint8_t> target;
  std::vector<std::uint32_t> cmd;
  std::uint32_t count_override = 0;
  static constexpr std::uint32_t W = 64, H = 64;
  void build(bool post, bool indexed, std::uint32_t nverts) {
    vs_blob = make_pnr_vs3_blob(post);
    mat.assign(16, 0); for (int i = 0; i < 4; ++i) mat[i * 5] = f2u(1.0f);
    const float tri_r[9] = {-0.8f, -0.8f, 0, 0.8f, -0.8f, 0, 0, 0.8f, 0};
    const float tri_g[9] = {0.8f, 0.8f, 0, -0.8f, 0.8f, 0, 0, -0.8f, 0};
    const std::uint32_t ntri = nverts / 3;
    for (std::uint32_t t = 0; t < ntri; ++t) {
      const float* pos = (t == ntri - 1) ? tri_g : tri_r;
      const float g = (t == ntri - 1) ? 1.0f : 0.0f, rr = (t == ntri - 1) ? 0.0f : 1.0f;
      for (int v = 0; v < 3; ++v) {
        vb.push_back(pos[v * 3]); vb.push_back(pos[v * 3 + 1]); vb.push_back(pos[v * 3 + 2]);
        vb.push_back(rr); vb.push_back(g); vb.push_back(0.0f);
      }
    }
    target.assign(W * H * 4, 0xCD);
    mem.map_external(0x100000, vs_blob.data(), vs_blob.size() * 4, "vs");
    mem.map_external(0x200000, ps_blob.data(), 4, "ps");
    mem.map_external(0x300000, mat.data(), 64, "matrix");
    mem.map_external(0x400000, vb.data(), vb.size() * 4, "vertices");
    mem.map_external(0x500000, target.data(), target.size(), "target");
    const std::uint32_t dc = count_override ? count_override : nverts;
    pm4::Builder b;
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_VS, {0x100000 >> 8, 0});
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_PS, {0x200000 >> 8, 0});
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_USER_DATA_VS_0, {0x300000, 0, 0x400000, 0});
    b.set_reg(pm4::SET_CONTEXT_REG, amd::PA_CL_VPORT_XSCALE, {f2u(W / 2.0f), f2u(W / 2.0f), f2u(-0.5f * float(H)), f2u(0.5f * float(H)), f2u(0.5f), f2u(0.5f)});
    b.set_context(amd::CB_COLOR0_BASE, 0x500000 >> 8).set_context(amd::CB_TARGET_MASK, 0xf);
    b.set_context(amd::PA_SC_SCREEN_SCISSOR_BR, (H << 16) | W);
    b.set_uconfig(amd::VGT_PRIMITIVE_TYPE, 4).num_instances(1);
    if (indexed) {
      for (std::uint32_t i = 0; i < nverts; ++i) ib.push_back(i);
      mem.map_external(0x600000, ib.data(), ib.size() * 4, "indices");
      b.index_type(1u);
      b.draw_indexed(0x600000, dc, dc);
    } else {
      b.draw_auto(dc);
    }
    cmd = b.dwords();
  }
  DrawResult run(VulkanBackend& vk) {
    auto sum = pm4::interpret(cmd.data(), cmd.size());
    if (sum.draws.size() != 1) { DrawResult r; r.error = "no draw parsed"; return r; }
    return execute_draw(vk, mem, sum.draws[0]);
  }
};
}  // namespace

void pnr_gfx_draw_tests(int& pass, int& fail) {
  auto check = [&](bool ok, const std::string& l) { if (ok) ++pass; else ++fail; std::printf("%s %s\n", ok ? "PASS" : "FAIL", l.c_str()); };
  VulkanBackend vk;
  auto st = vk.open();
  if (!st) { std::printf("SKIP gfx-draw: no Vulkan loader: %s\n", st.error.message.c_str()); return; }
  st = vk.create_instance("pnr-gfx-draw-tests");
  if (!st || vk.devices().empty()) { std::printf("SKIP gfx-draw: no Vulkan device\n"); return; }
  st = vk.create_device(0);
  if (!st) { std::printf("SKIP gfx-draw: device creation failed\n"); return; }

  {
    Scene s; s.build();
    auto sum = pm4::interpret(s.cmd.data(), s.cmd.size());
    check(sum.draws.size() == 1, "draw: synthetic command buffer yields one draw");
    auto r = execute_draw(vk, s.mem, sum.draws[0]);
    check(r.ok && r.rgba.size() == 64 * 64 * 4 && r.wrote_target, "draw: ok, 64x64 RGBA copied to the colour target address");
    if (!r.ok) std::printf("  error: %s\n", r.error.c_str());
    if (r.ok) {
      auto px = [&](int x, int y) { return &r.rgba[(y * 64 + x) * 4]; };
      auto red = [&](const std::uint8_t* p) { return p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] == 255; };
      auto black = [&](const std::uint8_t* p) { return p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 255; };
      // y up in clip space maps to the top row (Vulkan clip y down is flipped by the shader chain: check both halves honestly)
      int reds = 0;
      for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) reds += red(px(x, y));
      // area = 0.5 * 1.6 * 1.6 / 4 = 32% of 4096 = 1311
      check(reds > 1250 && reds < 1370, "draw: red pixel count " + std::to_string(reds) + " is within about 4% of the triangle area (1311)");
      check(black(px(1, 1)) && black(px(62, 62)) && black(px(1, 62)) && black(px(62, 1)), "draw: all four corners stay at the clear colour");
      check(red(px(32, 32)), "draw: the centre pixel is inside the triangle and red");
      check(std::memcmp(s.target.data(), r.rgba.data(), r.rgba.size()) == 0, "draw: target memory holds exactly the rendered pixels");
      check(r.notes.find("pixel shader not translated") != std::string::npos, "draw: result states that the pixel shader is not translated");
      // The apex side: clip y=+0.8 is where the triangle narrows. Rows near one end are narrow, near the other wide.
      int top = 0, bot = 0;
      for (int x = 0; x < 64; ++x) { top += red(px(x, 10)); bot += red(px(x, 54)); }
      check(top != bot && (top < 8 || bot < 8), "draw: triangle is wide at one end and narrow at the other (row 10 vs row 54)");
    }
  }
  // Refusals, each with a plain reason and no Vulkan draw.
  auto refused = [&](Scene& s, const std::string& frag, const std::string& label) {
    auto sum = pm4::interpret(s.cmd.data(), s.cmd.size());
    if (sum.draws.size() != 1) { check(false, label + " (no draw)"); return; }
    auto r = execute_draw(vk, s.mem, sum.draws[0]);
    if (r.ok || r.error.find(frag) == std::string::npos) std::printf("  got: ok=%d error='%s'\n", int(r.ok), r.error.c_str());
    check(!r.ok && r.error.find(frag) != std::string::npos && r.rgba.empty(), label);
  };
  { Scene s; s.build(64, 2); refused(s, "strips and fans", "draw: line list refused"); }
  { Scene s; s.build(32); refused(s, "viewport", "draw: viewport that does not match the target is refused"); }
  { Scene s; s.build(64, 4, false); refused(s, "user data", "draw: missing vertex user data refused"); }
  { Scene s; s.build(); s.vs_blob[0] = 0; refused(s, "PVS1", "draw: a vertex program that is not a PVS1 blob is refused"); }
  { Scene s; s.build(); s.mem.unmap(0x400000); refused(s, "vertex buffer is not mapped", "draw: unmapped vertex buffer refused"); }
  { Scene s; s.build(); s.mem.unmap(0x500000); auto sum = pm4::interpret(s.cmd.data(), s.cmd.size()); auto r = execute_draw(vk, s.mem, sum.draws[0]);
    check(r.ok && !r.wrote_target && r.notes.find("not mapped") != std::string::npos, "draw: unmapped colour target returns pixels only and says so"); }

  // ---- slice S7: indexed draws (DRAW_INDEX_INDX) ----
  {
    IndexedScene s32; s32.build(false);
    auto r32 = s32.run(vk);
    if (!r32.ok) std::printf("  indexed-32 error: %s\n", r32.error.c_str());
    check(r32.ok && r32.rgba.size() == 64 * 64 * 4 && r32.wrote_target, "draw: indexed 32-bit draw ok, pixels written to the target");
    IndexedScene s16; s16.build(true);
    auto r16 = s16.run(vk);
    if (!r16.ok) std::printf("  indexed-16 error: %s\n", r16.error.c_str());
    check(r16.ok, "draw: indexed 16-bit draw ok");
    if (r32.ok && r16.ok) {
      uint32_t d = 0;
      for (size_t p = 0; p < size_t(64) * 64 * 4; ++p) d += r32.rgba[p] != r16.rgba[p];
      check(d == 0, "draw: 16-bit and 32-bit index buffers give byte-identical pixels");
      int reds = 0;
      for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x)
        reds += r32.rgba[(y * 64 + x) * 4] == 255 && r32.rgba[(y * 64 + x) * 4 + 1] == 0 && r32.rgba[(y * 64 + x) * 4 + 2] == 0;
      // square (-0.8..0.8)^2 covers 64% of the target: about 2621 pixels
      check(reds > 2500 && reds < 2750, "draw: indexed square covers about 64% of the target (red count " + std::to_string(reds) + ")");
      check(r32.notes.find("index buffer fetched") != std::string::npos, "draw: indexed result notes the guest-memory index fetch");
    }
    // Same square through DRAW_INDEX_AUTO with 6 expanded vertices: identical pixels.
    {
      GpuAddressSpace mem2;
      auto blob = make_pnr_vs_blob(6);
      std::vector<std::uint32_t> m6(16, 0); for (int i = 0; i < 4; ++i) m6[i * 5] = f2u(1.0f);
      std::vector<float> vb6 = {-0.8f, -0.8f, 0, 1, 0, 0,   0.8f, -0.8f, 0, 1, 0, 0,   0.8f, 0.8f, 0, 1, 0, 0,
                                -0.8f, -0.8f, 0, 1, 0, 0,   0.8f,  0.8f, 0, 1, 0, 0,  -0.8f, 0.8f, 0, 1, 0, 0};
      std::vector<std::uint8_t> t6(64 * 64 * 4, 0xCD);
      std::vector<std::uint32_t> ps6{0xbf810000};
      mem2.map_external(0x100000, blob.data(), blob.size() * 4, "vs");
      mem2.map_external(0x200000, ps6.data(), 4, "ps");
      mem2.map_external(0x300000, m6.data(), 64, "matrix");
      mem2.map_external(0x400000, vb6.data(), vb6.size() * 4, "vertices");
      mem2.map_external(0x500000, t6.data(), t6.size(), "target");
      pm4::Builder b;
      b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_VS, {0x100000 >> 8, 0});
      b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_PS, {0x200000 >> 8, 0});
      b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_USER_DATA_VS_0, {0x300000, 0, 0x400000, 0});
      b.set_reg(pm4::SET_CONTEXT_REG, amd::PA_CL_VPORT_XSCALE, {f2u(32.0f), f2u(32.0f), f2u(-32.0f), f2u(32.0f), f2u(0.5f), f2u(0.5f)});
      b.set_context(amd::CB_COLOR0_BASE, 0x500000 >> 8).set_context(amd::CB_TARGET_MASK, 0xf);
      b.set_context(amd::PA_SC_SCREEN_SCISSOR_BR, (64 << 16) | 64);
      b.set_uconfig(amd::VGT_PRIMITIVE_TYPE, 4).num_instances(1).draw_auto(6);
      auto cmd6 = b.dwords();
      auto sum = pm4::interpret(cmd6.data(), cmd6.size());
      auto ra = execute_draw(vk, mem2, sum.draws[0]);
      if (!ra.ok) std::printf("  auto-square error: %s\n", ra.error.c_str());
      if (ra.ok && r32.ok) {
        uint32_t d = 0;
        for (size_t p = 0; p < size_t(64) * 64; ++p) {
          bool df = false;
          for (int c = 0; c < 3; ++c) df |= ra.rgba[p * 4 + c] != r32.rgba[p * 4 + c];
          d += df;
        }
        std::printf("  draw: pixels differing auto vs indexed same-square = %u of %u\n", d, 64 * 64);
        check(d == 0, "draw: indexed draw is pixel-identical to the expanded auto draw");
      } else check(false, "draw: auto square reference draw succeeded");
    }
    // Refusals.
    { IndexedScene s; s.build(false); s.mem.unmap(0x600000);
      auto r = s.run(vk);
      check(!r.ok && r.error.find("index buffer is not mapped") != std::string::npos, "draw: unmapped index buffer refused"); }
    { // Without INDEX_TYPE the PM4 interpreter defaults to 32-bit indices;
      // the draw then succeeds against the 32-bit buffer.
      IndexedScene s; s.build(false, false, false);
      auto r = s.run(vk);
      check(r.ok, "draw: draw_indexed without INDEX_TYPE uses the interpreter's 32-bit default"); }
    { IndexedScene s; s.build(false);
      std::uint32_t big = 5; std::memcpy(&s.ibuf[0], &big, 4);  // index 5 with only 4 vertices
      auto r = s.run(vk);
      check(!r.ok && r.error.find("vertex buffer is not mapped") != std::string::npos, "draw: index past the vertex buffer refused at the vertex fetch"); }
    // Indexed + PVS2 (translated divide) combo.
    { IndexedScene s; s.build(false, true);
      auto r = s.run(vk);
      if (!r.ok) std::printf("  indexed-pvs2 error: %s\n", r.error.c_str());
      if (r.ok && r32.ok) {
        uint32_t d = 0;
        for (size_t p = 0; p < size_t(64) * 64; ++p) {
          bool df = false;
          for (int c = 0; c < 3; ++c) df |= r.rgba[p * 4 + c] != r32.rgba[p * 4 + c];
          d += df;
        }
        check(d == 0, "draw: indexed PVS2 draw pixel-identical to indexed PVS1");
      } else check(false, "draw: indexed PVS2 draw ok"); }
  }

  // ---- slice S8: translated pixel stage (PPS1 constant colour) ----
  {
    auto pu = [](float br, float bg, float bb, float ba, float inten, float amb) {
      const float f[8] = {br, bg, bb, ba, inten, amb, 0.0f, 1.0f};
      std::vector<std::uint32_t> u(8);
      for (int k = 0; k < 8; ++k) std::memcpy(&u[k], &f[k], 4);
      return u;
    };
    {
      const struct { float b[4]; float i, a; } cases[3] = {
        {{1.0f, 0.5f, 0.25f, 1.0f}, 0.5f, 0.5f},
        {{1.0f, 0.5f, 0.25f, 0.75f}, 0.3f, 0.1f},
        {{0.9f, 0.5f, 0.25f, 1.0f}, 1.5f, 0.75f},
      };
      bool ok = true;
      for (const auto& cs : cases) {
        float got[4];
        auto st = rdna_pixel_colour(vk, pu(cs.b[0], cs.b[1], cs.b[2], cs.b[3], cs.i, cs.a), got);
        if (!st) { std::printf("  pps1 run error: %s\n", st.error.message.c_str()); ok = false; break; }
        for (int c = 0; c < 4; ++c) {
          const float want = std::min(std::max(cs.b[c] * (cs.i + cs.a), 0.0f), 1.0f);
          ok = ok && std::memcmp(&got[c], &want, 4) == 0;
        }
      }
      check(ok, "pps1: translated colour bitwise equal to CPU float reference (3 parameter sets)");
    }
    {
      Scene s; s.build();
      s.ps_blob = make_pnr_ps_blob();
      s.mem.map_external(0x210000, s.ps_blob.data(), s.ps_blob.size() * 4, "ps1blob");
      auto u = pu(1.0f, 0.5f, 0.25f, 1.0f, 0.3f, 0.1f);
      pm4::Builder b;
      b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_VS, {0x100000 >> 8, 0});
      b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_PS, {0x210000 >> 8, 0});
      b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_USER_DATA_VS_0, {0x300000, 0, 0x400000, 0});
      b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_USER_DATA_PS_0, {u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7]});
      b.set_reg(pm4::SET_CONTEXT_REG, amd::PA_CL_VPORT_XSCALE, {f2u(32.0f), f2u(32.0f), f2u(-32.0f), f2u(32.0f), f2u(0.5f), f2u(0.5f)});
      b.set_context(amd::CB_COLOR0_BASE, 0x500000 >> 8).set_context(amd::CB_TARGET_MASK, 0xf);
      b.set_context(amd::PA_SC_SCREEN_SCISSOR_BR, (64 << 16) | 64);
      b.set_uconfig(amd::VGT_PRIMITIVE_TYPE, 4).num_instances(1).draw_auto(3);
      auto cmd = b.dwords();
      auto sum = pm4::interpret(cmd.data(), cmd.size());
      auto r = execute_draw(vk, s.mem, sum.draws[0]);
      if (!r.ok) std::printf("  pps1 draw error: %s\n", r.error.c_str());
      check(r.ok && r.notes.find("translated PPS1") != std::string::npos, "pps1: draw ok, result states the pixel shader is translated");
      if (r.ok) {
        Scene ref; ref.build();
        const float cc[3] = {0.4f, 0.2f, 0.1f};
        for (int v = 0; v < 3; ++v) for (int c = 0; c < 3; ++c) ref.vb[v * 6 + 3 + c] = cc[c];
        auto sum2 = pm4::interpret(ref.cmd.data(), ref.cmd.size());
        auto rr = execute_draw(vk, ref.mem, sum2.draws[0]);
        if (!rr.ok) std::printf("  pps1 ref error: %s\n", rr.error.c_str());
        if (rr.ok) {
          uint32_t d = 0;
          for (size_t p = 0; p < size_t(64) * 64 * 4; ++p) d += r.rgba[p] != rr.rgba[p];
          std::printf("  pps1: bytes differing vs constant-colour reference = %u of %u\n", d, 64 * 64 * 4);
          check(d == 0, "pps1: translated-colour draw byte-identical to the reference");
          auto px = [&](int x, int y) { return &r.rgba[(y * 64 + x) * 4]; };
          check(px(32, 32)[0] > 90 && px(32, 32)[0] < 115 && px(32, 32)[1] > 40 && px(32, 32)[1] < 62,
                "pps1: centre pixel is the translated colour (about 102,51,26)");
          check(px(1, 1)[0] == 0 && px(1, 1)[1] == 0, "pps1: a corner stays at the clear colour");
        } else check(false, "pps1: reference draw ok");
      }
      pm4::Builder b2;
      b2.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_VS, {0x100000 >> 8, 0});
      b2.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_PS, {0x210000 >> 8, 0});
      b2.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_USER_DATA_VS_0, {0x300000, 0, 0x400000, 0});
      b2.set_reg(pm4::SET_CONTEXT_REG, amd::PA_CL_VPORT_XSCALE, {f2u(32.0f), f2u(32.0f), f2u(-32.0f), f2u(32.0f), f2u(0.5f), f2u(0.5f)});
      b2.set_context(amd::CB_COLOR0_BASE, 0x500000 >> 8).set_context(amd::CB_TARGET_MASK, 0xf);
      b2.set_context(amd::PA_SC_SCREEN_SCISSOR_BR, (64 << 16) | 64);
      b2.set_uconfig(amd::VGT_PRIMITIVE_TYPE, 4).num_instances(1).draw_auto(3);
      auto cmd2 = b2.dwords();
      auto sum3 = pm4::interpret(cmd2.data(), cmd2.size());
      auto r3 = execute_draw(vk, s.mem, sum3.draws[0]);
      check(!r3.ok && r3.error.find("pixel user data") != std::string::npos, "pps1: missing pixel user data refused");
    }
  }

  // ---- slice S9: batched draws past the 64-lane single-dispatch limit ----
  {
    BatchScene a; a.build(false, false, 96);   // 2 batches: 64 + 32 vertices
    auto ra = a.run(vk);
    if (!ra.ok) std::printf("  pvs3 error: %s\n", ra.error.c_str());
    check(ra.ok && ra.vertices == 96 && ra.wrote_target, "pvs3: 96-vertex batched auto draw ok, pixels written");
    check(ra.ok && ra.notes.find("2 for 96 vertices") != std::string::npos, "pvs3: result states the batch count");
    if (ra.ok) {
      auto px = [&](int x, int y) { return &ra.rgba[(y * 64 + x) * 4]; };
      auto red = [&](const std::uint8_t* p) { return p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] == 255; };
      auto green = [&](const std::uint8_t* p) { return p[0] == 0 && p[1] == 255 && p[2] == 0 && p[3] == 255; };
      int greens = 0;
      for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) greens += green(px(x, y));
      // The mirrored green triangle is drawn last, so its full area shows: about 1311 pixels.
      check(greens > 1250 && greens < 1370, "pvs3: last-batch green triangle covers the expected area (count " + std::to_string(greens) + ")");
      check(green(px(32, 32)), "pvs3: centre pixel is the last-batch green triangle");
      check((red(px(8, 8)) && green(px(8, 56))) || (green(px(8, 8)) && red(px(8, 56))),
            "pvs3: first-batch red and last-batch green each own an exclusive pixel");
      check(std::memcmp(a.target.data(), ra.rgba.data(), ra.rgba.size()) == 0, "pvs3: target memory holds exactly the rendered pixels");
    }
    // Indexed draws referencing 66 vertices (2 batches), with and without the
    // translated post-transform: identity w = 1 geometry must give identical pixels.
    BatchScene b4; b4.build(false, true, 66);
    auto r4 = b4.run(vk);
    BatchScene b7; b7.build(true, true, 66);
    auto r7 = b7.run(vk);
    if (!r4.ok) std::printf("  pvs3-4 error: %s\n", r4.error.c_str());
    if (!r7.ok) std::printf("  pvs3-7 error: %s\n", r7.error.c_str());
    check(r4.ok && r4.vertices == 66, "pvs3: 66-vertex batched indexed draw ok");
    check(r7.ok && r7.vertices == 66 && r7.notes.find("2 for 66 vertices") != std::string::npos,
          "pvs3: batched indexed draw with translated divide ok, batches stated");
    if (r4.ok && r7.ok) {
      std::uint32_t d = 0;
      for (size_t p = 0; p < size_t(64) * 64 * 4; ++p) d += r4.rgba[p] != r7.rgba[p];
      check(d == 0, "pvs3: batched draws with and without the translated divide are pixel-identical (w = 1 geometry)");
    }
    // Refusals.
    { BatchScene d; d.build(false, false, 66);
      d.mem.unmap(0x100000);
      d.vs_blob = make_pnr_vs_blob(3);
      d.mem.map_external(0x100000, d.vs_blob.data(), d.vs_blob.size() * 4, "vs");
      auto r = d.run(vk);
      check(!r.ok && r.error.find("PVS3") != std::string::npos, "pvs3: PVS1 blob with 66 vertices refused, PVS3 named"); }
    { BatchScene e; e.count_override = 65538; e.build(false, false, 96);
      auto r = e.run(vk);
      check(!r.ok && r.error.find("at most 65536") != std::string::npos, "pvs3: auto draw over the 65536-vertex cap refused"); }
    { BatchScene f; f.count_override = 7; f.build(false, false, 96);
      auto r = f.run(vk);
      check(!r.ok && r.error.find("multiple of 3") != std::string::npos, "pvs3: non-multiple-of-3 vertex count refused"); }
  }

  // ---- slice S10: triangle strips and fans (host-side expansion to lists) ----
  {
    // Quad as a 4-vertex strip: triangles (0,1,2) and (2,1,3) after the
    // odd-triangle winding swap. Reference: the same two triangles listed.
    const std::vector<float> quad = {
      -0.8f, -0.8f, 0, 1, 0, 0,   -0.8f, 0.8f, 0, 1, 0, 0,
       0.8f, -0.8f, 0, 1, 0, 0,    0.8f, 0.8f, 0, 1, 0, 0};
    TopoScene strip; strip.build(quad, {}, 6);
    auto rs = strip.run(vk);
    if (!rs.ok) std::printf("  strip error: %s\n", rs.error.c_str());
    check(rs.ok && rs.notes.find("strip expanded") != std::string::npos, "topo: 4-vertex strip auto draw ok, expansion stated");
    TopoScene sref; sref.build(quad, {0, 1, 2, 2, 1, 3}, 4);
    auto rsr = sref.run(vk);
    if (!rsr.ok) std::printf("  strip-ref error: %s\n", rsr.error.c_str());
    if (rs.ok && rsr.ok) {
      std::uint32_t d = 0;
      for (size_t p = 0; p < size_t(64) * 64 * 4; ++p) d += rs.rgba[p] != rsr.rgba[p];
      check(d == 0, "topo: strip draw pixel-identical to the expanded triangle list");
      int reds = 0;
      for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) reds += rs.rgba[(y * 64 + x) * 4] == 255;
      check(reds > 2500 && reds < 2750, "topo: strip quad covers about 64% of the target (count " + std::to_string(reds) + ")");
    } else check(false, "topo: strip reference draw ok");
    // The same quad through an INDEXED strip (index buffer 0,1,2,3).
    TopoScene istrip; istrip.build(quad, {0, 1, 2, 3}, 6);
    auto ris = istrip.run(vk);
    if (!ris.ok) std::printf("  indexed-strip error: %s\n", ris.error.c_str());
    if (ris.ok && rs.ok) {
      std::uint32_t d = 0;
      for (size_t p = 0; p < size_t(64) * 64 * 4; ++p) d += ris.rgba[p] != rs.rgba[p];
      check(d == 0, "topo: indexed strip pixel-identical to the auto strip");
    } else check(false, "topo: indexed strip draw ok");
    // Fan: centre + 4 ring vertices, 3 triangles (the left wedge stays clear).
    const std::vector<float> fan = {
       0.0f,  0.0f, 0, 1, 0, 0,   -0.8f, -0.8f, 0, 1, 0, 0,   0.8f, -0.8f, 0, 1, 0, 0,
       0.8f,  0.8f, 0, 1, 0, 0,   -0.8f,  0.8f, 0, 1, 0, 0};
    TopoScene fs; fs.build(fan, {}, 5);
    auto rf = fs.run(vk);
    if (!rf.ok) std::printf("  fan error: %s\n", rf.error.c_str());
    check(rf.ok && rf.notes.find("fan expanded") != std::string::npos, "topo: 5-vertex fan auto draw ok, expansion stated");
    TopoScene fref; fref.build(fan, {0, 1, 2, 0, 2, 3, 0, 3, 4}, 4);
    auto rfr = fref.run(vk);
    if (!rfr.ok) std::printf("  fan-ref error: %s\n", rfr.error.c_str());
    if (rf.ok && rfr.ok) {
      std::uint32_t d = 0;
      for (size_t p = 0; p < size_t(64) * 64 * 4; ++p) d += rf.rgba[p] != rfr.rgba[p];
      check(d == 0, "topo: fan draw pixel-identical to the expanded triangle list");
      auto px = [&](int x, int y) { return &rf.rgba[(y * 64 + x) * 4]; };
      check(px(32, 32)[0] == 255, "topo: fan centre pixel is drawn");
      check(px(10, 32)[0] == 0 && px(10, 32)[3] == 255, "topo: the missing fan wedge stays at the clear colour");
    } else check(false, "topo: fan reference draw ok");
    // Refusals: too few vertices for a strip/fan; non-triangle primitives.
    { TopoScene t; t.build(quad, {}, 6);
      pm4::Builder b;   // rebuild the command with a 2-vertex strip
      b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_VS, {0x100000 >> 8, 0});
      b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_PS, {0x200000 >> 8, 0});
      b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_USER_DATA_VS_0, {0x300000, 0, 0x400000, 0});
      b.set_reg(pm4::SET_CONTEXT_REG, amd::PA_CL_VPORT_XSCALE, {f2u(32.0f), f2u(32.0f), f2u(-32.0f), f2u(32.0f), f2u(0.5f), f2u(0.5f)});
      b.set_context(amd::CB_COLOR0_BASE, 0x500000 >> 8).set_context(amd::CB_TARGET_MASK, 0xf);
      b.set_context(amd::PA_SC_SCREEN_SCISSOR_BR, (64 << 16) | 64);
      b.set_uconfig(amd::VGT_PRIMITIVE_TYPE, 6).num_instances(1).draw_auto(2);
      auto c2 = b.dwords();
      auto sum = pm4::interpret(c2.data(), c2.size());
      auto r = execute_draw(vk, t.mem, sum.draws[0]);
      check(!r.ok && r.error.find("at least 3") != std::string::npos, "topo: 2-vertex strip refused"); }
    { TopoScene t; t.build(quad, {}, 1);
      auto r = t.run(vk);
      check(!r.ok && r.error.find("strips and fans") != std::string::npos, "topo: point list refused"); }
  }

  // ---- slice S11: translated per-vertex varying (PVS4 Gouraud shade) ----
  {
    const float lp[6] = {0, 0, 1, 0, 0, 1};   // light +z, ambient 0, clamp 0..1
    // One triangle, colours all white, normals nz = 0, 0.5, 1: the translated
    // shade gives per-vertex greys 0, 0.5, 1, interpolated by the rasterizer.
    const std::vector<float> tri9 = {
      -0.8f, -0.8f, 0, 1, 1, 1, 0, 0, 0.0f,
       0.8f, -0.8f, 0, 1, 1, 1, 0, 0, 0.5f,
       0.0f,  0.8f, 0, 1, 1, 1, 0, 0, 1.0f };
    ShadeScene s; s.build(tri9, lp);
    auto rs = s.run(vk);
    if (!rs.ok) std::printf("  pvs4 error: %s\n", rs.error.c_str());
    check(rs.ok && rs.notes.find("PVS4") != std::string::npos, "pvs4: shade draw ok, translated varying stated");
    if (rs.ok) {
      auto lum = [&](int x, int y) { return int(rs.rgba[(y * 64 + x) * 4]); };
      const int centre = lum(32, 32), end_a = lum(32, 8), end_b = lum(32, 56);
      const int bright = std::max(end_a, end_b), dim = std::min(end_a, end_b);
      // barycentric at the centre: 0.25*0 + 0.25*0.5 + 0.5*1 = 0.625 -> byte 159
      std::printf("  pvs4: centre luminance %d (expect about 159), bright end %d, dim end %d\n", centre, bright, dim);
      check(centre > 150 && centre < 168, "pvs4: centre pixel is the interpolated shade (about 159)");
      check(bright > dim + 100, "pvs4: luminance rises from the dark normal to the bright normal (gradient)");
      check(dim < 90 && bright > 230, "pvs4: gradient endpoints near the vertex shades 0.28 and 0.98");
    }
    // Constant normal everywhere: shade is a uniform scale, so the draw is
    // byte-identical to a PVS1 draw with the colours pre-scaled on the CPU.
    const std::vector<float> flat9 = {
      -0.8f, -0.8f, 0, 1, 1, 1, 0, 0, 0.5f,
       0.8f, -0.8f, 0, 1, 1, 1, 0, 0, 0.5f,
       0.0f,  0.8f, 0, 1, 1, 1, 0, 0, 0.5f };
    ShadeScene f9; f9.build(flat9, lp);
    auto rf = f9.run(vk);
    Scene pref; pref.build();
    for (int v = 0; v < 3; ++v) for (int c = 0; c < 3; ++c) pref.vb[v * 6 + 3 + c] = 0.5f;
    auto sump = pm4::interpret(pref.cmd.data(), pref.cmd.size());
    auto rp = execute_draw(vk, pref.mem, sump.draws[0]);
    if (!rf.ok) std::printf("  pvs4-flat error: %s\n", rf.error.c_str());
    if (!rp.ok) std::printf("  pvs4-ref error: %s\n", rp.error.c_str());
    if (rf.ok && rp.ok) {
      std::uint32_t d = 0;
      for (size_t p = 0; p < size_t(64) * 64 * 4; ++p) d += rf.rgba[p] != rp.rgba[p];
      check(d == 0, "pvs4: constant-shade draw byte-identical to the CPU pre-scaled reference");
    } else check(false, "pvs4: constant-shade draws ok");
    // Batched: 66 vertices (2 dispatches) through the PVS4 path.
    {
      std::vector<float> big;
      for (int t = 0; t < 22; ++t)
        for (int v = 0; v < 3; ++v) {
          const float pos[3] = {-0.8f, -0.8f, 0};
          const float px2[3] = {0.8f, -0.8f, 0};
          const float py2[3] = {0.0f, 0.8f, 0};
          const float* p = (v == 0) ? pos : (v == 1) ? px2 : py2;
          big.insert(big.end(), {p[0], p[1], p[2], 1, 1, 1, 0, 0, 1.0f});
        }
      ShadeScene b66; b66.build(big, lp);
      auto rb = b66.run(vk);
      if (!rb.ok) std::printf("  pvs4-66 error: %s\n", rb.error.c_str());
      check(rb.ok && rb.vertices == 66 && rb.notes.find("2 for 66 vertices") != std::string::npos,
            "pvs4: 66-vertex shade draw batched (2 dispatches)");
      if (rb.ok) check(rb.rgba[(32 * 64 + 32) * 4] == 255, "pvs4: batched draw centre pixel fully lit (nz = 1)");
    }
    // Missing light/ambient/clamp parameters are refused.
    { ShadeScene m; m.build(tri9, lp, false);
      auto rm = m.run(vk);
      check(!rm.ok && rm.error.find("user data 4..9") != std::string::npos, "pvs4: missing shade parameters refused"); }
  }

  // ---- slice S12: depth testing (DB_DEPTH_CONTROL) ----
  {
    // Two full-screen overlapping quads in ONE draw: near red (z = 0.2) and
    // far green (z = 0.8). With depth on, red wins in BOTH draw orders.
    auto mkquad = [](float z, float rr, float gg, std::vector<float>* out) {
      const float p[4][2] = {{-0.8f, -0.8f}, {0.8f, -0.8f}, {0.8f, 0.8f}, {-0.8f, 0.8f}};
      const int order[6] = {0, 1, 2, 0, 2, 3};
      for (int k = 0; k < 6; ++k)
        out->insert(out->end(), {p[order[k]][0], p[order[k]][1], z, rr, gg, 0.0f});
    };
    std::vector<float> rg; mkquad(0.2f, 1, 0, &rg); mkquad(0.8f, 0, 1, &rg);   // red first
    std::vector<float> gr; mkquad(0.8f, 0, 1, &gr); mkquad(0.2f, 1, 0, &gr);   // green first
    auto centre = [](const DrawResult& r) { return &r.rgba[(32 * 64 + 32) * 4]; };
    auto is_red = [](const std::uint8_t* p) { return p[0] == 255 && p[1] == 0; };
    auto is_green = [](const std::uint8_t* p) { return p[0] == 0 && p[1] == 255; };
    TopoScene d1; d1.depth_control = 0x16; d1.build(rg, {}, 4); // depth on (enable + write + LESS), red first
    auto r1 = d1.run(vk);
    TopoScene d2; d2.depth_control = 0x16; d2.build(gr, {}, 4); // depth on (enable + write + LESS), green first
    auto r2 = d2.run(vk);
    TopoScene d0; d0.build(rg, {}, 4);                          // depth off, same red-first order as d1
    auto r0 = d0.run(vk);
    if (!r1.ok) std::printf("  depth-1 error: %s\n", r1.error.c_str());
    if (!r2.ok) std::printf("  depth-2 error: %s\n", r2.error.c_str());
    if (!r0.ok) std::printf("  depth-0 error: %s\n", r0.error.c_str());
    check(r1.ok && r2.ok && r0.ok, "depth: three draws ok");
    if (r1.ok && r2.ok && r0.ok) {
      check(is_red(centre(r1)) && r1.notes.find("depth testing on") != std::string::npos,
            "depth: near quad wins when drawn first, depth stated");
      check(is_red(centre(r2)), "depth: near quad wins when drawn second (order-independent)");
      check(is_green(centre(r0)), "depth: same draw order with depth off - the last-drawn (green) quad wins (control)");
      std::uint32_t d = 0;
      for (size_t p = 0; p < size_t(64) * 64 * 4; ++p) d += r1.rgba[p] != r2.rgba[p];
      check(d == 0, "depth: both draw orders give byte-identical pixels with depth on");
    }
    // Refusals: stencil, and compare functions other than LESS.
    { TopoScene s; s.depth_control = 3; // stencil bit + enable, func field 0 (stencil refused first)
      s.build(rg, {}, 4);
      auto r = s.run(vk);
      check(!r.ok && r.error.find("stencil") != std::string::npos, "depth: stencil enable refused"); }
    { TopoScene g; g.depth_control = 2 | (4u << 4); g.build(rg, {}, 4);
      auto r = g.run(vk);
      check(!r.ok && r.error.find("LESS") != std::string::npos, "depth: GREATER compare refused (only LESS supported)"); }
  }

  // ---- slice S13: frame-persistent render targets (bound DB depth buffer) ----
  {
    // A big far green quad (z = 0.8, covers [-0.8,0.8]) and a small near red
    // quad (z = 0.2, covers [-0.4,0.4]) as SEPARATE draws into one frame.
    auto mkq = [](float half, float z, float rr, float gg, std::vector<float>* out) {
      const float p[4][2] = {{-half, -half}, {half, -half}, {half, half}, {-half, half}};
      const int order[6] = {0, 1, 2, 0, 2, 3};
      for (int k = 0; k < 6; ++k)
        out->insert(out->end(), {p[order[k]][0], p[order[k]][1], z, rr, gg, 0.0f});
    };
    std::vector<float> green_far; mkq(0.8f, 0.8f, 0, 1, &green_far);
    std::vector<float> red_near;  mkq(0.4f, 0.2f, 1, 0, &red_near);
    auto at = [](const DrawResult& r, int x, int y) { return &r.rgba[(y * 64 + x) * 4]; };
    auto is_red = [](const std::uint8_t* p) { return p[0] == 255 && p[1] == 0; };
    auto is_green = [](const std::uint8_t* p) { return p[0] == 0 && p[1] == 255; };
    auto mk = [&](const std::vector<float>& v, std::uint64_t zb, std::uint32_t dctl, TopoScene* t) {
      t->depth_control = dctl; t->z_base = zb; t->build(v, {}, 4);
    };
    // Frame 1: far green first, near red second, one shared frame. The final
    // image keeps green outside the red quad (accumulation) and shows red in
    // the centre (depth lets the nearer second draw overwrite).
    { DrawFrameCache f;
      TopoScene g, r2; mk(green_far, 0x700000, 0x16, &g); mk(red_near, 0x700000, 0x16, &r2);
      auto r1 = g.run(vk, f); auto r2r = r2.run(vk, f);
      if (!r1.ok) std::printf("  frame1-1 error: %s\n", r1.error.c_str());
      if (!r2r.ok) std::printf("  frame1-2 error: %s\n", r2r.error.c_str());
      check(r1.ok && r2r.ok, "frame: two draws into one frame ok");
      if (r1.ok && r2r.ok) {
        check(is_red(at(r2r, 32, 32)), "frame: near second draw wins the overlap (cross-draw depth)");
        check(is_green(at(r2r, 8, 8)), "frame: first draw's pixels survive the second draw (accumulation)");
        check(r2r.notes.find("frame-persistent depth surface") != std::string::npos,
              "frame: notes state the persistent depth surface");
      } }
    // Frame 2: near red FIRST, far green second. Cross-draw depth must block
    // the green quad over the centre - with a per-draw surface the second
    // draw would clear depth and paint green everywhere.
    { DrawFrameCache f;
      TopoScene r1, g; mk(red_near, 0x710000, 0x16, &r1); mk(green_far, 0x710000, 0x16, &g);
      auto ra = r1.run(vk, f); auto rb = g.run(vk, f);
      check(ra.ok && rb.ok, "frame: reversed order draws ok");
      if (ra.ok && rb.ok) {
        check(is_red(at(rb, 32, 32)), "frame: far second draw blocked by earlier depth (centre stays red)");
        check(is_green(at(rb, 8, 8)), "frame: far second draw still covers empty depth");
      } }
    // Frame 3: control - the same two draws WITHOUT a shared cache keep the
    // per-draw behaviour: the second draw only sees its own clear.
    { TopoScene r1, g; mk(red_near, 0x720000, 0x16, &r1); mk(green_far, 0x720000, 0x16, &g);
      auto ra = r1.run(vk); auto rb = g.run(vk);
      check(ra.ok && rb.ok && is_green(at(rb, 32, 32)),
            "frame: without a shared cache the second draw repaints the centre (control)");
      (void)ra; }
    // Depth write off (enable + LESS, write bit clear): the first draw does
    // not update depth, so the far second draw passes everywhere.
    { DrawFrameCache f;
      TopoScene r1, g; mk(red_near, 0x730000, 0x12, &r1); mk(green_far, 0x730000, 0x16, &g);
      auto ra = r1.run(vk, f); auto rb = g.run(vk, f);
      check(ra.ok && rb.ok && is_green(at(rb, 32, 32)),
            "frame: depth-write-off draw leaves depth untouched (far quad passes)");
      (void)ra; }
    // A new depth buffer address restarts the frame: far green after near
    // red under a NEW z_base clears and paints over the centre.
    { DrawFrameCache f;
      TopoScene r1, g; mk(red_near, 0x740000, 0x16, &r1); mk(green_far, 0x750000, 0x16, &g);
      auto ra = r1.run(vk, f); auto rb = g.run(vk, f);
      check(ra.ok && rb.ok && is_green(at(rb, 32, 32)),
            "frame: a new depth buffer address restarts the frame (fresh clear)");
      (void)ra; }
    // Refusals: Z16, tiled, stencil format, separate read/write bases.
    { TopoScene t; mk(red_near, 0x760000, 0x16, &t); t.zinfo = 1; t.build(red_near, {}, 4);
      DrawFrameCache f; auto r = t.run(vk, f);
      check(!r.ok && r.error.find("16-bit depth") != std::string::npos, "frame: Z16 depth surface refused"); }
    { TopoScene t; t.zinfo = 3 | (1u << 28); t.depth_control = 0x16; t.z_base = 0x770000; t.build(red_near, {}, 4);
      DrawFrameCache f; auto r = t.run(vk, f);
      check(!r.ok && r.error.find("tiled depth") != std::string::npos, "frame: tiled (HTILE) depth refused"); }
    { TopoScene t; t.stencil_info = 1; t.depth_control = 0x16; t.z_base = 0x780000; t.build(red_near, {}, 4);
      DrawFrameCache f; auto r = t.run(vk, f);
      check(!r.ok && r.error.find("stencil") != std::string::npos, "frame: stencil surface refused"); }
    { TopoScene t; t.z_write_base = 0x790000; t.depth_control = 0x16; t.z_base = 0x780000; t.build(red_near, {}, 4);
      DrawFrameCache f; auto r = t.run(vk, f);
      check(!r.ok && r.error.find("separate depth read/write bases") != std::string::npos,
            "frame: separate depth read/write bases refused"); }
  }

  // ---- slice S14: target-0 blending (CB_BLEND0_CONTROL) ----
  {
    // Opaque green quad, then a red quad over it in the SAME frame with
    // blending on. GNM factors match Vulkan 1:1; enable is bit 30.
    auto mkq = [](float half, float rr, float gg, std::vector<float>* out) {
      const float p[4][2] = {{-half, -half}, {half, -half}, {half, half}, {-half, half}};
      const int order[6] = {0, 1, 2, 0, 2, 3};
      for (int k = 0; k < 6; ++k)
        out->insert(out->end(), {p[order[k]][0], p[order[k]][1], 0.5f, rr, gg, 0.0f});
    };
    std::vector<float> green; mkq(0.8f, 0, 1, &green);
    std::vector<float> red;   mkq(0.4f, 1, 0, &red);
    auto centre = [](const DrawResult& r) { return &r.rgba[(32 * 64 + 32) * 4]; };
    const std::uint32_t EN = 1u << 30;
    auto blend_val = [](std::uint32_t csf, std::uint32_t cf, std::uint32_t cdf) {
      return EN | (csf & 31) | ((cf & 7) << 5) | ((cdf & 31) << 8);
    };
    // Additive (src ONE, dst ONE, ADD): red over green -> yellow.
    { DrawFrameCache f;
      TopoScene g, r2; g.build(green, {}, 4); r2.blend0 = blend_val(1, 0, 1); r2.build(red, {}, 4);
      auto ra = g.run(vk, f); auto rb = r2.run(vk, f);
      if (!rb.ok) std::printf("  blend-add error: %s\n", rb.error.c_str());
      check(ra.ok && rb.ok, "blend: additive draw ok");
      if (ra.ok && rb.ok) {
        const std::uint8_t* p = centre(rb);
        check(p[0] == 255 && p[1] == 255 && p[2] == 0, "blend: ONE+ONE add gives yellow over green");
        check(rb.notes.find("target-0 blending on") != std::string::npos, "blend: notes state blending");
      } }
    // MIN (src ONE, dst ONE, MIN): min(red, green) -> black.
    { DrawFrameCache f;
      TopoScene g, r2; g.build(green, {}, 4); r2.blend0 = blend_val(1, 2, 1); r2.build(red, {}, 4);
      auto ra = g.run(vk, f); auto rb = r2.run(vk, f);
      check(ra.ok && rb.ok, "blend: min draw ok");
      if (ra.ok && rb.ok) {
        const std::uint8_t* p = centre(rb);
        check(p[0] == 0 && p[1] == 0 && p[2] == 0, "blend: MIN gives black over green");
      } }
    // DST_COLOR factor (src DST_COLOR, dst ZERO, ADD): red * green -> black.
    { DrawFrameCache f;
      TopoScene g, r2; g.build(green, {}, 4); r2.blend0 = blend_val(8, 0, 0); r2.build(red, {}, 4);
      auto ra = g.run(vk, f); auto rb = r2.run(vk, f);
      check(ra.ok && rb.ok, "blend: dst_color draw ok");
      if (ra.ok && rb.ok) {
        const std::uint8_t* p = centre(rb);

        check(p[0] == 0 && p[1] == 0 && p[2] == 0, "blend: src*DST_COLOR multiplies red by green (black)");
      } }
    // SRC_ALPHA (GNM 4; fragment alpha is 1.0, so red passes through) -
    // pins the alpha/colour swap of the factor table: a wrong map to
    // Vulkan's DST_COLOR (4) would give red*green = black.
    { DrawFrameCache f;
      TopoScene g, r2; g.build(green, {}, 4); r2.blend0 = blend_val(4, 0, 0); r2.build(red, {}, 4);
      auto ra = g.run(vk, f); auto rb = r2.run(vk, f);
      check(ra.ok && rb.ok && centre(rb)[0] == 255 && centre(rb)[1] == 0 && centre(rb)[2] == 0,
            "blend: SRC_ALPHA acts as ONE at alpha 1.0");
      (void)ra; }
    // Control: no blend register -> the second draw replaces (pure red).
    { DrawFrameCache f;
      TopoScene g, r2; g.build(green, {}, 4); r2.build(red, {}, 4);
      auto ra = g.run(vk, f); auto rb = r2.run(vk, f);
      check(ra.ok && rb.ok && centre(rb)[0] == 255 && centre(rb)[1] == 0,
            "blend: without CB_BLEND0_CONTROL the draw replaces (control)");
      (void)ra; }
    // Refusals: constant-colour and dual-source factors.
    { TopoScene t; t.blend0 = blend_val(13, 0, 1); t.build(red, {}, 4);
      DrawFrameCache f; auto r = t.run(vk, f);
      check(!r.ok && r.error.find("blend factor not supported") != std::string::npos,
            "blend: constant-colour factor refused"); }
    { TopoScene t; t.blend0 = blend_val(15, 0, 1); t.build(red, {}, 4);
      DrawFrameCache f; auto r = t.run(vk, f);
      check(!r.ok && r.error.find("blend factor not supported") != std::string::npos,
            "blend: dual-source (src1) factor refused"); }

  // Slice S15: vertex fetch through V# descriptors (PVS5). The reference is
  // the same quad through the raw-pointer blob path; pixels must match.
  {
    TopoScene ref; ref.build(red, {}, 4);
    DrawFrameCache f0; auto rr = ref.run(vk, f0);
    VsharpScene s; s.build(red, 4);
    auto rv = s.run(vk);
    check(rr.ok && rv.ok, "fetch: reference and PVS5 draws run");
    if (rr.ok && rv.ok) {
      std::uint32_t diff = 0;
      for (std::size_t i = 0; i < rr.rgba.size(); ++i) if (rr.rgba[i] != rv.rgba[i]) ++diff;
      check(diff == 0, "fetch: PVS5 pixels identical to raw-pointer reference");
      check(rv.notes.find("vertex fetch via V#") != std::string::npos, "fetch: notes state the V# path");
    }
    // Packed separate buffers: position packed at the buffer base (stride 0),
    // colour packed right behind it in a second region.
    {
      std::vector<float> joined;
      for (std::size_t i = 0; i + 5 < red.size(); i += 6)
        joined.insert(joined.end(), {red[i], red[i + 1], red[i + 2]});
      const std::size_t pos_floats = joined.size();
      for (std::size_t i = 0; i + 5 < red.size(); i += 6)
        joined.insert(joined.end(), {red[i + 3], red[i + 4], red[i + 5]});
      VsharpScene p;
      p.pos_stride = 0; p.col_stride = 0;   // packed: stride_words = components
      p.pos_base = 0x400000; p.col_base = 0x400000 + pos_floats * 4;
      p.build(joined, 4);
      auto rp = p.run(vk);
      check(rp.ok, std::string("fetch: packed separate buffers draw ok") + (rp.ok ? "" : (" - " + rp.error)));
      if (rp.ok && rr.ok) {
        std::uint32_t diff = 0;
        for (std::size_t i = 0; i < rp.rgba.size(); ++i) if (rp.rgba[i] != rr.rgba[i]) ++diff;
        check(diff == 0, "fetch: packed-buffer pixels identical to reference");
      }
    }
    // Format32_32_32_32 position (w present, ignored) at a wider stride.
    {
      std::vector<float> wide;
      for (std::size_t i = 0; i + 5 < red.size(); i += 6)
        wide.insert(wide.end(), {red[i], red[i + 1], red[i + 2], 1.0f, red[i + 3], red[i + 4], red[i + 5]});
      VsharpScene w4;
      w4.pos_fmt = 14; w4.pos_stride = 28; w4.col_stride = 28; w4.col_base = 0x400000 + 16;
      w4.vcount = 6;
      w4.build(wide, 4);
      auto rw = w4.run(vk);
      check(rw.ok, std::string("fetch: xyzw position draw ok") + (rw.ok ? "" : (" - " + rw.error)));
      if (rw.ok && rr.ok) {
        std::uint32_t diff = 0;
        for (std::size_t i = 0; i < rw.rgba.size(); ++i) if (rw.rgba[i] != rr.rgba[i]) ++diff;
        check(diff == 0, "fetch: xyzw-position pixels identical to reference");
      }
    }
    // Refusals.
    { VsharpScene b1; b1.pos_w3 = VsharpScene::vsel(13) | (1u << 23); b1.build(red, 4);
      auto r1 = b1.run(vk);
      check(!r1.ok && r1.error.find("add_tid") != std::string::npos, "fetch: add_tid_enable refused"); }
    { VsharpScene b2; b2.pos_w3 = 0x688u | (0u << 12) | (13u << 15); b2.build(red, 4);  // unorm
      auto r2 = b2.run(vk);
      check(!r2.ok && r2.error.find("number format is not float") != std::string::npos, "fetch: unorm vertex data refused"); }
    { VsharpScene b3; b3.pos_w3 = VsharpScene::vsel(10); b3.build(red, 4);   // 8_8_8_8
      auto r3 = b3.run(vk);
      check(!r3.ok && r3.error.find("data format is not a supported") != std::string::npos, "fetch: packed 8-bit format refused"); }
    { VsharpScene b4; b4.num_records = 3; b4.build(red, 4);                  // red quad has 6 vertices
      auto r4 = b4.run(vk);
      check(!r4.ok && r4.error.find("num_records") != std::string::npos, "fetch: short buffer refused"); }
    { VsharpScene b5; b5.pos_w3 = (0x688u ^ 7u) | (7u << 12) | (13u << 15); b5.build(red, 4);  // dst_sel x = 7
      auto r5 = b5.run(vk);
      check(!r5.ok && r5.error.find("swizzle") != std::string::npos, "fetch: non-identity dst_sel refused"); }
    // Batched: 66 vertices (22 triangles) crosses two 64-lane fetch dispatches.
    {
      std::vector<float> fan;
      for (int k = 0; k < 22; ++k) {
        const float x = -0.9f + 0.08f * float(k);
        fan.insert(fan.end(), {x, -0.5f, 0.5f, 0, 1, 0, x + 0.06f, -0.5f, 0.5f, 0, 1, 0, x + 0.03f, 0.5f, 0.5f, 0, 1, 0});
      }
      VsharpScene s66; s66.build(fan, 4);
      auto r66 = s66.run(vk);
      check(r66.ok, "fetch: 66-vertex batched draw ok");
      if (r66.ok) {
        std::uint32_t green = 0;
        for (std::size_t i = 0; i + 3 < r66.rgba.size(); i += 4)
          if (r66.rgba[i + 1] == 255 && r66.rgba[i] == 0) ++green;
        check(green > 500, "fetch: batched draw covers the screen");
      }
    }
  }
  }
}
