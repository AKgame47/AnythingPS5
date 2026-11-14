// Core slice C4b tests: named registers and draw-state extraction on SYNTHETIC command buffers.
#include <cstdio>
#include <cstring>
#include <string>

#include "graphics/amd_regs.hpp"
#include "graphics/gfx_state.hpp"

void pnr_gfx_state_tests(int& pass, int& fail) {
  using namespace pnr::graphics;
  using namespace pnr::graphics::amd;
  auto check = [&](bool ok, const std::string& l) { if (ok) ++pass; else ++fail; std::printf("%s %s\n", ok ? "PASS" : "FAIL", l.c_str()); };
  auto f2u = [](float f) { std::uint32_t u; std::memcpy(&u, &f, 4); return u; };

  // Absolute numbers: mm offset + segment base, matching the kernel header values.
  check(SPI_SHADER_PGM_LO_PS == 0x2c08 && SPI_SHADER_PGM_LO_VS == 0x2c48 && SPI_SHADER_PGM_LO_GS == 0x2c88 && COMPUTE_PGM_LO == 0x2e0c, "regs: SH absolute numbers (0x19a8+0x1260 = 0x2c08 and so on)");
  check(CB_COLOR0_BASE == 0xa318 && PA_CL_VPORT_XSCALE == 0xa10f && CB_TARGET_MASK == 0xa08e && VGT_PRIMITIVE_TYPE == 0xc242, "regs: context and uconfig absolute numbers");
  check(SPI_SHADER_PGM_LO_PS >= pnr::graphics::pm4::kShStart && SPI_SHADER_USER_DATA_PS_0 + 15 < pnr::graphics::pm4::kShEnd && CB_COLOR0_BASE_EXT < pnr::graphics::pm4::kContextEnd && VGT_PRIMITIVE_TYPE >= pnr::graphics::pm4::kUconfigStart && VGT_PRIMITIVE_TYPE < pnr::graphics::pm4::kUconfigEnd, "regs: numbers sit inside the PM4 windows");
  check(std::string(register_name(CB_COLOR0_BASE)) == "CB_COLOR0_BASE" && std::string(register_name(SPI_SHADER_USER_DATA_VS_0 + 3)) == "SPI_SHADER_USER_DATA_VS_3" && register_name(0x1234) == nullptr, "regs: names resolve, unknown numbers do not");

  // A command buffer that sets up a draw.
  pm4::Builder b;
  b.set_reg(pm4::SET_SH_REG, SPI_SHADER_PGM_LO_VS, {0x1000, 0});
  b.set_reg(pm4::SET_SH_REG, SPI_SHADER_PGM_RSRC1_VS, {0xA1, 0xA2});
  b.set_reg(pm4::SET_SH_REG, SPI_SHADER_USER_DATA_VS_0, {0xC0, 0xC1});
  b.set_reg(pm4::SET_SH_REG, SPI_SHADER_PGM_LO_PS, {0x2000, 0});
  b.set_reg(pm4::SET_SH_REG, SPI_SHADER_USER_DATA_PS_0, {0xD0});
  b.set_reg(pm4::SET_CONTEXT_REG, PA_CL_VPORT_XSCALE, {f2u(960.f), f2u(960.f), f2u(-540.f), f2u(540.f), f2u(0.5f), f2u(0.5f)});
  b.set_context(CB_COLOR0_BASE, 0x4000).set_context(CB_COLOR0_BASE_EXT, 1).set_context(CB_COLOR0_PITCH, 239).set_context(CB_TARGET_MASK, 0xf);
  b.set_context(PA_SC_SCREEN_SCISSOR_BR, (1080u << 16) | 1920u);
  b.set_uconfig(VGT_PRIMITIVE_TYPE, 4).draw_auto(3);
  const auto s = pm4::interpret(b.dwords().data(), b.dwords().size());
  check(s.draws.size() == 1 && s.issues.empty(), "state: the buffer decodes with no issues (SH, context and uconfig windows)");
  const DrawBindings d = extract_bindings(s.draws[0]);
  check(d.vs.present && d.vs.address == 0x100000 && d.vs.rsrc1 == 0xA1 && d.vs.rsrc2 == 0xA2 && d.vs.user_data[0] == 0xC0 && d.vs.user_data[1] == 0xC1 && d.vs.user_data_set[1] && !d.vs.user_data_set[2], "state: vertex shader address (<< 8), resources and user data");
  check(d.ps.present && d.ps.address == 0x200000 && d.ps.user_data[0] == 0xD0 && !d.gs.present, "state: pixel shader found, geometry stage absent");
  check(d.viewport.present && d.viewport.xscale == 960.f && d.viewport.yscale == -540.f && d.viewport.yoffset == 540.f && d.viewport.zscale == 0.5f, "state: viewport floats decoded from raw register bits");
  check(d.color0.present && d.color0.address == ((1ull << 40) | (0x4000ull << 8)) && d.color0.pitch == 239 && d.color0.target_mask == 0xf, "state: color target 0 address from BASE and BASE_EXT, pitch, mask");
  check(d.primitive == PrimitiveType::TriangleList && d.has_scissor && (d.screen_scissor_br & 0xffff) == 1920, "state: primitive type and screen scissor");
  check(d.missing.empty() && d.vertex_stage() == &d.vs, "state: nothing missing for a minimal draw");

  // Missing pieces are reported in words, not silently defaulted.
  pm4::Builder e; e.set_reg(pm4::SET_SH_REG, SPI_SHADER_PGM_LO_GS, {0x30, 0}).draw_auto(3);
  const auto s2 = pm4::interpret(e.dwords().data(), e.dwords().size());
  const DrawBindings d2 = extract_bindings(s2.draws[0]);
  check(d2.vertex_stage() == &d2.gs && d2.gs.address == 0x3000 && d2.missing.find("pixel shader") != std::string::npos && d2.missing.find("color target") != std::string::npos && d2.missing.find("viewport") != std::string::npos && d2.missing.find("primitive") != std::string::npos, "state: a GS-only draw reports every missing piece");
  const std::string text = describe_bindings(d2);
  check(text.find("MISSING for a draw") != std::string::npos && text.find("unverified on PlayStation") != std::string::npos, "state: the description carries the verification label");
}
