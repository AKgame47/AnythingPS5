#include "graphics/gfx_state.hpp"

#include <cstdio>
#include <cstring>

namespace pnr::graphics {

namespace {
using Regs = std::map<std::uint32_t, std::uint32_t>;
bool get(const Regs& r, std::uint32_t reg, std::uint32_t* v) { auto it = r.find(reg); if (it == r.end()) return false; *v = it->second; return true; }
float as_float(std::uint32_t v) { float f; std::memcpy(&f, &v, 4); return f; }

void shader(const Regs& r, std::uint32_t lo, std::uint32_t hi, std::uint32_t rs1, std::uint32_t rs2, std::uint32_t ud0, ShaderBinding* out) {
  std::uint32_t l = 0, h = 0;
  if (!get(r, lo, &l)) return;  // the low half is the presence marker
  get(r, hi, &h);
  out->present = true;
  out->address = ((static_cast<std::uint64_t>(h) << 32) | l) << 8;
  get(r, rs1, &out->rsrc1); get(r, rs2, &out->rsrc2);
  for (int i = 0; i < amd::kUserDataCount; ++i) out->user_data_set[i] = get(r, ud0 + static_cast<std::uint32_t>(i), &out->user_data[i]);
}
}  // namespace

DrawBindings extract_bindings(const pm4::DrawCall& d) {
  using namespace amd;
  DrawBindings b;
  const Regs& r = d.state;
  shader(r, SPI_SHADER_PGM_LO_VS, SPI_SHADER_PGM_HI_VS, SPI_SHADER_PGM_RSRC1_VS, SPI_SHADER_PGM_RSRC2_VS, SPI_SHADER_USER_DATA_VS_0, &b.vs);
  shader(r, SPI_SHADER_PGM_LO_GS, SPI_SHADER_PGM_HI_GS, SPI_SHADER_PGM_RSRC1_GS, SPI_SHADER_PGM_RSRC2_GS, SPI_SHADER_USER_DATA_GS_0, &b.gs);
  shader(r, SPI_SHADER_PGM_LO_PS, SPI_SHADER_PGM_HI_PS, SPI_SHADER_PGM_RSRC1_PS, SPI_SHADER_PGM_RSRC2_PS, SPI_SHADER_USER_DATA_PS_0, &b.ps);
  std::uint32_t v = 0;
  if (get(r, PA_CL_VPORT_XSCALE, &v)) {
    b.viewport.present = true; b.viewport.xscale = as_float(v);
    if (get(r, PA_CL_VPORT_XOFFSET, &v)) b.viewport.xoffset = as_float(v);
    if (get(r, PA_CL_VPORT_YSCALE, &v)) b.viewport.yscale = as_float(v);
    if (get(r, PA_CL_VPORT_YOFFSET, &v)) b.viewport.yoffset = as_float(v);
    if (get(r, PA_CL_VPORT_ZSCALE, &v)) b.viewport.zscale = as_float(v);
    if (get(r, PA_CL_VPORT_ZOFFSET, &v)) b.viewport.zoffset = as_float(v);
  }
  std::uint32_t base = 0, ext = 0;
  if (get(r, CB_COLOR0_BASE, &base)) {
    b.color0.present = true;
    get(r, CB_COLOR0_BASE_EXT, &ext);
    b.color0.address = (static_cast<std::uint64_t>(ext & 0xff) << 40) | (static_cast<std::uint64_t>(base) << 8);
    get(r, CB_COLOR0_PITCH, &b.color0.pitch); get(r, CB_COLOR0_INFO, &b.color0.info); get(r, CB_COLOR0_ATTRIB, &b.color0.attrib);
    get(r, CB_TARGET_MASK, &b.color0.target_mask);
  }
  if (get(r, VGT_PRIMITIVE_TYPE, &v)) b.primitive = static_cast<PrimitiveType>(v);
  if (get(r, PA_SC_SCREEN_SCISSOR_BR, &b.screen_scissor_br)) { b.has_scissor = true; get(r, PA_SC_SCREEN_SCISSOR_TL, &b.screen_scissor_tl); }
  if (!b.vertex_stage()) b.missing += "no vertex-stage program address (VS or GS); ";
  if (!b.ps.present) b.missing += "no pixel shader address; ";
  if (!b.color0.present) b.missing += "no color target 0; ";
  if (!b.viewport.present) b.missing += "no viewport transform; ";
  if (b.primitive == PrimitiveType::None) b.missing += "no primitive type; ";
  return b;
}

std::string describe_bindings(const DrawBindings& b) {
  char buf[256];
  std::string o;
  auto sh = [&](const char* n, const ShaderBinding& s) {
    if (!s.present) return;
    std::snprintf(buf, sizeof buf, "  %s shader @0x%llx rsrc1=0x%x rsrc2=0x%x\n", n, static_cast<unsigned long long>(s.address), s.rsrc1, s.rsrc2);
    o += buf;
  };
  sh("vertex(VS)", b.vs); sh("vertex(GS)", b.gs); sh("pixel", b.ps);
  if (b.viewport.present) { std::snprintf(buf, sizeof buf, "  viewport scale=(%g,%g,%g) offset=(%g,%g,%g)\n", b.viewport.xscale, b.viewport.yscale, b.viewport.zscale, b.viewport.xoffset, b.viewport.yoffset, b.viewport.zoffset); o += buf; }
  if (b.color0.present) { std::snprintf(buf, sizeof buf, "  color0 @0x%llx pitch=0x%x info=0x%x attrib=0x%x mask=0x%x\n", static_cast<unsigned long long>(b.color0.address), b.color0.pitch, b.color0.info, b.color0.attrib, b.color0.target_mask); o += buf; }
  std::snprintf(buf, sizeof buf, "  primitive=%u\n", static_cast<unsigned>(b.primitive)); o += buf;
  if (!b.missing.empty()) o += "  MISSING for a draw: " + b.missing + "\n";
  o += "  (register meaning per public AMD headers, unverified on PlayStation)\n";
  return o;
}

}  // namespace pnr::graphics

