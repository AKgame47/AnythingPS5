// PNR PS-ABI shim - the in-process binding layer (v1)
// A shared library exporting the sce* symbols a PS-style binary imports.
// At launch the runtime points the game at this library (LD_LIBRARY_PATH)
// and at its jail (PNR_JAIL_ROOT); calls then do real work natively in the
// game process: kernel-style services over host equivalents, file calls
// through the SAME jailed FileSystemService as the host-side PsFileTable.
// (Application-level path routing, not a security boundary - see
// compat/services.hpp.)
// This is how the registry's prx-implemented promises get kept.
// Taxonomy per AnyPS5 (GPL-2.0); implementation original. Status: Implemented, Tested.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <filesystem>
#include <thread>
#include <vector>

#include <sys/mman.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "compat/services.hpp"
#include "graphics/demo/cube_shaders.hpp"
#include "graphics/demo/spin_shaders.hpp"
#include "graphics/demo/clip_shaders.hpp"
#include "graphics/demo/lit_shaders.hpp"
#include "graphics/demo/tex_shaders.hpp"
#include "graphics/demo/littex_shaders.hpp"
#include "graphics/mat4.hpp"
#include "graphics/rdna_vertex.hpp"
#include "graphics/vulkan.hpp"

namespace {
pnr::compat::FileSystemService& jail() {
  static pnr::compat::FileSystemService fs([] {
    const char* r = std::getenv("PNR_JAIL_ROOT");
    return std::string(r != nullptr ? r : "/tmp/pnr_psabi_default_jail");
  }());
  return fs;
}
struct Mapping { void* start; std::size_t size; };
std::mutex g_mu;
std::vector<Mapping>& maps() { static std::vector<Mapping> m; return m; }

// ---- PNR graphics extension (NOT Sony API names) ------------------------
// pnrGfx* calls give a game an offscreen, headless 3D cube renderer backed
// by the Vulkan backend. "Present" writes a PPM into the game's jail; there
// is no swapchain or display. Status: Implemented, Tested on software Vulkan.
struct Gfx {
  pnr::graphics::VulkanBackend vk;
  bool up = false;
  uint32_t w = 0, h = 0, frames = 0, rdna_frames = 0;
  std::vector<float> verts, xyz, col;
  std::vector<uint32_t> idx, vs, fs, cvs;
  // game-uploaded mesh (pnrGfxSetMesh)
  std::vector<float> m_verts, m_xyz, m_col;
  std::vector<uint32_t> m_idx;
  uint32_t mesh_draws = 0;
  // game-uploaded lit mesh (pnrGfxSetLitMesh) and light (pnrGfxSetLight)
  std::vector<float> l_verts, l_xyz, l_col, l_nrm;
  std::vector<uint32_t> l_idx, lvs_hand, lvs_clip, lfs;
  float light[3] = {0.3f, 0.5f, 0.8124f};  // toward the light, view space
  uint32_t lit_draws = 0;
  // game-uploaded texture (pnrGfxSetTexture) and textured mesh
  std::vector<uint8_t> t_rgba;
  uint32_t t_w = 0, t_h = 0;
  bool t_linear = false, t_repeat = false;
  std::vector<float> x_verts;
  std::vector<uint32_t> x_idx, tvs, tfs;
  uint32_t tex_draws = 0;
  std::vector<float> lx_verts;
  std::vector<uint32_t> lx_idx, ltvs, ltfs;
  uint32_t littex_draws = 0;
  std::vector<uint8_t> last;
  std::string error;
};
Gfx& gfx() {
  static Gfx* g = new Gfx();  // intentionally leaked: no driver teardown at exit
  return *g;
}
}  // namespace

