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

inline constexpr std::uint32_t kPnrVsMagic = 0x31535650u;  // "PVS1"

// Builds the PVS1 blob for n vertices (1..kRdnaVertexMax).
std::vector<std::uint32_t> make_pnr_vs_blob(std::uint32_t n);

struct DrawResult {
  bool ok = false;
  std::string error;           // plain words, empty on success
  std::uint32_t width = 0, height = 0, vertices = 0;
  bool wrote_target = false;   // pixels were copied to the colour target address
  std::string notes;           // e.g. "pixel shader not translated: fixed colour used"
  std::vector<std::uint8_t> rgba;  // width*height*4, top row first
};

// One non-indexed triangle-list draw (DRAW_INDEX_AUTO). Indexed and other primitives are refused.
DrawResult execute_draw(VulkanBackend& vk, GpuAddressSpace& mem, const pm4::DrawCall& draw);

}  // namespace pnr::graphics
