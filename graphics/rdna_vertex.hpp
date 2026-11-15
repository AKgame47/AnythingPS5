// PNR "compute-as-vertex-stage" bridge, GPL-2.0. Experimental.
// Builds restricted RDNA machine code (only instructions the PNR translator
// accepts) that transforms points by a 4x4 matrix, one clip-space component
// per dispatch, then runs it through the checked translated-compute route.
// NOT a Sony/RDNA vertex-stage ABI: no VS inputs/exports, no parameter cache,
// no real driver state. Matrix scalars arrive as s0..s15 (PNR uniform ABI).
// Float ops are binary32 add/mul under host Vulkan policy (no PS5 rounding).
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include "core/runtime.hpp"
#include "graphics/vulkan.hpp"
#include "shader/translate.hpp"

namespace pnr::graphics {

inline constexpr uint64_t kRdnaVertexBase = 0x1200001000ull;
inline constexpr uint32_t kRdnaVertexMax = 24;  // lanes in the single workgroup (<=64)

namespace detail {
inline uint32_t rdna_vop2(uint32_t op, uint32_t d, uint32_t v1, uint32_t src0) {
  return (op << 25u) | (d << 17u) | (v1 << 9u) | src0;
}
// Lane address setup + three loads (x,y,z regions) into v11,v12,v13; address
// pair for the output region ends up in (v9,v10).
inline void rdna_prelude(std::vector<uint32_t>& w, uint32_t n) {
  auto lit = [&](uint32_t d, uint32_t value) {
    w.push_back(0x7e0002ffu | (d << 17u)); w.push_back(value);
  };
  const uint32_t lo = uint32_t(kRdnaVertexBase);
  const uint32_t hi = uint32_t(kRdnaVertexBase >> 32u);
  w.push_back(rdna_vop2(0x25, 1, 0, 256));  // v1 = v0 + v0   (lane*2)
  w.push_back(rdna_vop2(0x25, 2, 1, 257));  // v2 = v1 + v1   (lane*4)
  const uint32_t region_off[4] = {0, n * 4, n * 8, n * 12};
  for (uint32_t k = 0; k < 4; ++k) {   // address pairs (v3,v4) (v5,v6) (v7,v8) (v9,v10)
    const uint32_t a = 3 + 2 * k;
    lit(a, lo + region_off[k]);
    w.push_back(rdna_vop2(0x25, a, 2, 256 + a));  // lo += lane*4
    lit(a + 1, hi);
  }
  for (uint32_t k = 0; k < 3; ++k) {
    w.push_back(0xdc300000u);
    w.push_back(0x007f0000u | ((11 + k) << 24u) | (3 + 2 * k));
  }
}
inline void rdna_epilogue(std::vector<uint32_t>& w) {
  w.push_back(0xdc700000u); w.push_back(0x007f0000u | (14u << 8u) | 9u);  // store v14
  w.push_back(0xbf810000u);
}
// Two-word VOP3A v_fma_f32 (AMD public "RDNA 2" ISA, Table 83 opcode 331),
// no modifiers. Sources are 9-bit selectors: SGPR k = k, VGPR k = 256+k.
inline void rdna_fma_f32(std::vector<uint32_t>& w, uint32_t d,
                         uint32_t s0, uint32_t s1, uint32_t s2) {
  w.push_back(0xd4000000u | (331u << 16u) | d);
  w.push_back(s0 | (s1 << 9u) | (s2 << 18u));
}
}  // namespace detail

// SSBO word layout for n points: x[n], y[n], z[n], out[n].
// out_c = m[c]*x + m[4+c]*y + m[8+c]*z + m[12+c]  (column-major m, w_in = 1),
// summed left to right.
inline std::vector<uint32_t> rdna_vertex_component_kernel(uint32_t c, uint32_t n) {
  using detail::rdna_vop2;
  std::vector<uint32_t> w;
  detail::rdna_prelude(w, n);
  w.push_back(rdna_vop2(8, 14, 11, c));       // v14 = s[c]   * x
  w.push_back(rdna_vop2(8, 15, 12, 4 + c));   // v15 = s[4+c] * y
  w.push_back(rdna_vop2(3, 14, 15, 256 + 14));  // v14 += v15
  w.push_back(rdna_vop2(8, 15, 13, 8 + c));   // v15 = s[8+c] * z
  w.push_back(rdna_vop2(3, 14, 15, 256 + 14));  // v14 += v15
  w.push_back(rdna_vop2(3, 14, 14, 12 + c));  // v14 = s[12+c] + v14
  detail::rdna_epilogue(w);
  return w;
}

// Lambert dot kernel over the same layout (nx[n], ny[n], nz[n], out[n]):
// out = s0*nx + s1*ny + s2*nz, summed left to right (3 uniform words: the unit
// light direction in object space). No clamp: max() is outside the translated
// subset, so the fragment shader clamps.
inline std::vector<uint32_t> rdna_dot3_kernel(uint32_t n) {
  using detail::rdna_vop2;
  std::vector<uint32_t> w;
  detail::rdna_prelude(w, n);
  w.push_back(rdna_vop2(8, 14, 11, 0));
  w.push_back(rdna_vop2(8, 15, 12, 1));
  w.push_back(rdna_vop2(3, 14, 15, 256 + 14));
  w.push_back(rdna_vop2(8, 15, 13, 2));
  w.push_back(rdna_vop2(3, 14, 15, 256 + 14));
  detail::rdna_epilogue(w);
  return w;
}

// Same component kernel built on fused multiply-add: one VOP3 v_fma_f32 per
// matrix term, single rounding per term instead of separate mul/add. Same
// uniform ABI (s0..s15 = column-major matrix) and SSBO layout.
inline std::vector<uint32_t> rdna_vertex_component_kernel_fma(uint32_t c, uint32_t n) {
  std::vector<uint32_t> w;
  detail::rdna_prelude(w, n);
  // out_c = fma(x, m[c], fma(y, m[4+c], fma(z, m[8+c], m[12+c])))
  w.push_back(0x7e000200u | (14u << 17u) | (12u + c));  // v_mov_b32 v14, s[12+c]
  detail::rdna_fma_f32(w, 14, 256 + 13, 8 + c, 256 + 14);
  detail::rdna_fma_f32(w, 14, 256 + 12, 4 + c, 256 + 14);
  detail::rdna_fma_f32(w, 14, 256 + 11, c, 256 + 14);
  detail::rdna_epilogue(w);
  return w;
}

// Transforms n points (xyz interleaved) by column-major mat4 words (as uint
// bit patterns). Fills clip[4n] with x,y,z,w per point. Four dispatches.
// fma=true selects the fused multiply-add kernel (v_fma_f32 VOP3 path).
inline Status rdna_transform_points(VulkanBackend& vk, const std::vector<float>& xyz,
                                    const std::vector<uint32_t>& mat_words,
                                    std::vector<float>* clip, bool fma = false) {
  if (!clip) return Status::Fail(100, "rdna-vertex", "output vector required");
  if (xyz.empty() || xyz.size() % 3 || xyz.size() / 3 > kRdnaVertexMax)
    return Status::Fail(100, "rdna-vertex", "points must be xyz triples, 1..24");
  if (mat_words.size() != 16)
    return Status::Fail(100, "rdna-vertex", "matrix must be 16 words");
  const uint32_t n = uint32_t(xyz.size() / 3);
  clip->assign(size_t(n) * 4, 0.0f);
  for (uint32_t c = 0; c < 4; ++c) {
    auto t = shader::translate_compute_words(fma ? rdna_vertex_component_kernel_fma(c, n)
                                                 : rdna_vertex_component_kernel(c, n),
                                             kRdnaVertexBase, n * 4, n, 16);
    if (!t.ok) return Status::Fail(100, "rdna-vertex", "translate: " + t.error);
    std::vector<uint32_t> io(n * 4, 0);
    for (uint32_t i = 0; i < n; ++i)
      for (uint32_t k = 0; k < 3; ++k)
        std::memcpy(&io[k * n + i], &xyz[i * 3 + k], 4);
    auto st = shader::run_translated_compute(vk, t, &io, mat_words);
    if (!st) return st;
    for (uint32_t i = 0; i < n; ++i) std::memcpy(&(*clip)[i * 4 + c], &io[3 * n + i], 4);
  }
  return Status::Ok();
}

// Per-vertex Lambert dot d[i] = dot(light_obj, n[i]) for n unit normals (xyz
// interleaved), one dispatch of the dot3 kernel. light_obj is the unit light
// direction (toward the light) in the mesh's object space.
inline Status rdna_light_dots(VulkanBackend& vk, const std::vector<float>& normals,
                              const float light_obj[3], std::vector<float>* d) {
  if (!d) return Status::Fail(100, "rdna-vertex", "output vector required");
  if (normals.empty() || normals.size() % 3 || normals.size() / 3 > kRdnaVertexMax)
    return Status::Fail(100, "rdna-vertex", "normals must be xyz triples, 1..24");
  const uint32_t n = uint32_t(normals.size() / 3);
  auto t = shader::translate_compute_words(rdna_dot3_kernel(n), kRdnaVertexBase, n * 4, n, 3);
  if (!t.ok) return Status::Fail(100, "rdna-vertex", "translate: " + t.error);
  std::vector<uint32_t> io(n * 4, 0), u(3);
  for (uint32_t i = 0; i < n; ++i)
    for (uint32_t k = 0; k < 3; ++k) std::memcpy(&io[k * n + i], &normals[i * 3 + k], 4);
  for (uint32_t k = 0; k < 3; ++k) std::memcpy(&u[k], &light_obj[k], 4);
  auto st = shader::run_translated_compute(vk, t, &io, u);
  if (!st) return st;
  d->assign(n, 0.0f);
  for (uint32_t i = 0; i < n; ++i) std::memcpy(&(*d)[i], &io[3 * n + i], 4);
  return Status::Ok();
}


// ---- slice S6: translated perspective divide + viewport transform ----
namespace detail {
// Generalised prelude: address pairs for nregions n-word regions (pair k in
// v[3+2k], v[4+2k]) and nloads FLAT dword loads of the first regions into
// first_load_reg+k. Same lane addressing as rdna_prelude (v0 = lane index).
inline void rdna_prelude_multi(std::vector<uint32_t>& w, uint32_t n,
                               uint32_t nregions, uint32_t nloads,
                               uint32_t first_load_reg) {
  auto lit = [&](uint32_t d, uint32_t value) {
    w.push_back(0x7e0002ffu | (d << 17u)); w.push_back(value);
  };
  const uint32_t lo = uint32_t(kRdnaVertexBase);
  const uint32_t hi = uint32_t(kRdnaVertexBase >> 32u);
  w.push_back(rdna_vop2(0x25, 1, 0, 256));  // v1 = v0 + v0   (lane*2)
  w.push_back(rdna_vop2(0x25, 2, 1, 257));  // v2 = v1 + v1   (lane*4)
  for (uint32_t k = 0; k < nregions; ++k) {
    const uint32_t a = 3 + 2 * k;
    lit(a, lo + n * 4 * k);
    w.push_back(rdna_vop2(0x25, a, 2, 256 + a));  // lo += lane*4
    lit(a + 1, hi);
  }
  for (uint32_t k = 0; k < nloads; ++k) {
    w.push_back(0xdc300000u);
    w.push_back(0x007f0000u | ((first_load_reg + k) << 24u) | (3 + 2 * k));
  }
}
// FLAT dword store of data_vgpr through address pair addr_pair_lo/+1.
inline void rdna_store(std::vector<uint32_t>& w, uint32_t data_vgpr, uint32_t addr_pair_lo) {
  w.push_back(0xdc700000u);
  w.push_back(0x007f0000u | (data_vgpr << 8u) | addr_pair_lo);
}
// The translated subset allows one store per kernel, so the post-transform
// is one kernel per output component over the same 5-region layout:
// in clip x[n], y[n], z[n], w[n]; out region 4.
inline std::vector<uint32_t> post_body_ndc(uint32_t n, uint32_t c) {
  std::vector<uint32_t> w;
  rdna_prelude_multi(w, n, 5, 4, 50);          // v50=x v51=y v52=z v53=w
  w.push_back(0x7e000000u | (0x2au << 9u) | (54u << 17u) | (256u + 53u));  // v54 = rcp(w)
  w.push_back(rdna_vop2(8, 55, 50 + c, 256 + 54));                        // v55 = clip_c * inv_w
  rdna_store(w, 55, 3 + 2 * 4);
  w.push_back(0xbf810000u);
  return w;
}
inline std::vector<uint32_t> post_body_win(uint32_t n, uint32_t c) {
  std::vector<uint32_t> w = post_body_ndc(n, c);
  w.pop_back();  // drop end
  w.pop_back();  // drop store word 2
  w.pop_back();  // drop store word 1
  // v55 = ndc_c; win = fma(ndc_c, s[2c], s[2c+1]); depth clamps to [s6, s7].
  rdna_fma_f32(w, 55, 256 + 55, 2 * c, 2 * c + 1);
  if (c == 2) {
    w.push_back(rdna_vop2(0x10, 55, 55, 6));   // v55 = max(s6, v55)
    w.push_back(rdna_vop2(0x0f, 55, 55, 7));   // v55 = min(s7, v55)
  }
  rdna_store(w, 55, 3 + 2 * 4);
  w.push_back(0xbf810000u);
  return w;
}
}  // namespace detail

// ndc component c (0=x,1=y,2=z) of the post-transform: inv_w = rcp(w) (slice
// S5: correctly-rounded 1.0f/w, NOT RDNA's approximate rcp), ndc_c = clip_c *
// inv_w. w = 0 gives IEEE inf/NaN propagation: no guard, documented.
inline std::vector<uint32_t> rdna_post_ndc_kernel(uint32_t c, uint32_t n) {
  return detail::post_body_ndc(n, c);
}

// Window component c of the post-transform: win_c = fma(ndc_c, s[2c],
// s[2c+1]); the depth component (c = 2) is clamped to [s6, s7] with host
// FMin/FMax NaN semantics. Uniform ABI (8 words): s0=xscale s1=xoffset
// s2=yscale s3=yoffset s4=zscale s5=zoffset s6=zmin s7=zmax.
inline std::vector<uint32_t> rdna_post_win_kernel(uint32_t c, uint32_t n) {
  return detail::post_body_win(n, c);
}

// Runs the post-transform over n clip-space vertices (clip = 4n interleaved
// x,y,z,w). vp8 = 8 viewport words (xscale, xoffset, yscale, yoffset, zscale,
// zoffset, zmin, zmax) as float bit patterns. Fills ndc[3n] and win[3n]
// (window x, y, clamped depth). Six dispatches (one store per kernel).
inline Status rdna_post_transform_clip(VulkanBackend& vk, const std::vector<float>& clip,
                                       const std::vector<uint32_t>& vp8,
                                       std::vector<float>* ndc, std::vector<float>* win) {
  if (!ndc || !win) return Status::Fail(100, "rdna-vertex", "output vectors required");
  if (clip.empty() || clip.size() % 4 || clip.size() / 4 > kRdnaVertexMax)
    return Status::Fail(100, "rdna-vertex", "clip must be xyzw quads, 1..24");
  if (vp8.size() != 8) return Status::Fail(100, "rdna-vertex", "viewport must be 8 words");
  const uint32_t n = uint32_t(clip.size() / 4);
  ndc->assign(size_t(n) * 3, 0.0f);
  win->assign(size_t(n) * 3, 0.0f);
  for (uint32_t c = 0; c < 3; ++c) {
    auto io = [&]() {
      std::vector<uint32_t> v(size_t(n) * 5, 0);
      for (uint32_t i = 0; i < n; ++i)
        for (uint32_t k = 0; k < 4; ++k) std::memcpy(&v[k * n + i], &clip[i * 4 + k], 4);
      return v;
    };
    {
      auto t = shader::translate_compute_words(rdna_post_ndc_kernel(c, n), kRdnaVertexBase, n * 5, n, 0);
      if (!t.ok) return Status::Fail(100, "rdna-vertex", "translate: " + t.error);
      std::vector<uint32_t> buf = io();
      auto st = shader::run_translated_compute(vk, t, &buf, {});
      if (!st) return st;
      for (uint32_t i = 0; i < n; ++i) std::memcpy(&(*ndc)[i * 3 + c], &buf[4 * n + i], 4);
    }
    {
      auto t = shader::translate_compute_words(rdna_post_win_kernel(c, n), kRdnaVertexBase, n * 5, n, 8);
      if (!t.ok) return Status::Fail(100, "rdna-vertex", "translate: " + t.error);
      std::vector<uint32_t> buf = io();
      auto st = shader::run_translated_compute(vk, t, &buf, vp8);
      if (!st) return st;
      for (uint32_t i = 0; i < n; ++i) std::memcpy(&(*win)[i * 3 + c], &buf[4 * n + i], 4);
    }
  }
  return Status::Ok();
}

}  // namespace pnr::graphics