extern "C" {

const char* pnrGfxLastError() { return gfx().error.c_str(); }

std::int32_t pnrGfxInit(std::uint32_t w, std::uint32_t h) {
  Gfx& g = gfx();
  if (g.up) return 0;
  if (w == 0 || h == 0 || w > 1024 || h > 1024) {
    g.error = "pnrGfxInit: size must be 1..1024";
    return -2;
  }
  if (auto st = g.vk.open(); !st) { g.error = st.error.message; return -10; }
  if (auto st = g.vk.create_instance("pnr-game"); !st) { g.error = st.error.message; return -11; }
  if (g.vk.devices().empty()) { g.error = "no Vulkan device"; return -11; }
  if (auto st = g.vk.create_device(0); !st) { g.error = st.error.message; return -12; }
  pnr::graphics::cube_mesh(&g.verts, &g.idx);
  for (size_t i = 0; i + 5 < g.verts.size(); i += 6)
    for (int k = 0; k < 3; ++k) { g.xyz.push_back(g.verts[i + k]); g.col.push_back(g.verts[i + 3 + k]); }
  g.cvs.assign(std::begin(clip_vert), std::end(clip_vert));
  g.lvs_hand.assign(std::begin(lit_hand_vert), std::end(lit_hand_vert));
  g.lvs_clip.assign(std::begin(lit_clip_vert), std::end(lit_clip_vert));
  g.lfs.assign(std::begin(lit_frag), std::end(lit_frag));
  g.ltvs.assign(std::begin(littex_vert), std::end(littex_vert));
  g.ltfs.assign(std::begin(littex_frag), std::end(littex_frag));
  g.tvs.assign(std::begin(tex_vert), std::end(tex_vert));
  g.tfs.assign(std::begin(tex_frag), std::end(tex_frag));
  g.vs.assign(std::begin(cube_vert), std::end(cube_vert));
  g.fs.assign(std::begin(spin_frag), std::end(spin_frag));
  g.w = w; g.h = h; g.up = true;
  return 0;
}

}  // extern "C"

namespace {
std::int32_t coverage(const std::vector<uint8_t>& img) {
  std::int32_t cov = 0;
  for (size_t i = 0; i + 3 < img.size(); i += 4)
    if (img[i] | img[i + 1] | img[i + 2]) ++cov;
  return cov;
}
pnr::graphics::Mat4 cube_mvp(const Gfx& g, float yaw, float pitch) {
  using namespace pnr::graphics;
  const Mat4 P = mat4_perspective(1.0471976f, (float)g.w / (float)g.h, 0.1f, 10.0f);
  const Mat4 T = mat4_translate(0, 0, -3);
  const Mat4 M = mat4_mul(mat4_rotate_x(pitch), mat4_rotate_y(yaw));
  return mat4_mul(P, mat4_mul(T, M));
}
}  // namespace


namespace {
// Shared draw helpers: hand-authored MVP vertex shader, or translated RDNA
// compute-as-vertex kernel + pass-through shader. Both fill g.last.
std::int32_t draw_hand(Gfx& g, const std::vector<float>& verts, const std::vector<uint32_t>& idx,
                       const pnr::graphics::Mat4& mvp) {
  using namespace pnr::graphics;
  std::vector<std::vector<uint32_t>> push{mat4_words(mvp)};
  const float black[4] = {0, 0, 0, 1};
  std::vector<std::vector<uint8_t>> out;
  auto st = g.vk.render_mesh_frames_rgba(g.w, g.h, g.vs, g.fs, verts, idx, true, push, black, &out);
  if (!st || out.size() != 1) { g.error = st ? "no frame" : st.error.message; return -3; }
  g.last = std::move(out[0]);
  ++g.frames;
  return coverage(g.last);
}
std::int32_t draw_rdna(Gfx& g, const std::vector<float>& xyz, const std::vector<float>& col,
                       const std::vector<uint32_t>& idx, const pnr::graphics::Mat4& mvp) {
  using namespace pnr::graphics;
  std::vector<float> clip;
  auto st = rdna_transform_points(g.vk, xyz, mat4_words(mvp), &clip);
  if (!st) { g.error = st.error.message; return -4; }
  const size_t n = xyz.size() / 3;
  std::vector<float> cv;
  for (size_t i = 0; i < n; ++i) {
    for (int c = 0; c < 4; ++c) cv.push_back(clip[i * 4 + c]);
    for (int c = 0; c < 3; ++c) cv.push_back(col[i * 3 + c]);
  }
  const float black[4] = {0, 0, 0, 1};
  std::vector<uint8_t> img;
  st = g.vk.render_clip_mesh_rgba(g.w, g.h, g.cvs, g.fs, cv, idx, true, black, &img);
  if (!st) { g.error = st.error.message; return -3; }
  g.last = std::move(img);
  ++g.frames; ++g.rdna_frames;
  return coverage(g.last);
}
}  // namespace

