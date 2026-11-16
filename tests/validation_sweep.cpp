// Slice S29: Vulkan validation-layer sweep. Runs the texture/draw paths PNR
// ships, ONCE each, under an explicitly requested VK_LAYER_KHRONOS_validation
// so any API-invalid usage is reported by the layer (stderr, "Validation
// Error"). Byte-exact lavapipe output alone cannot prove API validity - the
// software rasterizer tolerates wrong-but-plausible configuration (the S27
// view-type and S28 CUBE_COMPATIBLE enum bugs all rendered fine). This sweep
// is the runtime counterpoint to tests/vulkan_enum_check_tests.cpp (which
// covers constant values at compile time).
//
// Exit 0 with "SKIP" when no Vulkan device or the layer is not installed, so
// the ctest stays green on machines without the layer; CI installs
// vulkan-validationlayers so the sweep really runs there. The ctest
// pnr_vulkan_validation fails on any "Validation Error" in the output.
//
// Coverage (each line printed below): instance+device under the layer,
// untextured PVS1 execute_draw (base graphics pipeline + translated-RDNA
// compute vertex stage + colour-target copyback), plain 2D texture, mip-chain
// 2D texture, sRGB 2D texture, 2D array texture (2D_ARRAY view), cubemap
// (CUBE_COMPATIBLE image + CUBE view). NOT covered here: swapchain/present
// (window path, needs a display server), depth/stencil attachments, blending,
// multi-frame surfaces, non-default sampler filter/address modes.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <vector>

#include "graphics/demo/cliptex3_shaders.hpp"
#include "graphics/demo/cliptex_shaders.hpp"
#include "graphics/demo/tex_shaders.hpp"
#include "graphics/demo/texarr_shaders.hpp"
#include "graphics/demo/texcube_shaders.hpp"
#include "graphics/gfx_draw.hpp"
#include "graphics/gpu_va.hpp"
#include "graphics/pm4.hpp"
#include "graphics/vulkan.hpp"

namespace {
using namespace pnr::graphics;

uint32_t f2u(float v) { uint32_t r; std::memcpy(&r, &v, 4); return r; }

int g_failures = 0;

void report(bool ok, const char* name, const std::string& err) {
  if (ok) {
    std::printf("SWEEP %-22s ok\n", name);
  } else {
    ++g_failures;
    std::printf("SWEEP %-22s FAILED: %s\n", name, err.c_str());
  }
}

// Case 1: one untextured PVS1 triangle through the full PM4 -> execute_draw
// path (translated-RDNA compute vertex stage, base graphics pipeline, colour
// target copyback).
void sweep_untextured_draw(VulkanBackend& vk) {
  GpuAddressSpace mem;
  auto vs_blob = make_pnr_vs_blob(3);
  std::vector<std::uint32_t> ps_blob{0xbf810000};
  std::vector<std::uint32_t> mat(16, 0);
  for (int i = 0; i < 4; ++i) mat[i * 5] = f2u(1.0f);
  std::vector<float> vb = {-0.8f, -0.8f, 0.0f, 1.0f, 0.0f, 0.0f,
                           0.8f, -0.8f, 0.0f, 0.0f, 1.0f, 0.0f,
                           0.0f, 0.8f, 0.0f, 0.0f, 0.0f, 1.0f};
  constexpr std::uint32_t W = 64, H = 64;
  std::vector<std::uint8_t> target(W * H * 4, 0xCD);
  mem.map_external(0x100000, vs_blob.data(), vs_blob.size() * 4, "vs");
  mem.map_external(0x200000, ps_blob.data(), ps_blob.size() * 4, "ps");
  mem.map_external(0x300000, mat.data(), 64, "matrix");
  mem.map_external(0x400000, vb.data(), vb.size() * 4, "vertices");
  mem.map_external(0x500000, target.data(), target.size(), "target");
  pm4::Builder b;
  b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_VS, {0x100000 >> 8, 0});
  b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_PGM_LO_PS, {0x200000 >> 8, 0});
  b.set_reg(pm4::SET_SH_REG, amd::SPI_SHADER_USER_DATA_VS_0, {0x300000, 0, 0x400000, 0});
  b.set_reg(pm4::SET_CONTEXT_REG, amd::PA_CL_VPORT_XSCALE,
            {f2u(W / 2.0f), f2u(W / 2.0f), f2u(-0.5f * float(H)), f2u(0.5f * float(H)), f2u(0.5f), f2u(0.5f)});
  b.set_context(amd::CB_COLOR0_BASE, 0x500000 >> 8).set_context(amd::CB_TARGET_MASK, 0xf);
  b.set_context(amd::PA_SC_SCREEN_SCISSOR_BR, (H << 16) | W);
  b.set_uconfig(amd::VGT_PRIMITIVE_TYPE, 4).num_instances(1).draw_auto(3);
  auto cmd = b.dwords();
  auto sum = pm4::interpret(cmd.data(), cmd.size());
  if (sum.draws.size() != 1) { report(false, "untextured-pvs1-draw", "no draw parsed"); return; }
  auto r = execute_draw(vk, mem, sum.draws[0]);
  report(r.ok && r.wrote_target && r.rgba.size() == W * H * 4, "untextured-pvs1-draw", r.error);
}

