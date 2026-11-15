// Slice S6 tests: translated perspective divide + viewport transform.
// The post-transform kernel (rdna_post_transform_kernel) runs rcp(w), three
// muls, three fma viewport transforms and a min/max depth clamp as restricted
// RDNA machine code, translated and executed on the real Vulkan backend
// (llvmpipe here) against a CPU float reference. The PVS2 draw path feeds
// the rasterizer ndc with w = 1; pixels are compared against the PVS1 path
// (GPU-side divide). No commercial shaders. GPL-2.0.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

#include "graphics/gfx_draw.hpp"
#include "graphics/gpu_va.hpp"
#include "graphics/pm4.hpp"
#include "graphics/rdna_vertex.hpp"
#include "graphics/vulkan.hpp"
#include "shader/rdna.hpp"
#include "shader/translate.hpp"

namespace {
using pnr::graphics::VulkanBackend;
using pnr::graphics::kRdnaVertexBase;

uint32_t f2u(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

struct DrawScene {
  pnr::graphics::GpuAddressSpace mem;
  std::vector<std::uint32_t> vs_blob, ps_blob{0xbf810000}, mat;
  std::vector<float> vb;
  std::vector<std::uint8_t> target;
  std::vector<std::uint32_t> cmd;
  static constexpr std::uint32_t W = 64, H = 64;
  void build(bool pvs2, const std::vector<uint32_t>& mat_words) {
    vs_blob = pvs2 ? pnr::graphics::make_pnr_vs2_blob(3) : pnr::graphics::make_pnr_vs_blob(3);
    mat = mat_words;
    vb = {-0.8f, -0.8f, 0.5f, 1, 0, 0,   0.8f, -0.6f, 0.5f, 1, 0, 0,   0, 0.9f, 0.5f, 1, 0, 0};
    target.assign(W * H * 4, 0xCD);
    mem.map_external(0x100000, vs_blob.data(), vs_blob.size() * 4, "vs");
    mem.map_external(0x200000, ps_blob.data(), 4, "ps");
    mem.map_external(0x300000, mat.data(), 64, "matrix");
    mem.map_external(0x400000, vb.data(), vb.size() * 4, "vertices");
    mem.map_external(0x500000, target.data(), target.size(), "target");
    pnr::graphics::pm4::Builder b;
    namespace pm4 = pnr::graphics::pm4;
    namespace amd = pnr::graphics::amd;
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_VS, {0x100000 >> 8, 0});
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_PS, {0x200000 >> 8, 0});
    b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_USER_DATA_VS_0, {0x300000, 0, 0x400000, 0});
    b.set_reg(pm4::SET_CONTEXT_REG, amd::PA_CL_VPORT_XSCALE,
              {f2u(W / 2.0f), f2u(W / 2.0f), f2u(-0.5f * float(H)), f2u(0.5f * float(H)), f2u(0.5f), f2u(0.5f)});
    b.set_context(amd::CB_COLOR0_BASE, 0x500000 >> 8).set_context(amd::CB_TARGET_MASK, 0xf);
    b.set_context(amd::PA_SC_SCREEN_SCISSOR_BR, (H << 16) | W);
    b.set_uconfig(amd::VGT_PRIMITIVE_TYPE, 4).num_instances(1).draw_auto(3);
    cmd = b.dwords();
  }
};
}  // namespace