extern "C" {

// Renders one frame of the built-in unit cube rotated by pitch (X) then yaw
// (Y) with the hand-authored MVP vertex shader. Returns covered pixels or <0.
std::int32_t pnrGfxDrawCube(float yaw, float pitch) {
  Gfx& g = gfx();
  if (!g.up) { g.error = "pnrGfxInit first"; return -1; }
  return draw_hand(g, g.verts, g.idx, cube_mvp(g, yaw, pitch));
}

// Same cube, but the vertex stage is a translated RDNA-style compute kernel
// (compute-as-vertex-stage, PNR test ABI, NOT a real vertex-stage ABI): four
// dispatches produce clip-space positions that the draw path consumes
// through a pass-through vertex shader. Returns covered pixels or negative.
std::int32_t pnrGfxDrawCubeRdna(float yaw, float pitch) {
  Gfx& g = gfx();
  if (!g.up) { g.error = "pnrGfxInit first"; return -1; }
  return draw_rdna(g, g.xyz, g.col, g.idx, cube_mvp(g, yaw, pitch));
}

// Upload a game-supplied mesh: vertices are 6 floats each (x,y,z,r,g,b),
// indices a triangle list. Bounded: 1..24 vertices (the RDNA kernel's lane
// count), 3..384 indices, finite floats, every index < vertex count. The data
// is COPIED; on any error the previously uploaded mesh is kept.
// Returns 0, or -1 not initialised, -6 invalid arguments (see pnrGfxLastError).
std::int32_t pnrGfxSetMesh(const float* verts, std::uint32_t vertex_count,
                           const std::uint32_t* indices, std::uint32_t index_count) {
  Gfx& g = gfx();
  if (!g.up) { g.error = "pnrGfxInit first"; return -1; }
  auto bad = [&](const char* why) { g.error = std::string("pnrGfxSetMesh: ") + why; return -6; };
  if (verts == nullptr || indices == nullptr) return bad("null pointer");
  if (vertex_count == 0 || vertex_count > 24) return bad("vertex count must be 1..24");
  if (index_count < 3 || index_count > 384 || index_count % 3) return bad("index count must be a multiple of 3 in 3..384");
  for (std::uint32_t i = 0; i < vertex_count * 6; ++i)
    if (!std::isfinite(verts[i])) return bad("non-finite vertex float");
  for (std::uint32_t i = 0; i < index_count; ++i)
    if (indices[i] >= vertex_count) return bad("index out of range");
  g.m_verts.assign(verts, verts + vertex_count * 6);
  g.m_idx.assign(indices, indices + index_count);
  g.m_xyz.clear(); g.m_col.clear();
  for (std::uint32_t i = 0; i < vertex_count; ++i)
    for (int k = 0; k < 3; ++k) { g.m_xyz.push_back(verts[i * 6 + k]); g.m_col.push_back(verts[i * 6 + 3 + k]); }
  return 0;
}

// Draw the uploaded mesh rotated by pitch (X) then yaw (Y). mode 0 = hand-
// authored MVP vertex shader, mode 1 = translated-RDNA compute-as-vertex path.
// Returns covered pixels or negative (-7 no mesh uploaded, -6 bad argument).
std::int32_t pnrGfxDrawMesh(float yaw, float pitch, std::int32_t mode) {
  Gfx& g = gfx();
  if (!g.up) { g.error = "pnrGfxInit first"; return -1; }
  if (g.m_idx.empty()) { g.error = "pnrGfxSetMesh first"; return -7; }
  if (mode != 0 && mode != 1) { g.error = "pnrGfxDrawMesh: mode must be 0 or 1"; return -6; }
  if (!std::isfinite(yaw) || !std::isfinite(pitch)) { g.error = "pnrGfxDrawMesh: non-finite angle"; return -6; }
  const auto mvp = cube_mvp(g, yaw, pitch);
  const std::int32_t r = mode == 0 ? draw_hand(g, g.m_verts, g.m_idx, mvp) : draw_rdna(g, g.m_xyz, g.m_col, g.m_idx, mvp);
  if (r >= 0) ++g.mesh_draws;
  return r;
}

std::uint32_t pnrGfxMeshDraws() { return gfx().mesh_draws; }

// ---- lit mesh (per-vertex normals + one directional light) -------------
// Lambert: rgb = colour * (0.25 + 0.75 * max(d, 0)), d = dot(N, L) per vertex
// (Gouraud: d is interpolated). mode 0: the hand-authored vertex shader
// computes d; mode 1: the translated-RDNA dot3 kernel computes d and the
// clip-space positions. The clamp/ambient live in a hand-authored fragment
// shader (max() is outside the translated subset). Float policy: unfused
// binary32 add/mul on the RDNA path, fused/unfused unspecified in the
// hand-authored path, so results agree within rounding, not bit-for-bit.

// Upload: 9 floats per vertex (x,y,z,r,g,b,nx,ny,nz), 1..24 vertices, 3..384
// indices (multiple of 3), all finite, indices in range, each normal of unit
// length within 1% (the shim never silently renormalises). Copied; a failed
// upload keeps the previous lit mesh. Returns 0 / -1 / -6.
std::int32_t pnrGfxSetLitMesh(const float* verts, std::uint32_t vertex_count,
                              const std::uint32_t* indices, std::uint32_t index_count) {
  Gfx& g = gfx();
  if (!g.up) { g.error = "pnrGfxInit first"; return -1; }
  auto bad = [&](const char* why) { g.error = std::string("pnrGfxSetLitMesh: ") + why; return -6; };
  if (verts == nullptr || indices == nullptr) return bad("null pointer");
  if (vertex_count == 0 || vertex_count > 24) return bad("vertex count must be 1..24");
  if (index_count < 3 || index_count > 384 || index_count % 3) return bad("index count must be a multiple of 3 in 3..384");
  for (std::uint32_t i = 0; i < vertex_count * 9; ++i)
    if (!std::isfinite(verts[i])) return bad("non-finite vertex float");
  for (std::uint32_t i = 0; i < vertex_count; ++i) {
    const float* n = verts + i * 9 + 6;
    const double len = std::sqrt((double)n[0] * n[0] + (double)n[1] * n[1] + (double)n[2] * n[2]);
    if (len < 0.99 || len > 1.01) return bad("normal is not unit length (within 1%)");
  }
  for (std::uint32_t i = 0; i < index_count; ++i)
    if (indices[i] >= vertex_count) return bad("index out of range");
  g.l_verts.assign(verts, verts + vertex_count * 9);
  g.l_idx.assign(indices, indices + index_count);
  g.l_xyz.clear(); g.l_col.clear(); g.l_nrm.clear();
  for (std::uint32_t i = 0; i < vertex_count; ++i)
    for (int k = 0; k < 3; ++k) {
      g.l_xyz.push_back(verts[i * 9 + k]);
      g.l_col.push_back(verts[i * 9 + 3 + k]);
      g.l_nrm.push_back(verts[i * 9 + 6 + k]);
    }
  return 0;
}

// Direction TOWARD the light in view space (x right, y up on screen is -y in
// Vulkan clip space but this is plain view space: +z toward the camera).
// Normalised by the shim; non-finite or near-zero is rejected with -6.
std::int32_t pnrGfxSetLight(float x, float y, float z) {
  Gfx& g = gfx();
  if (!g.up) { g.error = "pnrGfxInit first"; return -1; }
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) { g.error = "pnrGfxSetLight: non-finite"; return -6; }
  const double len = std::sqrt((double)x * x + (double)y * y + (double)z * z);
  if (len < 1e-6) { g.error = "pnrGfxSetLight: zero-length direction"; return -6; }
  g.light[0] = float(x / len); g.light[1] = float(y / len); g.light[2] = float(z / len);
  return 0;
}