// Fullscreen-quad vertex records, float4 clip + float3 colour + uv/layer/dir.
// Indices for the 6-vertex fullscreen quad (the clip-mesh APIs require an
// index list: a nonzero multiple of 3).
const std::vector<std::uint32_t> kQuadIndices{0, 1, 2, 3, 4, 5};

std::vector<float> quad9(float u, float v) {
  const float p[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
  const float t[6][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};
  std::vector<float> out;
  for (int i = 0; i < 6; ++i) {
    out.insert(out.end(), {p[i][0], p[i][1], 0.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                           t[i][0] * u, t[i][1] * v});
  }
  return out;
}

std::vector<float> quad10(float x, float y, float z) {
  const float p[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
  const float t[6][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};
  std::vector<float> out;
  for (int i = 0; i < 6; ++i) {
    out.insert(out.end(), {p[i][0], p[i][1], 0.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                           0.25f + t[i][0] * 0.5f, 0.25f + t[i][1] * 0.5f, z});
  }
  (void)x; (void)y;
  return out;
}

// Solid-colour texture data: pixels for `px` RGBA8 pixels in `count` slices.
std::vector<std::uint8_t> solid_tex(std::uint32_t px, std::uint32_t count,
                                    uint8_t r, uint8_t g, uint8_t b) {
  std::vector<std::uint8_t> d(px * count * 4);
  for (std::uint32_t i = 0; i < px * count; ++i) {
    d[i * 4] = r; d[i * 4 + 1] = g; d[i * 4 + 2] = b; d[i * 4 + 3] = 255;
  }
  return d;
}

void sweep_textured(VulkanBackend& vk) {
  constexpr std::uint32_t W = 32, H = 32;
  const float clear[4] = {0, 0, 0, 1};
  std::vector<std::uint8_t> img;
  const std::vector<std::uint32_t> vert(cliptex_vert, cliptex_vert + sizeof(cliptex_vert) / 4);
  const std::vector<std::uint32_t> frag(tex_frag, tex_frag + sizeof(tex_frag) / 4);

  // Plain 2D.
  {
    auto data = solid_tex(8 * 8, 1, 255, 0, 0);
    TexInput tex; tex.rgba = data.data(); tex.w = 8; tex.h = 8;
    auto vb = quad9(1.0f, 1.0f);
    auto st = vk.render_textured_clip_mesh_rgba(W, H, vert, frag, vb, kQuadIndices, false, tex, clear, &img);
    report(st.ok && img.size() == W * H * 4, "texture-2d", st.ok ? "" : st.error.message);
  }
  // Mip chain: level 0 (8x8) + level 1 (4x4), packed one after another.
  {
    auto l0 = solid_tex(8 * 8, 1, 255, 0, 0);
    auto l1 = solid_tex(4 * 4, 1, 0, 0, 255);
    l0.insert(l0.end(), l1.begin(), l1.end());
    TexInput tex; tex.rgba = l0.data(); tex.w = 8; tex.h = 8; tex.levels = 2;
    auto vb = quad9(1.0f, 1.0f);
    auto st = vk.render_textured_clip_mesh_rgba(W, H, vert, frag, vb, kQuadIndices, false, tex, clear, &img);
    report(st.ok && img.size() == W * H * 4, "texture-2d-mips", st.ok ? "" : st.error.message);
  }
  // sRGB-staged 2D.
  {
    auto data = solid_tex(8 * 8, 1, 128, 128, 128);
    TexInput tex; tex.rgba = data.data(); tex.w = 8; tex.h = 8; tex.srgb = true;
    auto vb = quad9(1.0f, 1.0f);
    auto st = vk.render_textured_clip_mesh_rgba(W, H, vert, frag, vb, kQuadIndices, false, tex, clear, &img);
    report(st.ok && img.size() == W * H * 4, "texture-2d-srgb", st.ok ? "" : st.error.message);
  }
}

void sweep_array_and_cube(VulkanBackend& vk) {
  constexpr std::uint32_t W = 32, H = 32;
  const float clear[4] = {0, 0, 0, 1};
  std::vector<std::uint8_t> img;
  const std::vector<std::uint32_t> vert(cliptex3_vert, cliptex3_vert + sizeof(cliptex3_vert) / 4);

  // 2D array: 4 slices, sample slice 2.
  {
    const std::vector<std::uint32_t> frag(texarr_frag, texarr_frag + sizeof(texarr_frag) / 4);
    auto data = solid_tex(8 * 8, 4, 0, 255, 0);
    TexInput tex; tex.rgba = data.data(); tex.w = 8; tex.h = 8; tex.layers = 4;
    auto vb = quad10(0.0f, 0.0f, 2.0f);
    auto st = vk.render_textured_array_clip_mesh_rgba(W, H, vert, frag, vb, kQuadIndices, false, tex, clear, &img);
    report(st.ok && img.size() == W * H * 4, "texture-2d-array", st.ok ? "" : st.error.message);
  }
  // Cube: 6 faces, direction +X.
  {
    const std::vector<std::uint32_t> frag(texcube_frag, texcube_frag + sizeof(texcube_frag) / 4);
    auto data = solid_tex(8 * 8, 6, 255, 0, 255);
    TexInput tex; tex.rgba = data.data(); tex.w = 8; tex.h = 8; tex.layers = 6; tex.cube = true;
    auto vb = quad10(0.0f, 0.0f, 1.0f);  // direction (0.25*, 0.25*, 1) after quad10 offsets
    auto st = vk.render_textured_array_clip_mesh_rgba(W, H, vert, frag, vb, kQuadIndices, false, tex, clear, &img);
    report(st.ok && img.size() == W * H * 4, "texture-cube", st.ok ? "" : st.error.message);
  }
}

}  // namespace

// The loader only WARNS and skips a missing env-forced layer, so check
// vkEnumerateInstanceLayerProperties directly before claiming coverage.
bool layer_available() {
  void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!lib) return false;
  auto enum_layers = reinterpret_cast<int32_t (*)(uint32_t*, void*)>(dlsym(lib, "vkEnumerateInstanceLayerProperties"));
  if (!enum_layers) { dlclose(lib); return false; }
  uint32_t n = 0;
  if (enum_layers(&n, nullptr) != 0 || n == 0) { dlclose(lib); return false; }
  // VkLayerProperties: char layerName[256], uint32 spec, impl; char desc[256]
  struct LayerProps { char name[256]; uint32_t spec, impl; char desc[256]; };
  std::vector<LayerProps> props(n);
  bool found = false;
  if (enum_layers(&n, props.data()) == 0)
    for (const auto& p : props)
      if (std::strcmp(p.name, "VK_LAYER_KHRONOS_validation") == 0) found = true;
  dlclose(lib);
  return found;
}

int main() {
  if (!layer_available()) {
    std::printf("SKIP validation sweep: VK_LAYER_KHRONOS_validation not installed\n");
    return 0;
  }
  setenv("VK_INSTANCE_LAYERS", "VK_LAYER_KHRONOS_validation", 1);
  VulkanBackend vk;
  if (!vk.open()) { std::printf("SKIP validation sweep: no Vulkan loader\n"); return 0; }
  auto st = vk.create_instance("pnr-validation-sweep");
  if (!st || vk.devices().empty()) {
    std::printf("SKIP validation sweep: no Vulkan device\n");
    return 0;
  }
  st = vk.create_device(0);
  if (!st) { std::printf("SKIP validation sweep: device creation failed\n"); return 0; }
  std::printf("VALIDATION SWEEP: running under VK_LAYER_KHRONOS_validation (explicit request)\n");

  sweep_untextured_draw(vk);
  sweep_textured(vk);
  sweep_array_and_cube(vk);

  std::printf("VALIDATION SWEEP COMPLETE: %d path(s) failed\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
