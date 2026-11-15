// PNR Graphics - draw from tracked GNM state to one Vulkan draw (Core slice C4d), GPL-2.0.
// EXPERIMENTAL. Reads the vertex-stage address and bindings from tracked PM4 registers, fetches the
// shader bytes through the GPU address map, runs the restricted RDNA translator and does ONE headless
// Vulkan draw into an offscreen image, then copies the pixels to the colour target address.
//
// PNR TEST ABI (not the PlayStation ABI; no real game shader runs through this):
//  * The vertex-stage memory holds a "PVS1" blob: dword magic 0x31535650, dword 4, four dword
//    lengths, then four restricted RDNA kernels (clip x, y, z, w) as built by rdna_vertex_component_kernel.
//  * VS user data 0,1 = lo,hi GPU address of 16 matrix floats (column-major); 2,3 = lo,hi GPU address
//    of the vertex buffer: per vertex 6 floats (x y z r g b).
//  * The pixel-stage address must be set and mapped, but pixel shader code is NOT translated: a fixed
//    pass-through colour shader is used. This is stated in the result.
//  * Viewport registers are checked against the target size (|scale| = size/2), not otherwise applied.
//  * Target size comes from the screen scissor bottom-right. Format: RGBA8 only (host image).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/runtime.hpp"
#include "graphics/gfx_state.hpp"
#include "graphics/gpu_va.hpp"
#include "graphics/vulkan.hpp"

namespace pnr::graphics {

inline constexpr std::uint32_t kPnrVsMagic = 0x31535650u;   // "PVS1"
inline constexpr std::uint32_t kPnrVs2Magic = 0x32535650u;  // "PVS2"
inline constexpr std::uint32_t kPnrVs3Magic = 0x33535650u;  // "PVS3"
inline constexpr std::uint32_t kPnrVs4Magic = 0x34535650u;  // "PVS4"

// Slice S9 sanity caps for one draw (PNR TEST ABI limits, not hardware):
// at most 64k transformed vertices and 4M indices per draw call.
inline constexpr std::uint32_t kDrawMaxVertices = 65536;
inline constexpr std::uint32_t kDrawMaxIndices = 4u * 1024u * 1024u;

// Builds the PVS1 blob for n vertices (1..kRdnaVertexMax).
std::vector<std::uint32_t> make_pnr_vs_blob(std::uint32_t n);

// PVS2: the four PVS1 clip kernels plus a fifth post-transform kernel
// (slice S6: translated perspective divide + viewport transform). The draw
// path feeds the rasterizer ndc positions with w = 1; the divide itself is
// done by translated RDNA code. Window/depth outputs are computed for
// verification; viewport scale/offset on screen stays host fixed-function.
std::vector<std::uint32_t> make_pnr_vs2_blob(std::uint32_t n);

// PVS3 (slice S9): batched draws past the 64-lane single-dispatch limit.
// Same layout as PVS1/PVS2 (magic, kernel count 4 or 7, lengths, kernels),
// but the kernels are built ONCE for the fixed 64-lane workgroup and carry
// no vertex count: the draw path runs ceil(n/64) dispatches, zero-padding
// the tail lanes of the last batch (their stores land in scratch and are
// discarded). Kernel count 4 = clip only; 7 = clip + post-transform (the
// PVS2 divide semantics). Covers any vertex count up to kDrawMaxVertices.
std::vector<std::uint32_t> make_pnr_vs3_blob(bool with_post);

// PVS4 (slice S11): translated per-vertex varying - Gouraud shading. Magic,
// dword 7, lengths, then 7 kernels built for the fixed 64-lane workgroup
// (batched like PVS3): 4 clip kernels + 3 shade kernels (one per colour
// channel). The vertex buffer layout for a PVS4 draw is 9 floats per vertex
// (x y z r g b nx ny nz). Each shade kernel computes
//   out_c = col_c * clamp(dot(n, light) + ambient, lo, hi)
// in translated RDNA (fused multiply-add chain); the shaded rgb replaces the
// raw vertex colours and the rasterizer interpolates it. VS user data 0..3 as
// usual (matrix, vertex buffer); user data 4..9 = light xyz, ambient, clamp
// lo/hi (missing data is refused). A PPS1 pixel blob still overrides the
// whole draw colour. No specular, no textures, one directional light.
std::vector<std::uint32_t> make_pnr_vs4_blob();

struct DrawResult {
  bool ok = false;
  std::string error;           // plain words, empty on success
  std::uint32_t width = 0, height = 0, vertices = 0;
  bool wrote_target = false;   // pixels were copied to the colour target address
  std::string notes;           // e.g. "pixel shader not translated: fixed colour used"
  std::vector<std::uint8_t> rgba;  // width*height*4, top row first
};

// One draw: DRAW_INDEX_AUTO, or DRAW_INDEX_INDX with a 16/32-bit index buffer in guest memory
// (slice S7); triangle lists, strips and fans (strips/fans expand to a list on the host,
// slice S10). PVS1/PVS2 blobs are built for one specific vertex count (at most 64, one
// dispatch); a PVS3 blob batches any count up to kDrawMaxVertices (slice S9). Other
// primitives and instancing beyond 1 are refused. (GNM has no 8-bit index type; nothing
// is emulated for one.)
DrawResult execute_draw(VulkanBackend& vk, GpuAddressSpace& mem, const pm4::DrawCall& draw);

}  // namespace pnr::graphics