// Draw the lit mesh rotated by pitch (X) then yaw (Y); mode 0 hand-authored,
// mode 1 translated RDNA. Returns covered pixels, -7 no lit mesh, -6 bad arg.
std::int32_t pnrGfxDrawLitMesh(float yaw, float pitch, std::int32_t mode) {
  Gfx& g = gfx();
  if (!g.up) { g.error = "pnrGfxInit first"; return -1; }
  if (g.l_idx.empty()) { g.error = "pnrGfxSetLitMesh first"; return -7; }
  if (mode != 0 && mode != 1) { g.error = "pnrGfxDrawLitMesh: mode must be 0 or 1"; return -6; }
  if (!std::isfinite(yaw) || !std::isfinite(pitch)) { g.error = "pnrGfxDrawLitMesh: non-finite angle"; return -6; }
  using namespace pnr::graphics;
  const Mat4 P = mat4_perspective(1.0471976f, (float)g.w / (float)g.h, 0.1f, 10.0f);
  const Mat4 T = mat4_translate(0, 0, -3);
  const Mat4 M = mat4_mul(mat4_rotate_x(pitch), mat4_rotate_y(yaw));
  const Mat4 mvp = mat4_mul(P, mat4_mul(T, M));
  float lobj[3];  // R^T * L: the view-space light in the mesh's object space
  for (int c = 0; c < 3; ++c) lobj[c] = M[c * 4 + 0] * g.light[0] + M[c * 4 + 1] * g.light[1] + M[c * 4 + 2] * g.light[2];
  const float black[4] = {0, 0, 0, 1};
  std::int32_t r;
  if (mode == 0) {
    auto words = mat4_words(mvp);
    for (int k = 0; k < 3; ++k) { std::uint32_t u; std::memcpy(&u, &lobj[k], 4); words.push_back(u); }
    words.push_back(0);
    std::vector<std::vector<std::uint32_t>> push{words};
    std::vector<std::vector<std::uint8_t>> out;
    auto st = g.vk.render_lit_mesh_frames_rgba(g.w, g.h, g.lvs_hand, g.lfs, g.l_verts, g.l_idx, true, push, black, &out);
    if (!st || out.size() != 1) { g.error = st ? "no frame" : st.error.message; return -3; }
    g.last = std::move(out[0]);
    ++g.frames;
    r = coverage(g.last);
  } else {
    std::vector<float> clip, d;
    auto st = rdna_transform_points(g.vk, g.l_xyz, mat4_words(mvp), &clip);
    if (!st) { g.error = st.error.message; return -4; }
    st = rdna_light_dots(g.vk, g.l_nrm, lobj, &d);
    if (!st) { g.error = st.error.message; return -4; }
    std::vector<float> cv;
    for (size_t i = 0; i < g.l_xyz.size() / 3; ++i) {
      for (int c = 0; c < 4; ++c) cv.push_back(clip[i * 4 + c]);
      for (int c = 0; c < 3; ++c) cv.push_back(g.l_col[i * 3 + c]);
      cv.push_back(d[i]);
    }
    std::vector<std::uint8_t> img;
    st = g.vk.render_lit_clip_mesh_rgba(g.w, g.h, g.lvs_clip, g.lfs, cv, g.l_idx, true, black, &img);
    if (!st) { g.error = st.error.message; return -3; }
    g.last = std::move(img);
    ++g.frames; ++g.rdna_frames;
    r = coverage(g.last);
  }
  if (r >= 0) ++g.lit_draws;
  return r;
}