void pnr_rdna_post_xform_tests(int& pass, int& fail) {
  auto check = [&](bool ok, const std::string& l) {
    if (ok) ++pass; else ++fail;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", l.c_str());
  };
  using namespace pnr::graphics;

  // ---- translate acceptance (host-only) ----
  const uint32_t N = 24;
  auto t = pnr::shader::translate_compute_words(rdna_post_ndc_kernel(1, N), kRdnaVertexBase, N * 5, N, 0);
  check(t.ok && t.host_float32 && t.required_uniform_words == 0 && t.local_size_x == N &&
        t.required_buffer_words == N * 5,
        "post-xform: ndc kernel translates (float32, 24 lanes, 120 buffer words)");
  auto tw = pnr::shader::translate_compute_words(rdna_post_win_kernel(2, N), kRdnaVertexBase, N * 5, N, 8);
  check(tw.ok && tw.host_float32 && tw.required_uniform_words == 8 && tw.local_size_x == N,
        "post-xform: window/depth kernel translates (float32, 8 uniform words)");
  {
    auto d = pnr::shader::rdna_decode(rdna_post_win_kernel(2, N));
    int have_rcp = 0, have_fma = 0, have_min = 0, have_max = 0;
    for (const auto& in : d.instructions) {
      have_rcp += in.mnemonic == "v_rcp_f32";
      have_fma += in.mnemonic == "v_fma_f32";
      have_min += in.mnemonic == "v_min_f32";
      have_max += in.mnemonic == "v_max_f32";
    }
    check(d.ok && have_rcp == 1 && have_fma == 1 && have_min == 1 && have_max == 1,
          "post-xform: decoder names rcp, fma, min and max in the depth kernel");
  }
  if (const char* val = std::getenv("PNR_SPIRV_VAL")) {
    const std::string tmp = "/tmp/pnr_postx_" + std::to_string(::getpid()) + ".spv";
    { std::ofstream o(tmp, std::ios::binary); o.write(reinterpret_cast<const char*>(tw.spirv.data()), tw.spirv.size() * 4); }
    const std::string cmd = std::string(val) + " " + tmp + " 2>&1";
    FILE* p = popen(cmd.c_str(), "r"); std::string out; char buf[256];
    while (p && fgets(buf, sizeof buf, p)) out += buf;
    const int rc = p ? pclose(p) : -1;
    check(rc == 0, "post-xform: spirv-val accepts the post-transform module");
    if (rc != 0) std::printf("    spirv-val said: %s\n", out.c_str());
    std::remove(tmp.c_str());
  }
  {
    auto b1 = make_pnr_vs_blob(3), b2 = make_pnr_vs2_blob(3);
    check(b2.size() > b1.size() && b2[0] == kPnrVs2Magic && b2[1] == 7, "post-xform: PVS2 blob has magic, count 7 (4 clip + 3 ndc kernels)");
  }

  // ---- execution on the real Vulkan backend (llvmpipe here) ----
  VulkanBackend vk;
  auto st = vk.open();
  if (!st) { std::printf("SKIP post-xform exec: %s\n", st.error.message.c_str()); return; }
  st = vk.create_instance("pnr-rdna-post-xform-tests");
  if (!st || vk.devices().empty()) { check(false, "post-xform: device available"); return; }
  st = vk.create_device(0);
  check(bool(st), "post-xform: device created");
  if (!st) return;

  // CPU reference vs translated kernel, bitwise. All inputs are exactly
  // representable; division and fma are correctly rounded on both sides.
  {
    const float vp[8] = {32.0f, 32.0f, -32.0f, 32.0f, 0.5f, 0.5f, 0.0f, 1.0f};
    std::vector<uint32_t> vp8(8);
    for (int k = 0; k < 8; ++k) vp8[k] = f2u(vp[k]);
    std::vector<float> clip(N * 4);
    for (uint32_t i = 0; i < N; ++i) {
      clip[i * 4 + 0] = float(int(i % 9) - 4) * 0.75f;   // -3 .. 3
      clip[i * 4 + 1] = float(int(i % 5) - 2) * 1.5f;    // -3 .. 3
      clip[i * 4 + 2] = float(int(i % 7) - 3) * 0.25f;   // -0.75 .. 0.75
      clip[i * 4 + 3] = 0.5f + float(i % 6) * 0.5f;      // 0.5 .. 3.0, never 0
    }
    std::vector<float> ndc, win;
    st = rdna_post_transform_clip(vk, clip, vp8, &ndc, &win);
    check(bool(st) && ndc.size() == N * 3 && win.size() == N * 3, "post-xform: 24 vertices post-transformed on the GPU");
    uint32_t bad = 0;
    if (st) {
      for (uint32_t i = 0; i < N; ++i) {
        const float inv = 1.0f / clip[i * 4 + 3];
        const float en[3] = {clip[i * 4 + 0] * inv, clip[i * 4 + 1] * inv, clip[i * 4 + 2] * inv};
        const float ew[3] = {std::fma(en[0], vp[0], vp[1]), std::fma(en[1], vp[2], vp[3]),
                             std::min(std::max(std::fma(en[2], vp[4], vp[5]), vp[6]), vp[7])};
        for (int k = 0; k < 3; ++k) {
          bad += f2u(en[k]) != f2u(ndc[i * 3 + k]);
          bad += f2u(ew[k]) != f2u(win[i * 3 + k]);
        }
      }
      std::printf("  post-xform: components differing from CPU reference = %u of %u\n", bad, N * 6);
      check(bad == 0, "post-xform: ndc + window/depth bitwise equal to CPU division/fma reference (0 ulp)");
    }
    // w = 0: IEEE propagation, no guard (documented).
    std::vector<float> z(4 * 3, 0.0f);
    z[0] = 1.5f; z[1] = 0.25f; z[2] = 0.5f; z[3] = 0.0f;
    std::vector<float> ndc0, win0;
    st = rdna_post_transform_clip(vk, z, vp8, &ndc0, &win0);
    check(bool(st) && std::isinf(ndc0[0]) && ndc0[0] > 0, "post-xform: w = 0 gives IEEE +inf on ndc_x (no guard, documented)");
  }

  // ---- PVS2 draw path vs PVS1 (translated divide vs GPU divide) ----
  {
    // Perspective matrix makes w = 3 for all three vertices (divide matters).
    std::vector<uint32_t> mat(16, 0);
    for (int i = 0; i < 4; ++i) mat[i * 5] = f2u(1.0f);
    mat[11] = 0; mat[14] = f2u(0.0f); mat[15] = 0; mat[3] = 0;
    // w_row: w = -z_view; vertices have z = 0.5 with a translate of -3.5 -> w = 3.
    mat[14] = f2u(0.0f); mat[11] = f2u(-1.0f); mat[15] = f2u(3.0f); mat[10] = f2u(1.0f);
    DrawScene s1, s2;
    s1.build(false, mat);
    s2.build(true, mat);
    auto sum1 = pnr::graphics::pm4::interpret(s1.cmd.data(), s1.cmd.size());
    auto sum2 = pnr::graphics::pm4::interpret(s2.cmd.data(), s2.cmd.size());
    auto r1 = execute_draw(vk, s1.mem, sum1.draws[0]);
    auto r2 = execute_draw(vk, s2.mem, sum2.draws[0]);
    if (!r1.ok) std::printf("  pvs1 draw error: %s\n", r1.error.c_str());
    if (!r2.ok) std::printf("  pvs2 draw error: %s\n", r2.error.c_str());
    check(r1.ok && r2.ok, "post-xform: PVS1 and PVS2 draws both succeed");
    if (r1.ok && r2.ok) {
      check(r2.notes.find("perspective divide") != std::string::npos,
            "post-xform: PVS2 result states the divide is translated RDNA");
      uint32_t d = 0;
      for (size_t p = 0; p < size_t(64) * 64; ++p) {
        bool df = false;
        for (int c = 0; c < 3; ++c) df |= r1.rgba[p * 4 + c] != r2.rgba[p * 4 + c];
        d += df;
      }
      std::printf("  post-xform: pixels differing PVS1 (GPU divide) vs PVS2 (translated divide) = %u of %u\n", d, 64 * 64);
      check(d == 0, "post-xform: PVS2 pixels identical to PVS1 on lavapipe");
      int reds = 0;
      for (size_t p = 0; p < size_t(64) * 64; ++p)
        reds += r2.rgba[p * 4 + 0] == 255 && r2.rgba[p * 4 + 1] == 0 && r2.rgba[p * 4 + 2] == 0;
      check(reds > 200, "post-xform: PVS2 triangle actually covers pixels (red count " + std::to_string(reds) + ")");
    }
  }
}
