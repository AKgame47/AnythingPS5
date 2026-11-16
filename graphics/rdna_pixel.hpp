// PNR translated pixel stage, GPL-2.0. Experimental.
// A "PPS1" blob is a PNR TEST ABI (not the PlayStation PS ABI: no
// interpolation, no export targets, no parameter cache). It holds four
// restricted RDNA kernels - one per colour component, one store each -
// computing a CONSTANT draw colour from eight uniform floats:
//   s0..s3 = base rgba, s4 = intensity, s5 = ambient,
//   s6 = clamp lo (0.0), s7 = clamp hi (1.0)
//   colour_c = clamp(base_c * (intensity + ambient), lo, hi)
// Float ops are binary32 add/mul/min/max under host Vulkan FP policy (no PS5
// rounding); min/max keep their documented host GLSL.std.450 NaN semantics.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include "core/runtime.hpp"
#include "graphics/vulkan.hpp"
#include "shader/translate.hpp"

namespace pnr::graphics {

inline constexpr uint32_t kPnrPsMagic = 0x31535050u;   // "PPS1"
inline constexpr uint32_t kPnrPs2Magic = 0x32535050u;  // "PPS2"
inline constexpr uint64_t kRdnaPixelBase = 0x1200002000ull;

namespace detail {
inline uint32_t px_vop2(uint32_t op, uint32_t d, uint32_t v1, uint32_t src0) {
  return (op << 25u) | (d << 17u) | (v1 << 9u) | src0;
}
}  // namespace detail

// One-lane kernel storing component c (0..3) at word 0 of the buffer.
inline std::vector<uint32_t> rdna_pixel_component_kernel(uint32_t c) {
  using detail::px_vop2;
  const uint32_t lo = uint32_t(kRdnaPixelBase);
  const uint32_t hi = uint32_t(kRdnaPixelBase >> 32u);
  std::vector<uint32_t> w;
  auto lit = [&](uint32_t d, uint32_t value) {
    w.push_back(0x7e0002ffu | (d << 17u)); w.push_back(value);
  };
  auto mov = [&](uint32_t d, uint32_t s) { w.push_back(0x7e000200u | (d << 17u) | s); };
  lit(1, lo); lit(2, hi);                 // (v1,v2) = buffer address
  mov(10, 4);                             // v10 = s4 (intensity)
  w.push_back(px_vop2(3, 10, 10, 5));     // v10 += s5 (ambient)
  mov(11, c);                             // v11 = s_c (base component)
  w.push_back(px_vop2(8, 12, 11, 256 + 10));  // v12 = v10 * v11
  w.push_back(px_vop2(0x10, 12, 12, 6));  // v12 = max(s6, v12)
  w.push_back(px_vop2(0x0f, 12, 12, 7));  // v12 = min(s7, v12)
  w.push_back(0xdc700000u);               // store v12 at (v1,v2)
  w.push_back(0x007f0000u | (12u << 8u) | 1u);
  w.push_back(0xbf810000u);               // end
  return w;
}

// Builds the PPS1 blob.
inline std::vector<uint32_t> make_pnr_ps_blob() {
  std::vector<std::vector<uint32_t>> k;
  for (uint32_t c = 0; c < 4; ++c) k.push_back(rdna_pixel_component_kernel(c));
  std::vector<uint32_t> blob = {kPnrPsMagic, 4};
  for (auto& v : k) blob.push_back(static_cast<uint32_t>(v.size()));
  for (auto& v : k) blob.insert(blob.end(), v.begin(), v.end());
  return blob;
}

// Slice S17: a "PPS2" blob marks the textured pixel stage. It carries no
// kernels yet - {magic, 0} - and tells the draw to bind the T#/S# pair
// addressed by pixel user data 0..3 (T# at 0..1, S# at 2..3) and to sample
// it in the host rasterizer's fragment stage with vertex uv varyings.
// Sampling itself is not translated RDNA in this slice, stated in the notes.
inline std::vector<uint32_t> make_pnr_ps2_blob() { return {kPnrPs2Magic, 0}; }
// Slice S27: blob v1 - the T# is a Color2DArray; the vertex record carries a
// slice index after the uv pair (9 floats per vertex).
inline std::vector<uint32_t> make_pnr_ps2_array_blob() { return {kPnrPs2Magic, 1}; }
// Slice S28: blob v2 - the T# is a Cube; the vertex record carries the
// cubemap direction (9 floats per vertex: xyz rgb dir3).
inline std::vector<uint32_t> make_pnr_ps2_cube_blob() { return {kPnrPs2Magic, 2}; }

// PPS2 v3 (slice S30): the pixel stage samples a Color3D T#. Same layout as
// v0; the 9-float vertex record carries the 3D sampling coordinate (uvw,
// each in [0,1]) where v1 carries the slice index.
inline std::vector<uint32_t> make_pnr_ps2_3d_blob() { return {kPnrPs2Magic, 3}; }

// Runs the four component kernels; uniforms8 = s0..s7 as float bit patterns.
inline Status rdna_pixel_colour(VulkanBackend& vk, const std::vector<uint32_t>& uniforms8,
                                float out_rgba[4]) {
  if (!out_rgba) return Status::Fail(100, "rdna-pixel", "output required");
  if (uniforms8.size() != 8) return Status::Fail(100, "rdna-pixel", "uniforms must be 8 words");
  for (uint32_t c = 0; c < 4; ++c) {
    auto t = shader::translate_compute_words(rdna_pixel_component_kernel(c), kRdnaPixelBase, 1, 1, 8);
    if (!t.ok) return Status::Fail(100, "rdna-pixel", "translate: " + t.error);
    std::vector<uint32_t> io(1, 0);
    auto st = shader::run_translated_compute(vk, t, &io, uniforms8);
    if (!st) return st;
    std::memcpy(&out_rgba[c], &io[0], 4);
  }
  return Status::Ok();
}

}  // namespace pnr::graphics