std::uint32_t pnrGfxLitDraws() { return gfx().lit_draws; }

// ---- textured mesh ------------------------------------------------------
// One RGBA8 texture (row 0 on top, no mipmaps) sampled by a hand-authored
// fragment shader: rgb = texel.rgb * vertex colour, alpha 1. Filtering and
// addressing are the host Vulkan sampler's (nearest/linear, repeat/clamp);
// results are only verified against a CPU reference within tolerance on
// software Vulkan. The transform is the hand-authored vertex stage only.

// flags: bit 0 = linear filter (else nearest), bit 1 = repeat (else clamp to
// edge); other bits are refused. 1..256 per side, rgba is w*h*4 bytes, copied.
// A failed call keeps the previous texture. Returns 0 / -1 / -6.
std::int32_t pnrGfxSetTexture(const std::uint8_t* rgba, std::uint32_t w, std::uint32_t h,
                              std::uint32_t flags) {
  Gfx& g = gfx();
  if (!g.up) { g.error = "pnrGfxInit first"; return -1; }
  auto bad = [&](const char* why) { g.error = std::string("pnrGfxSetTexture: ") + why; return -6; };
  if (rgba == nullptr) return bad("null pixel pointer");
  if (w == 0 || h == 0 || w > 256 || h > 256) return bad("size must be 1..256 per side");
  if (flags & ~3u) return bad("unknown flag bits (only 1 = linear, 2 = repeat)");
  g.t_rgba.assign(rgba, rgba + (std::size_t)w * h * 4);
  g.t_w = w; g.t_h = h; g.t_linear = flags & 1; g.t_repeat = (flags & 2) != 0;
  return 0;
}

