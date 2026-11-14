// PNR Graphics - draw state extracted from tracked PM4 registers (Core slice C4b), GPL-2.0.
// Reads only registers named in graphics/amd_regs.hpp. Everything is "per public AMD headers,
// unverified on PlayStation". Tested on SYNTHETIC command buffers.
#pragma once
#include <array>
#include <cstdint>
#include <string>

#include "graphics/amd_regs.hpp"
#include "graphics/pm4.hpp"

namespace pnr::graphics {

struct ShaderBinding {
  bool present = false;
  std::uint64_t address = 0;       // GPU virtual address (register value << 8)
  std::uint32_t rsrc1 = 0, rsrc2 = 0;
  std::array<std::uint32_t, amd::kUserDataCount> user_data{};
  std::array<bool, amd::kUserDataCount> user_data_set{};
};

struct Viewport { bool present = false; float xscale = 0, xoffset = 0, yscale = 0, yoffset = 0, zscale = 0, zoffset = 0; };
struct ColorTarget {
  bool present = false;
  std::uint64_t address = 0;       // (BASE_EXT << 40 | BASE << 8)
  std::uint32_t pitch = 0, info = 0, attrib = 0, target_mask = 0;
};

struct DrawBindings {
  ShaderBinding vs, gs, ps;        // gs is the stage used when the vertex work runs as a geometry-stage program
  Viewport viewport;
  ColorTarget color0;
  amd::PrimitiveType primitive = amd::PrimitiveType::None;
  std::uint32_t screen_scissor_tl = 0, screen_scissor_br = 0;
  bool has_scissor = false;
  // What a minimal draw would need but is missing, in plain words.
  std::string missing;
  const ShaderBinding* vertex_stage() const { return vs.present ? &vs : (gs.present ? &gs : nullptr); }
};

DrawBindings extract_bindings(const pm4::DrawCall& d);
// One readable block, for `pnr gnm-decode`.
std::string describe_bindings(const DrawBindings& b);

}  // namespace pnr::graphics