namespace pnr::graphics::amd {
const char* register_name(std::uint32_t r) {
  struct E { std::uint32_t reg; const char* name; };
  static const E t[] = {
      {SPI_SHADER_PGM_LO_PS, "SPI_SHADER_PGM_LO_PS"}, {SPI_SHADER_PGM_HI_PS, "SPI_SHADER_PGM_HI_PS"}, {SPI_SHADER_PGM_RSRC1_PS, "SPI_SHADER_PGM_RSRC1_PS"}, {SPI_SHADER_PGM_RSRC2_PS, "SPI_SHADER_PGM_RSRC2_PS"},
      {SPI_SHADER_PGM_LO_VS, "SPI_SHADER_PGM_LO_VS"}, {SPI_SHADER_PGM_HI_VS, "SPI_SHADER_PGM_HI_VS"}, {SPI_SHADER_PGM_RSRC1_VS, "SPI_SHADER_PGM_RSRC1_VS"}, {SPI_SHADER_PGM_RSRC2_VS, "SPI_SHADER_PGM_RSRC2_VS"},
      {SPI_SHADER_PGM_LO_GS, "SPI_SHADER_PGM_LO_GS"}, {SPI_SHADER_PGM_HI_GS, "SPI_SHADER_PGM_HI_GS"}, {SPI_SHADER_PGM_RSRC1_GS, "SPI_SHADER_PGM_RSRC1_GS"}, {SPI_SHADER_PGM_RSRC2_GS, "SPI_SHADER_PGM_RSRC2_GS"},
      {COMPUTE_PGM_LO, "COMPUTE_PGM_LO"}, {COMPUTE_PGM_HI, "COMPUTE_PGM_HI"}, {COMPUTE_PGM_RSRC1, "COMPUTE_PGM_RSRC1"}, {COMPUTE_PGM_RSRC2, "COMPUTE_PGM_RSRC2"},
      {PA_SC_SCREEN_SCISSOR_TL, "PA_SC_SCREEN_SCISSOR_TL"}, {PA_SC_SCREEN_SCISSOR_BR, "PA_SC_SCREEN_SCISSOR_BR"}, {PA_SC_VPORT_SCISSOR_0_TL, "PA_SC_VPORT_SCISSOR_0_TL"}, {PA_SC_VPORT_SCISSOR_0_BR, "PA_SC_VPORT_SCISSOR_0_BR"},
      {PA_CL_VPORT_XSCALE, "PA_CL_VPORT_XSCALE"}, {PA_CL_VPORT_XOFFSET, "PA_CL_VPORT_XOFFSET"}, {PA_CL_VPORT_YSCALE, "PA_CL_VPORT_YSCALE"}, {PA_CL_VPORT_YOFFSET, "PA_CL_VPORT_YOFFSET"},
      {PA_CL_VPORT_ZSCALE, "PA_CL_VPORT_ZSCALE"}, {PA_CL_VPORT_ZOFFSET, "PA_CL_VPORT_ZOFFSET"}, {CB_TARGET_MASK, "CB_TARGET_MASK"}, {CB_SHADER_MASK, "CB_SHADER_MASK"}, {CB_COLOR_CONTROL, "CB_COLOR_CONTROL"},
      {CB_COLOR0_BASE, "CB_COLOR0_BASE"}, {CB_COLOR0_PITCH, "CB_COLOR0_PITCH"}, {CB_COLOR0_VIEW, "CB_COLOR0_VIEW"}, {CB_COLOR0_INFO, "CB_COLOR0_INFO"}, {CB_COLOR0_ATTRIB, "CB_COLOR0_ATTRIB"}, {CB_COLOR0_BASE_EXT, "CB_COLOR0_BASE_EXT"},
      {SPI_PS_INPUT_ENA, "SPI_PS_INPUT_ENA"}, {SPI_PS_INPUT_ADDR, "SPI_PS_INPUT_ADDR"}, {VGT_SHADER_STAGES_EN, "VGT_SHADER_STAGES_EN"}, {PA_CL_VTE_CNTL, "PA_CL_VTE_CNTL"}, {PA_SU_SC_MODE_CNTL, "PA_SU_SC_MODE_CNTL"},
      {VGT_PRIMITIVE_TYPE, "VGT_PRIMITIVE_TYPE"}, {VGT_INDEX_TYPE, "VGT_INDEX_TYPE"}, {VGT_NUM_INSTANCES, "VGT_NUM_INSTANCES"},
  };
  for (const auto& e : t) if (e.reg == r) return e.name;
  static char ud[40];
  for (int i = 0; i < kUserDataCount; ++i) {
    if (r == SPI_SHADER_USER_DATA_PS_0 + i) { std::snprintf(ud, sizeof ud, "SPI_SHADER_USER_DATA_PS_%d", i); return ud; }
    if (r == SPI_SHADER_USER_DATA_VS_0 + i) { std::snprintf(ud, sizeof ud, "SPI_SHADER_USER_DATA_VS_%d", i); return ud; }
    if (r == SPI_SHADER_USER_DATA_GS_0 + i) { std::snprintf(ud, sizeof ud, "SPI_SHADER_USER_DATA_GS_%d", i); return ud; }
    if (r == COMPUTE_USER_DATA_0 + i) { std::snprintf(ud, sizeof ud, "COMPUTE_USER_DATA_%d", i); return ud; }
  }
  return nullptr;
}
}  // namespace pnr::graphics::amd