// Upload: 8 floats per vertex (x,y,z, r,g,b tint, u,v), 1..64 vertices,
// 3..384 indices (multiple of 3), all finite, indices in range. uv may lie
// outside 0..1 (the sampler's address mode decides). Returns 0 / -1 / -6.
std::int32_t pnrGfxSetTexturedMesh(const float* verts, std::uint32_t vertex_count,
                                   const std::uint32_t* indices, std::uint32_t index_count) {
  Gfx& g = gfx();
  if (!g.up) { g.error = "pnrGfxInit first"; return -1; }
  auto bad = [&](const char* why) { g.error = std::string("pnrGfxSetTexturedMesh: ") + why; return -6; };
  if (verts == nullptr || indices == nullptr) return bad("null pointer");
  if (vertex_count == 0 || vertex_count > 64) return bad("vertex count must be 1..64");
  if (index_count < 3 || index_count > 384 || index_count % 3) return bad("index count must be a multiple of 3 in 3..384");
  for (std::uint32_t i = 0; i < vertex_count * 8; ++i)
    if (!std::isfinite(verts[i])) return bad("non-finite vertex float");
  for (std::uint32_t i = 0; i < index_count; ++i)
    if (indices[i] >= vertex_count) return bad("index out of range");
  g.x_verts.assign(verts, verts + vertex_count * 8);
  g.x_idx.assign(indices, indices + index_count);
  return 0;
}

// Draw the textured mesh rotated by pitch (X) then yaw (Y), 60 deg
// perspective, 3 units back. Returns covered pixels, -7 no mesh, -8 no
// texture, -6 bad angle, -3 render failure.
std::int32_t pnrGfxDrawTexturedMesh(float yaw, float pitch) {
  Gfx& g = gfx();
  if (!g.up) { g.error = "pnrGfxInit first"; return -1; }
  if (g.x_idx.empty()) { g.error = "pnrGfxSetTexturedMesh first"; return -7; }
  if (g.t_rgba.empty()) { g.error = "pnrGfxSetTexture first"; return -8; }
  if (!std::isfinite(yaw) || !std::isfinite(pitch)) { g.error = "pnrGfxDrawTexturedMesh: non-finite angle"; return -6; }
  using namespace pnr::graphics;
  const Mat4 P = mat4_perspective(1.0471976f, (float)g.w / (float)g.h, 0.1f, 10.0f);
  const Mat4 T = mat4_translate(0, 0, -3);
  const Mat4 M = mat4_mul(mat4_rotate_x(pitch), mat4_rotate_y(yaw));
  std::vector<std::vector<std::uint32_t>> push{mat4_words(mat4_mul(P, mat4_mul(T, M)))};
  TexInput tex;
  tex.rgba = g.t_rgba.data(); tex.w = g.t_w; tex.h = g.t_h; tex.linear = g.t_linear; tex.repeat = g.t_repeat;
  const float black[4] = {0, 0, 0, 1};
  std::vector<std::vector<std::uint8_t>> out;
  auto st = g.vk.render_textured_mesh_frames_rgba(g.w, g.h, g.tvs, g.tfs, g.x_verts, g.x_idx, true, tex, push, black, &out);
  if (!st || out.size() != 1) { g.error = st ? "no frame" : st.error.message; return -3; }
  g.last = std::move(out[0]);
  ++g.frames;
  const std::int32_t r = coverage(g.last);
  if (r >= 0) ++g.tex_draws;
  return r;
}

std::uint32_t pnrGfxTexturedDraws() { return gfx().tex_draws; }

// ---- lit + textured mesh -------------------------------------------------
// Combines the lit path (pnrGfxSetLight, Lambert ambient 0.25, per-vertex d)
// with the texture (pnrGfxSetTexture): rgb = texel.rgb * tint * (0.25 +
// 0.75 * max(d, 0)). Hand-authored vertex + fragment shaders only (no RDNA
// path for this combination).

// 11 floats per vertex (x y z, r g b tint, u v, nx ny nz), 1..64 vertices,
// 3..384 indices, finite, indices in range, unit normals within 1%.
std::int32_t pnrGfxSetLitTexturedMesh(const float* verts, std::uint32_t vertex_count,
                                      const std::uint32_t* indices, std::uint32_t index_count) {
  Gfx& g = gfx();
  if (!g.up) { g.error = "pnrGfxInit first"; return -1; }
  auto bad = [&](const char* why) { g.error = std::string("pnrGfxSetLitTexturedMesh: ") + why; return -6; };
  if (verts == nullptr || indices == nullptr) return bad("null pointer");
  if (vertex_count == 0 || vertex_count > 64) return bad("vertex count must be 1..64");
  if (index_count < 3 || index_count > 384 || index_count % 3) return bad("index count must be a multiple of 3 in 3..384");
  for (std::uint32_t i = 0; i < vertex_count * 11; ++i)
    if (!std::isfinite(verts[i])) return bad("non-finite vertex float");
  for (std::uint32_t i = 0; i < vertex_count; ++i) {
    const float* n = verts + i * 11 + 8;
    const double len = std::sqrt((double)n[0] * n[0] + (double)n[1] * n[1] + (double)n[2] * n[2]);
    if (len < 0.99 || len > 1.01) return bad("normal is not unit length (within 1%)");
  }
  for (std::uint32_t i = 0; i < index_count; ++i)
    if (indices[i] >= vertex_count) return bad("index out of range");
  g.lx_verts.assign(verts, verts + vertex_count * 11);
  g.lx_idx.assign(indices, indices + index_count);
  return 0;
}

