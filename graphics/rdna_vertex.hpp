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

// SSBO word layout for n points: x[n], y[n], z[n], out[n].
// out_c = m[c]*x + m[4+c]*y + m[8+c]*z + m[12+c]  (column-major m, w_in = 1),
// summed left to right.
inline std::vector<uint32_t> rdna_vertex_component_kernel(uint32_t c, uint32_t n) {
  std::vector<uint32_t> w;
  auto vop2 = [](uint32_t op, uint32_t d, uint32_t v1, uint32_t src0) {
    return (op << 25u) | (d << 17u) | (v1 << 9u) | src0;
  };
  auto lit = [&](uint32_t d, uint32_t value) {
    w.push_back(0x7e0002ffu | (d << 17u)); w.push_back(value);
  };
  const uint32_t lo = uint32_t(kRdnaVertexBase);
  const uint32_t hi = uint32_t(kRdnaVertexBase >> 32u);
  w.push_back(vop2(0x25, 1, 0, 256));  // v1 = v0 + v0   (lane*2)
  w.push_back(vop2(0x25, 2, 1, 257));  // v2 = v1 + v1   (lane*4)
  const uint32_t region_off[4] = {0, n * 4, n * 8, n * 12};
  for (uint32_t k = 0; k < 4; ++k) {   // address pairs (v3,v4) (v5,v6) (v7,v8) (v9,v10)
    const uint32_t a = 3 + 2 * k;
    lit(a, lo + region_off[k]);
    w.push_back(vop2(0x25, a, 2, 256 + a));  // lo += lane*4
    lit(a + 1, hi);
  }
  auto flat = [&](bool load, uint32_t addr, uint32_t reg) {
    if (load) { w.push_back(0xdc300000u); w.push_back(0x007f0000u | (reg << 24u) | addr); }
    else      { w.push_back(0xdc700000u); w.push_back(0x007f0000u | (reg << 8u) | addr); }
  };
  flat(true, 3, 11); flat(true, 5, 12); flat(true, 7, 13);  // x,y,z
  w.push_back(vop2(8, 14, 11, c));       // v14 = s[c]   * x
  w.push_back(vop2(8, 15, 12, 4 + c));   // v15 = s[4+c] * y
  w.push_back(vop2(3, 14, 15, 256 + 14));  // v14 += v15
  w.push_back(vop2(8, 15, 13, 8 + c));   // v15 = s[8+c] * z
  w.push_back(vop2(3, 14, 15, 256 + 14));  // v14 += v15
  w.push_back(vop2(3, 14, 14, 12 + c));  // v14 = s[12+c] + v14
  flat(false, 9, 14);
  w.push_back(0xbf810000u);
  return w;
}

// Transforms n points (xyz interleaved) by column-major mat4 words (as uint
// bit patterns). Fills clip[4n] with x,y,z,w per point. Four dispatches.
inline Status rdna_transform_points(VulkanBackend& vk, const std::vector<float>& xyz,
                                    const std::vector<uint32_t>& mat_words,
                                    std::vector<float>* clip) {
  if (!clip) return Status::Fail(100, "rdna-vertex", "output vector required");
  if (xyz.empty() || xyz.size() % 3 || xyz.size() / 3 > kRdnaVertexMax)
    return Status::Fail(100, "rdna-vertex", "points must be xyz triples, 1..24");
  if (mat_words.size() != 16)
    return Status::Fail(100, "rdna-vertex", "matrix must be 16 words");
  const uint32_t n = uint32_t(xyz.size() / 3);
  clip->assign(size_t(n) * 4, 0.0f);
  for (uint32_t c = 0; c < 4; ++c) {
    auto t = shader::translate_compute_words(rdna_vertex_component_kernel(c, n),
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

}  // namespace pnr::graphics
