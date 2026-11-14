// PNR Graphics - named AMD GPU registers for the PM4 state tracker (Core slice C4b), GPL-2.0.
// Source of every number: the public Linux kernel headers
//   drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_3_0_offset.h   (register offsets, gfx10.3 / RDNA2 class)
//   drivers/gpu/drm/amd/include/sienna_cichlid_ip_offset.h        (GC segment bases 0x1260 and 0xA000)
// An absolute PM4 register number is the header's `mm` offset plus its segment base (BASE_IDX 0 -> 0x1260,
// BASE_IDX 1 -> 0xA000). The sums agree with the PM4 windows in soc15d.h (SH 0x2c00.., context 0xa000..).
// PrimitiveType values: public shadPS4 src/video_core/amdgpu/regs_vertex.h (values only, no code).
//
// Labeled: "per public AMD headers, unverified on PlayStation". A PlayStation GPU is RDNA2-class but
// not documented here; a register that does not show up where expected means the assumption is wrong.
#pragma once
#include <cstdint>

namespace pnr::graphics::amd {

constexpr std::uint32_t kShBase = 0x1260, kCtxBase = 0xA000;
constexpr std::uint32_t sh(std::uint32_t mm) { return mm + kShBase; }
constexpr std::uint32_t ctx(std::uint32_t mm) { return mm + kCtxBase; }

// SH registers (BASE_IDX 0). Program address registers hold the 256-byte-aligned address >> 8.
constexpr std::uint32_t SPI_SHADER_PGM_LO_PS = sh(0x19a8), SPI_SHADER_PGM_HI_PS = sh(0x19a9),
                        SPI_SHADER_PGM_RSRC1_PS = sh(0x19aa), SPI_SHADER_PGM_RSRC2_PS = sh(0x19ab), SPI_SHADER_USER_DATA_PS_0 = sh(0x19ac),
                        SPI_SHADER_PGM_LO_VS = sh(0x19e8), SPI_SHADER_PGM_HI_VS = sh(0x19e9),
                        SPI_SHADER_PGM_RSRC1_VS = sh(0x19ea), SPI_SHADER_PGM_RSRC2_VS = sh(0x19eb), SPI_SHADER_USER_DATA_VS_0 = sh(0x19ec),
                        SPI_SHADER_PGM_LO_GS = sh(0x1a28), SPI_SHADER_PGM_HI_GS = sh(0x1a29),
                        SPI_SHADER_PGM_RSRC1_GS = sh(0x1a2a), SPI_SHADER_PGM_RSRC2_GS = sh(0x1a2b), SPI_SHADER_USER_DATA_GS_0 = sh(0x1a2c),
                        COMPUTE_PGM_LO = sh(0x1bac), COMPUTE_PGM_HI = sh(0x1bad), COMPUTE_PGM_RSRC1 = sh(0x1bb2), COMPUTE_PGM_RSRC2 = sh(0x1bb3),
                        COMPUTE_USER_DATA_0 = sh(0x1be0);
constexpr int kUserDataCount = 16;

// Context registers (BASE_IDX 1).
constexpr std::uint32_t PA_SC_SCREEN_SCISSOR_TL = ctx(0x000c), PA_SC_SCREEN_SCISSOR_BR = ctx(0x000d),
                        PA_SC_VPORT_SCISSOR_0_TL = ctx(0x0094), PA_SC_VPORT_SCISSOR_0_BR = ctx(0x0095),
                        PA_CL_VPORT_XSCALE = ctx(0x010f), PA_CL_VPORT_XOFFSET = ctx(0x0110), PA_CL_VPORT_YSCALE = ctx(0x0111),
                        PA_CL_VPORT_YOFFSET = ctx(0x0112), PA_CL_VPORT_ZSCALE = ctx(0x0113), PA_CL_VPORT_ZOFFSET = ctx(0x0114),
                        CB_TARGET_MASK = ctx(0x008e), CB_SHADER_MASK = ctx(0x008f), CB_COLOR_CONTROL = ctx(0x0202),
                        CB_COLOR0_BASE = ctx(0x0318), CB_COLOR0_PITCH = ctx(0x0319), CB_COLOR0_VIEW = ctx(0x031b),
                        CB_COLOR0_INFO = ctx(0x031c), CB_COLOR0_ATTRIB = ctx(0x031d), CB_COLOR0_BASE_EXT = ctx(0x0390),
                        SPI_PS_INPUT_ENA = ctx(0x01b3), SPI_PS_INPUT_ADDR = ctx(0x01b4), VGT_SHADER_STAGES_EN = ctx(0x02d5),
                        PA_CL_VTE_CNTL = ctx(0x0206), PA_SU_SC_MODE_CNTL = ctx(0x0205);
// UCONFIG (BASE_IDX 1, inside the 0xc000 window).
constexpr std::uint32_t VGT_PRIMITIVE_TYPE = ctx(0x2242), VGT_INDEX_TYPE = ctx(0x2243), VGT_NUM_INSTANCES = ctx(0x224d);

enum class PrimitiveType : std::uint32_t { None = 0, PointList = 1, LineList = 2, LineStrip = 3, TriangleList = 4, TriangleFan = 5, TriangleStrip = 6 };

// Name of a known register or nullptr.
const char* register_name(std::uint32_t abs_reg);

}  // namespace pnr::graphics::amd