// Returns covered pixels, -7 no mesh, -8 no texture, -6 bad angle, -3 render failure.
std::int32_t pnrGfxDrawLitTexturedMesh(float yaw, float pitch) {
  Gfx& g = gfx();
  if (!g.up) { g.error = "pnrGfxInit first"; return -1; }
  if (g.lx_idx.empty()) { g.error = "pnrGfxSetLitTexturedMesh first"; return -7; }
  if (g.t_rgba.empty()) { g.error = "pnrGfxSetTexture first"; return -8; }
  if (!std::isfinite(yaw) || !std::isfinite(pitch)) { g.error = "pnrGfxDrawLitTexturedMesh: non-finite angle"; return -6; }
  using namespace pnr::graphics;
  const Mat4 P = mat4_perspective(1.0471976f, (float)g.w / (float)g.h, 0.1f, 10.0f);
  const Mat4 T = mat4_translate(0, 0, -3);
  const Mat4 M = mat4_mul(mat4_rotate_x(pitch), mat4_rotate_y(yaw));
  const Mat4 mvp = mat4_mul(P, mat4_mul(T, M));
  float lobj[3];
  for (int c = 0; c < 3; ++c) lobj[c] = M[c * 4 + 0] * g.light[0] + M[c * 4 + 1] * g.light[1] + M[c * 4 + 2] * g.light[2];
  auto words = mat4_words(mvp);
  for (int k = 0; k < 3; ++k) { std::uint32_t u; std::memcpy(&u, &lobj[k], 4); words.push_back(u); }
  words.push_back(0);
  std::vector<std::vector<std::uint32_t>> push{words};
  TexInput tex;
  tex.rgba = g.t_rgba.data(); tex.w = g.t_w; tex.h = g.t_h; tex.linear = g.t_linear; tex.repeat = g.t_repeat;
  const float black[4] = {0, 0, 0, 1};
  std::vector<std::vector<std::uint8_t>> out;
  auto st = g.vk.render_lit_textured_mesh_frames_rgba(g.w, g.h, g.ltvs, g.ltfs, g.lx_verts, g.lx_idx, true, tex, push, black, &out);
  if (!st || out.size() != 1) { g.error = st ? "no frame" : st.error.message; return -3; }
  g.last = std::move(out[0]);
  ++g.frames;
  const std::int32_t r = coverage(g.last);
  if (r >= 0) ++g.littex_draws;
  return r;
}

std::uint32_t pnrGfxLitTexturedDraws() { return gfx().littex_draws; }


std::uint32_t pnrGfxRdnaFramesRendered() { return gfx().rdna_frames; }

// "Present": write the last frame as a binary PPM at a jail path. Returns
// bytes written or a negative error. No display is involved.
std::int64_t pnrGfxPresentToFile(const char* path) {
  Gfx& g = gfx();
  std::string host;
  if (!g.up || g.last.empty()) { g.error = "nothing rendered yet"; return -1; }
  if (path == nullptr || !jail().resolve(path, host)) { g.error = "bad jail path"; return -2; }
  const auto slash = host.find_last_of('/');
  if (slash != std::string::npos && slash > 0) {
    std::error_code ec;
    std::filesystem::create_directories(host.substr(0, slash), ec);
    if (ec) { g.error = "cannot create jail directory"; return -3; }
  }
  std::string data = "P6\n" + std::to_string(g.w) + " " + std::to_string(g.h) + "\n255\n";
  for (size_t i = 0; i + 3 < g.last.size(); i += 4) data.append(reinterpret_cast<const char*>(&g.last[i]), 3);
  FILE* f = std::fopen(host.c_str(), "wb");
  if (!f) { g.error = "cannot open output"; return -3; }
  const size_t n = std::fwrite(data.data(), 1, data.size(), f);
  std::fclose(f);
  return n == data.size() ? (std::int64_t)n : -3;
}

