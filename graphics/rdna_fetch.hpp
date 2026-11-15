// PNR Graphics - vertex fetch kernels, GPL-2.0. Experimental.
// Builds restricted RDNA machine code (only instructions the PNR translator
// accepts) that copies one float component per dispatch from a raw
// V#-described vertex record region into a gathered float region - the job
// a GNM fetch shader does. Like GNM fetch shaders, the code is specialized
// per vertex layout: component index and record stride (in dwords) are
// baked in at build time, and the PVS5 blob header carries them so the draw
// can validate the bound V# against the compiled layout. 32-bit float
// formats only (slice S15); unorm/snorm/integer conversions are Planned
// (they need lshr + cvt_f32_u32 in the translator subset).
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

#include "graphics/rdna_vertex.hpp"  // kRdnaVertexBase, detail::rdna_vop2

namespace pnr::graphics {

namespace detail {
// v_mov_b32 (VOP1 opcode 1), reg-to-reg form: word = 0x7e000200 | dst<<17 | src0.
inline uint32_t rdna_vmov(uint32_t d, uint32_t src0) {
  return 0x7e000200u | (d << 17u) | src0;
}
// va = lane * stride_words, from doublings of v0 (the lane). stride 1..16.
// Powers: v1 = 2*lane, v2 = 4*lane, v3 = 8*lane (VGPR src selectors 257/258/259).
inline void rdna_lane_stride(std::vector<uint32_t>& w, uint32_t va, uint32_t stride_words) {
  w.push_back(rdna_vop2(0x25, 1, 0, 256));  // v1 = lane*2
  w.push_back(rdna_vop2(0x25, 2, 1, 257));  // v2 = lane*4
  w.push_back(rdna_vop2(0x25, 3, 2, 258));  // v3 = lane*8
  const uint32_t sel[3] = {257, 258, 259};  // 2l, 4l, 8l
  uint32_t s = stride_words;
  bool first = true;
  auto term = [&](uint32_t p) {  // add power p (0=2l, 1=4l, 2=8l) into va
    if (first) { w.push_back(rdna_vmov(va, sel[p])); first = false; }
    else w.push_back(rdna_vop2(0x25, va, va, sel[p]));
  };
  while (s >= 8) { term(2); s -= 8; }
  if (s & 4) term(1);
  if (s & 2) term(0);
  if (s & 1) {  // lane itself
    if (first) { w.push_back(rdna_vmov(va, 256)); first = false; }
    else w.push_back(rdna_vop2(0x25, va, va, 256));
  }
}
}  // namespace detail

// One-component gather kernel. SSBO layout: raw dwords
// [0, lanes*stride_words), gathered floats
// [lanes*stride_words, lanes*stride_words + lanes). Lane record dword base
// = lane*stride_words; the component dword is +comp. Addresses are 64-bit
// pairs like the clip kernels; offsets stay under 16KB above the base low
// word, so no carry path is needed. One load + one store, inside the subset.
// Slice S16: data_format 10 (8_8_8_8) with unorm numerics converts the byte
// in translated code: v_and byte mask, cvt_i32->f32 (the masked bytes 0..2
// stay below 2^24, so signed conversion is exact), fmul by 1/(255*256^c) -
// exact after the UNORM8 target quantization (error ~2 ulp, far below the
// 0.5/255 step). Alpha (byte 3) is not fetched (needs unsigned cvt).
inline std::vector<uint32_t> rdna_fetch_component_kernel(uint32_t comp, uint32_t stride_words, uint32_t lanes,
                                                         uint32_t data_format = 13) {
  using detail::rdna_vop2;
  std::vector<uint32_t> w;
  auto lit = [&](uint32_t d, uint32_t value) {
    w.push_back(0x7e0002ffu | (d << 17u)); w.push_back(value);
  };
  const uint32_t lo = uint32_t(kRdnaVertexBase), hi = uint32_t(kRdnaVertexBase >> 32u);
  detail::rdna_lane_stride(w, 4, stride_words);   // v4 = lane*stride_words
  w.push_back(rdna_vop2(0x25, 4, 4, 260));        // v4 *= 2
  w.push_back(rdna_vop2(0x25, 4, 4, 260));        // v4 *= 2 -> record byte offset
  // Float formats: the component dword sits at +comp*4 in the record.
  // 8_8_8_8: all components share the record's first dword; the byte is
  // extracted after the load.
  lit(5, data_format == 10 ? 0 : comp * 4);
  w.push_back(rdna_vop2(0x25, 4, 4, 261));        // v4 += comp byte offset
  lit(6, lo);
  w.push_back(rdna_vop2(0x25, 6, 6, 260));        // v6 = base lo + v4 (src pair low)
  lit(7, hi);                                     // src pair high
  w.push_back(rdna_vop2(0x25, 8, 0, 256));        // v8 = lane*2
  w.push_back(rdna_vop2(0x25, 8, 8, 264));        // v8 = lane*4
  lit(10, lo + lanes * stride_words * 4);
  w.push_back(rdna_vop2(0x25, 8, 8, 266));        // v8 += gathered region offset (dst pair low)
  lit(9, hi);                                     // dst pair high
  w.push_back(0xdc300000u);                       // flat_load_dword v11, (v6,v7)
  w.push_back(0x007f0000u | (11u << 24u) | 6u);
  if (data_format == 10) {                        // 8_8_8_8 unorm -> float
    float scalef = 1.0f / 255.0f;
    for (uint32_t k = 0; k < comp; ++k) scalef /= 256.0f;
    uint32_t scale; std::memcpy(&scale, &scalef, 4);
    lit(12, 0xFFu << (comp * 8));
    w.push_back(rdna_vop2(0x1b, 12, 11, 268));    // v12 = v11 & byte mask
    w.push_back(0x7e000000u | (8u << 9u) | (13u << 17u) | (256u + 12u));  // v_cvt_i32_f32 v13, v12
    lit(14, scale);
    w.push_back(rdna_vop2(8, 11, 13, 270));       // v11 = v13 * scale  (f32)
  }
  w.push_back(0xdc700000u);                       // flat_store_dword (v8,v9), v11
  w.push_back(0x007f0000u | (11u << 8u) | 8u);
  w.push_back(0xbf810000u);                       // s_endpgm
  return w;
}

}  // namespace pnr::graphics
