// Core slices C4d/C4e tests: SYNTHETIC GNM command buffer -> one Vulkan draw -> pixel check.
// Skips (with a message) when no Vulkan device exists. No real game data is involved.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "graphics/gfx_draw.hpp"
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
  { Scene s; s.build(64, 5); refused(s, "triangle lists", "draw: triangle fan refused"); }
  { Scene s; s.build(32); refused(s, "viewport", "draw: viewport that does not match the target is refused"); }
  { Scene s; s.build(64, 4, false); refused(s, "user data", "draw: missing vertex user data refused"); }
  { Scene s; s.build(); s.vs_blob[0] = 0; refused(s, "PVS1", "draw: a vertex program that is not a PVS1 blob is refused"); }
  { Scene s; s.build(); s.mem.unmap(0x400000); refused(s, "vertex buffer is not mapped", "draw: unmapped vertex buffer refused"); }
  { Scene s; s.build(); s.mem.unmap(0x500000); auto sum = pm4::interpret(s.cmd.data(), s.cmd.size()); auto r = execute_draw(vk, s.mem, sum.draws[0]);
    check(r.ok && !r.wrote_target && r.notes.find("not mapped") != std::string::npos, "draw: unmapped colour target returns pixels only and says so"); }
}