std::uint32_t pnrGfxFramesRendered() { return gfx().frames; }



void* sceKernelAlloc(std::size_t size) {
  return size == 0 ? nullptr : std::malloc(size);
}
std::int32_t sceKernelFree(void* p) { std::free(p); return 0; }

std::int32_t sceKernelUsleep(std::uint32_t us) {
  if (us > 60u * 1000u * 1000u) return -2;
  std::this_thread::sleep_for(std::chrono::microseconds(us));
  return 0;
}
std::int32_t sceKernelSleep(std::uint32_t s) {
  std::this_thread::sleep_for(std::chrono::seconds(s));
  return 0;
}
std::int32_t sceKernelCreateThread(const char*, void (*entry)(void*), void* arg) {
  if (entry == nullptr) return -2;
  try { std::thread(entry, arg).detach(); } catch (...) { return -3; }
  return 0;
}

void* sceKernelMapMemory(std::size_t size, std::int64_t prot) {
  if (size == 0 || prot < 0 || prot > 7) return nullptr;
  void* p = ::mmap(nullptr, size, static_cast<int>(prot),
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) return nullptr;
  std::lock_guard<std::mutex> lk(g_mu);
  maps().push_back(Mapping{p, size});
  return p;
}
std::int32_t sceKernelMunmap(void* addr, std::size_t size) {
  if (addr == nullptr || size == 0) return -2;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    bool ours = false;
    for (auto it = maps().begin(); it != maps().end(); ++it)
      if (it->start == addr) { maps().erase(it); ours = true; break; }
    if (!ours) return -2;
  }
  return ::munmap(addr, size) == 0 ? 0 : -3;
}
std::int32_t sceKernelMTypeprotect(void* addr, std::size_t size, std::int64_t prot) {
  if (addr == nullptr || size == 0 || prot < 0 || prot > 7) return -2;
  return ::mprotect(addr, size, static_cast<int>(prot)) == 0 ? 0 : -3;
}

std::int64_t sceKernelOpen(const char* path, std::int64_t flags) {
  std::string host;
  if (path == nullptr || !jail().resolve(path, host)) return -2;
  int hf = 0;
  switch (flags & 0x3) {
    case 0: hf = O_RDONLY; break;
    case 1: hf = O_WRONLY; break;
    case 2: hf = O_RDWR; break;
    default: return -2;
  }
  if (flags & 0x0200) {  // CREAT: parents inside the jail first
    const auto slash = host.find_last_of('/');
    if (slash != std::string::npos && slash > 0) {
      std::error_code ec;
      std::filesystem::create_directories(host.substr(0, slash), ec);
      if (ec) return -3;
    }
    hf |= O_CREAT;
  }
  if (flags & 0x0400) hf |= O_TRUNC;
  if (flags & 0x0008) hf |= O_APPEND;
  const int fd = ::open(host.c_str(), hf, 0644);
  return fd < 0 ? -3 : fd;
}
std::int64_t sceKernelRead(std::int64_t fd, void* buf, std::size_t n) {
  const ssize_t r = ::read(static_cast<int>(fd), buf, n);
  return r < 0 ? -3 : r;
}
std::int64_t sceKernelWrite(std::int64_t fd, const void* buf, std::size_t n) {
  const ssize_t w = ::write(static_cast<int>(fd), buf, n);
  return w < 0 ? -3 : w;
}
std::int32_t sceKernelClose(std::int64_t fd) {
  return ::close(static_cast<int>(fd)) == 0 ? 0 : -3;
}
std::int64_t sceKernelLseek(std::int64_t fd, std::int64_t off, std::int64_t whence) {
  if (whence < 0 || whence > 2) return -2;
  const off_t r = ::lseek(static_cast<int>(fd), off, static_cast<int>(whence));
  return r < 0 ? -3 : r;
}
std::int32_t sceKernelFstat(std::int64_t fd, std::int64_t* size_out) {
  if (size_out == nullptr) return -2;
  struct stat st {};
  if (::fstat(static_cast<int>(fd), &st) != 0) return -3;
  *size_out = st.st_size;
  return 0;
}

}  // extern "C"
