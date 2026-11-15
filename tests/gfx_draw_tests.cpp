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
  const std::vector<std::uint32_t>* raw_vb = nullptr;  // raw record words (8_8_8_8 colours)
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
    const void* vbytes = raw_vb ? static_cast<const void*>(raw_vb->data()) : static_cast<const void*>(vb.data());
    const std::size_t vsize = raw_vb ? raw_vb->size() * 4 : vb.size() * 4;
    mat.assign(16, 0); for (int i = 0; i < 4; ++i) mat[i * 5] = f2u(1.0f);
    target.assign(W * H * 4, 0xCD);
    vtable.resize(8);
    vsharp(&vtable[0], pos_base, pos_stride, nrec, pos_w3 ? pos_w3 : vsel(pos_fmt));
    vsharp(&vtable[4], col_base, col_stride, nrec, col_w3 ? col_w3 : vsel(col_fmt));
    mem.map_external(0x100000, vs_blob.data(), vs_blob.size() * 4, "vs");
    mem.map_external(0x200000, ps_blob.data(), 4, "ps");
    mem.map_external(0x300000, mat.data(), 64, "matrix");
    mem.map_external(0x400000, const_cast<void*>(vbytes), vsize, "vertices");
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
  bool mode_cntl_set = false;        // write PA_SU_SC_MODE_CNTL (slice S18)
  std::uint32_t mode_cntl = 0;       // raw PolygonControl value
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
    if (mode_cntl_set) b.set_context(amd::PA_SU_SC_MODE_CNTL, mode_cntl);
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
                  std::uint32_t type = 9, std::uint32_t last_array = 0) {
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
  void build_array(std::uint32_t nslices, float layer, std::uint32_t df = 10,
                   std::uint32_t last_level = 0, std::uint32_t mip_filter = 0,
                   std::uint32_t t_min_lod = 0, std::uint32_t tiling = 0,
                   bool array_blob = true) {
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
      for (std::uint32_t s = 0; s < nslices; ++s) {
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
    make_tdesc(tw, 0x700000, side, side, side, tiling, last_level, t_min_lod, df, 0, 13, nslices - 1);
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
    // Refusal: stencil (all eight compare functions are supported - slice S26).
    { TopoScene s; s.depth_control = 3; // stencil bit + enable, func field 0 (stencil refused first)
      s.build(rg, {}, 4);
      auto r = s.run(vk);
      check(!r.ok && r.error.find("stencil") != std::string::npos, "depth: stencil enable refused"); }
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

  // ---- slice S26: all eight depth compare functions ----
  {
    // A green reference quad (z = 0.5) drawn with ALWAYS + depth write, then
    // a red test quad with the function under test into the same frame. The
    // centre is red iff func(z_test, 0.5) passes.
    auto mkz = [](float z, float rr, float gg, std::vector<float>* out) {
      const float p[4][2] = {{-0.8f, -0.8f}, {0.8f, -0.8f}, {0.8f, 0.8f}, {-0.8f, 0.8f}};
      const int order[6] = {0, 1, 2, 0, 2, 3};
      for (int k = 0; k < 6; ++k)
        out->insert(out->end(), {p[order[k]][0], p[order[k]][1], z, rr, gg, 0.0f});
    };
    auto red_at_centre = [](const DrawResult& r) {
      const std::uint8_t* p = &r.rgba[(32 * 64 + 32) * 4];
      return p[0] == 255 && p[1] == 0;
    };
    struct CmpCase { std::uint32_t func; float z; bool red; const char* name; };
    const CmpCase cases[] = {
      {0, 0.4f, false, "cmp NEVER: never passes"},
      {1, 0.4f, true,  "cmp LESS: nearer passes"},
      {1, 0.6f, false, "cmp LESS: farther fails"},
      {2, 0.5f, true,  "cmp EQUAL: equal passes"},
      {2, 0.4f, false, "cmp EQUAL: different fails"},
      {3, 0.5f, true,  "cmp LEQUAL: equal passes"},
      {3, 0.6f, false, "cmp LEQUAL: farther fails"},
      {4, 0.6f, true,  "cmp GREATER: farther passes"},
      {4, 0.4f, false, "cmp GREATER: nearer fails"},
      {5, 0.4f, true,  "cmp NOTEQUAL: different passes"},
      {5, 0.5f, false, "cmp NOTEQUAL: equal fails"},
      {6, 0.5f, true,  "cmp GEQUAL: equal passes"},
      {6, 0.4f, false, "cmp GEQUAL: nearer fails"},
      {7, 0.6f, true,  "cmp ALWAYS: always passes"},
    };
    std::uint32_t zb = 0x800000;
    for (const auto& c : cases) {
      std::vector<float> ref; mkz(0.5f, 0, 1, &ref);
      std::vector<float> tst; mkz(c.z, 1, 0, &tst);
      DrawFrameCache f;
      TopoScene g, t;
      g.depth_control = 2 | 4 | (7u << 4); g.z_base = zb; g.build(ref, {}, 4);
      t.depth_control = 2 | 4 | (c.func << 4); t.z_base = zb; t.build(tst, {}, 4);
      auto rgr = g.run(vk, f); auto rt = t.run(vk, f);
      check(rgr.ok && rt.ok && (red_at_centre(rt) == c.red), c.name);
      zb += 0x10000;
    }
    // Notes name the selected function.
    { DrawFrameCache f;
      std::vector<float> far_green; mkz(0.8f, 0, 1, &far_green);
      TopoScene g; g.depth_control = 2 | 4 | (4u << 4); g.z_base = 0x8F0000; g.build(far_green, {}, 4);
      auto r = g.run(vk, f);
      check(r.ok && r.notes.find("GREATER") != std::string::npos, "cmp: notes name the compare function"); }
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
    // Slice S16: 8_8_8_8 unorm colours - byte r,g,b packed one dword per
    // vertex behind the xyz floats (record = 4 dwords). Converted in
    // translated RDNA (and + cvt + scale); pixels must match the float
    // reference with colours byte/255.
    {
      const std::uint32_t qu[6][3] = {{255, 128, 64}, {255, 128, 64}, {255, 128, 64},
                                      {255, 128, 64}, {255, 128, 64}, {255, 128, 64}};  // constant: no interpolation
      std::vector<std::uint32_t> rec;
      auto f2u_ = [](float f) { std::uint32_t u; std::memcpy(&u, &f, 4); return u; };
      for (int v = 0; v < 6; ++v) {
        rec.push_back(f2u_(red[v * 6])); rec.push_back(f2u_(red[v * 6 + 1])); rec.push_back(f2u_(red[v * 6 + 2]));
        rec.push_back(0xFF000000u | (qu[v][2] << 16) | (qu[v][1] << 8) | qu[v][0]);
      }
      VsharpScene u8;
      u8.col_fmt = 10; u8.pos_stride = 16; u8.col_stride = 16; u8.col_base = 0x40000C;
      u8.col_w3 = 0x688u | (0u << 12) | (10u << 15);   // unorm 8_8_8_8
      u8.vcount = 6;
      u8.raw_vb = &rec;
      u8.build(std::vector<float>(), 4);
      auto ru = u8.run(vk);
      check(ru.ok, std::string("fetch: 8_8_8_8 unorm colour draw ok") + (ru.ok ? "" : (" - " + ru.error)));
      // Float reference: per-vertex float colours byte/255, same geometry.
      std::vector<float> fref;
      for (int v = 0; v < 6; ++v)
        fref.insert(fref.end(), {red[v * 6], red[v * 6 + 1], red[v * 6 + 2],
                                 qu[v][0] / 255.0f, qu[v][1] / 255.0f, qu[v][2] / 255.0f});
      TopoScene fr; fr.build(fref, {}, 4);
      DrawFrameCache f1; auto rf = fr.run(vk, f1);
      if (ru.ok && rf.ok) {
        std::uint32_t diff = 0;
        for (std::size_t i = 0; i < ru.rgba.size(); ++i) if (ru.rgba[i] != rf.rgba[i]) ++diff;
        check(diff == 0, "fetch: 8_8_8_8 unorm pixels identical to float reference");
        check(centre(ru)[0] == 255 && centre(ru)[1] == 128 && centre(ru)[2] == 64,
              "fetch: 8_8_8_8 centre byte-exact (255,128,64)");
      }
    }
    // Refusals.
    { VsharpScene b1; b1.pos_w3 = VsharpScene::vsel(13) | (1u << 23); b1.build(red, 4);
      auto r1 = b1.run(vk);
      check(!r1.ok && r1.error.find("add_tid") != std::string::npos, "fetch: add_tid_enable refused"); }
    { VsharpScene b2; b2.pos_w3 = 0x688u | (0u << 12) | (13u << 15); b2.build(red, 4);  // unorm
      auto r2 = b2.run(vk);
      check(!r2.ok && r2.error.find("number format is not float") != std::string::npos, "fetch: unorm vertex data refused"); }
    { VsharpScene b3; b3.pos_w3 = VsharpScene::vsel(9); b3.build(red, 4);    // 2_10_10_10
      auto r3 = b3.run(vk);
      check(!r3.ok && r3.error.find("data format is not supported") != std::string::npos, "fetch: packed 10-bit format refused"); }
    { VsharpScene b3b; b3b.col_w3 = VsharpScene::vsel(10); b3b.build(red, 4);  // 8_8_8_8 with float numerics
      auto r3b = b3b.run(vk);
      check(!r3b.ok && r3b.error.find("unorm numerics") != std::string::npos, "fetch: 8_8_8_8 non-unorm refused"); }
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

  // ---- slice S18: face culling + front face (PA_SU_SC_MODE_CNTL) ----
  // Two triangles side by side: LEFT faces front when FACE=CCW, RIGHT faces
  // back. Front for FACE=CCW is a negative signed area in our framebuffer
  // (Vulkan's area convention negates the usual formula); that matches the
  // guest's GNM window-space facing because the guest viewport's y-flip
  // (yscale=-H/2) cancels that negation. The fan below covers three wedges
  // (top-right, top-left, bottom-left); its bottom-right quadrant is empty
  // by construction.
  {
    const std::vector<float> two = {
        -0.8f, -0.5f, 0.5f, 1, 0, 0,   // left: front when FACE=CCW
        -0.5f,  0.5f, 0.5f, 1, 0, 0,
        -0.2f, -0.5f, 0.5f, 1, 0, 0,
         0.2f, -0.5f, 0.5f, 0, 1, 0,   // right: back when FACE=CCW
         0.8f, -0.5f, 0.5f, 0, 1, 0,
         0.5f,  0.5f, 0.5f, 0, 1, 0,
    };
    auto lit = [](const DrawResult& r, std::uint32_t x, std::uint32_t y) {
      const auto* p = &r.rgba[(y * 64 + x) * 4];
      return p[0] > 16 || p[1] > 16 || p[2] > 16;   // clear is black
    };
    { TopoScene s; s.build(two, {}, 4);              // register never written
      auto r = s.run(vk);
      check(r.ok && lit(r, 16, 37) && lit(r, 48, 37), "cull: no PA_SU_SC_MODE_CNTL keeps both faces"); }
    { TopoScene s; s.mode_cntl_set = true; s.mode_cntl = 0; s.build(two, {}, 4);
      auto r = s.run(vk);
      check(r.ok && lit(r, 16, 37) && lit(r, 48, 37), "cull: PA_SU_SC_MODE_CNTL 0 keeps both faces"); }
    { TopoScene s; s.mode_cntl_set = true; s.mode_cntl = 2; s.build(two, {}, 4);   // cull back, CCW front
      auto r = s.run(vk);
      check(r.ok && lit(r, 16, 37) && !lit(r, 48, 37), "cull: cull-back drops the back-face (CW-wound) triangle"); }
    { TopoScene s; s.mode_cntl_set = true; s.mode_cntl = 1; s.build(two, {}, 4);   // cull front, CCW front
      auto r = s.run(vk);
      check(r.ok && !lit(r, 16, 37) && lit(r, 48, 37), "cull: cull-front drops the front-face (CCW-wound) triangle"); }
    { TopoScene s; s.mode_cntl_set = true; s.mode_cntl = 2 | 4; s.build(two, {}, 4);  // cull back, CW front
      auto r = s.run(vk);
      check(r.ok && !lit(r, 16, 37) && lit(r, 48, 37), "cull: CW front face flips the culled side"); }
    { TopoScene s; s.mode_cntl_set = true; s.mode_cntl = 3; s.build(two, {}, 4);   // cull front+back
      auto r = s.run(vk);
      check(r.ok && !lit(r, 16, 37) && !lit(r, 48, 37), "cull: front+back culls every triangle"); }
    { TopoScene s; s.mode_cntl_set = true; s.mode_cntl = (1u << 19); s.build(two, {}, 4);  // provoking vertex bit
      auto r = s.run(vk);
      check(r.ok && lit(r, 16, 37) && lit(r, 48, 37), "cull: provoking-vertex bit has no effect in this subset"); }
    { TopoScene s; s.mode_cntl_set = true; s.mode_cntl = 2; s.depth_control = 0x16;   // cull + depth LESS
      s.build(two, {}, 4);
      auto r = s.run(vk);
      check(r.ok && lit(r, 16, 37) && !lit(r, 48, 37), "cull: culling composes with depth testing"); }
    // Strips and fans: the S10 host expansion corrects the winding, so with
    // cull-back every expanded triangle still faces front.
    const std::vector<float> quad = {
        -0.9f, -0.5f, 0.5f, 1, 1, 1,
        -0.9f,  0.5f, 0.5f, 1, 1, 1,
         0.9f, -0.5f, 0.5f, 1, 1, 1,
         0.9f,  0.5f, 0.5f, 1, 1, 1,
    };
    { TopoScene s; s.mode_cntl_set = true; s.mode_cntl = 2; s.build(quad, {}, 6);   // strip + cull back
      auto r = s.run(vk);
      check(r.ok && lit(r, 8, 32) && lit(r, 56, 32), "cull: strip triangles all face front after expansion"); }
    const std::vector<float> fan = {
         0.0f,  0.0f, 0.5f, 1, 1, 1,   // fan: rim ordered so all three fan
         0.8f,  0.0f, 0.5f, 1, 1, 1,   // triangles wind the same (front) way;
         0.0f, -0.8f, 0.5f, 1, 1, 1,   // fans keep one consistent winding
        -0.8f,  0.0f, 0.5f, 1, 1, 1,
         0.0f,  0.8f, 0.5f, 1, 1, 1,
    };
    { TopoScene s; s.mode_cntl_set = true; s.mode_cntl = 2; s.build(fan, {}, 5);    // fan + cull back
      auto r = s.run(vk);
      check(r.ok && lit(r, 40, 24) && lit(r, 24, 24) && lit(r, 24, 40),
            "cull: fan triangles all face front after expansion"); }
    { TopoScene s; s.mode_cntl_set = true; s.mode_cntl = 2; s.build(two, {0, 1, 2, 3, 4, 5}, 4);  // indexed
      auto r = s.run(vk);
      check(r.ok && lit(r, 16, 37) && !lit(r, 48, 37), "cull: indexed draws honour the cull state"); }
    // Textured path: the S17 full-screen quad winds screen-CW, so cull-front
    // keeps it and cull-back removes it.
    { TexScene t; t.mode_cntl_set = true; t.mode_cntl = 1; t.build(1.0f, 0, 0, 0);
      auto r = t.run(vk);
      check(r.ok && lit(r, 32, 32), "cull: textured quad survives cull-front"); }
    { TexScene t; t.mode_cntl_set = true; t.mode_cntl = 2; t.build(1.0f, 0, 0, 0);
      auto r = t.run(vk);
      check(r.ok && !lit(r, 32, 32), "cull: textured quad is removed by cull-back"); }
    // Refusals: named reasons for the unsupported PolygonControl fields.
    auto refused = [&](std::uint32_t mc, const char* what, const std::string& l) {
      TopoScene s; s.mode_cntl_set = true; s.mode_cntl = mc; s.build(two, {}, 4);
      auto r = s.run(vk);
      check(!r.ok && r.error.find(what) != std::string::npos, l);
    };
    refused((1u << 3) | (1u << 5), "polygon mode", "cull: line polygon mode refused");
    refused((1u << 3) | (2u << 5) | (1u << 8), "polygon mode", "cull: non-fill back polygon mode refused");
    refused(1u << 11, "polygon offset", "cull: polygon offset refused");
    refused(1u << 16, "window offset", "cull: window offset refused");
    refused(1u << 20, "perspective", "cull: perspective-correct disable refused");
    refused(1u << 21, "multi-primitive", "cull: multi-primitive index buffer refused");
  }
}
