// PNR RDNA instruction decoder - MODULE 04 shader pipeline, slice 2.
// Decodes RDNA (PS5 GPU ISA) instruction words into structured form.
// Original implementation; the encoding taxonomy (families, field layouts,
// operand numbering, opcode numbering) is adapted from AnyPS5's
// core/shader/recompiler/RdnaDecoder (boykopovar, GPL-2.0) - AnythingPS5 is
// GPL-2.0, attribution preserved here and in docs.
//
// Honest scope: family detection/word walking cover the listed RDNA
// encoding families; field extraction is partial; MNEMONICS exist for a real named subset (scalar families
// partial in both scalar and vector families). Opcodes
// outside the named subset decode structurally with supported=false -
// never named, never guessed. SDWA/DPP modifiers and MIMG/MUBUF payload
// fields other than the restricted FLAT store shape are Planned.
// Status: Implemented (named subset), Tested.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pnr::shader {

enum class RdnaFamily {
  Unknown,
  SOP1, SOP2, SOPK, SOPC, SOPP,
  VOP1, VOP2, VOP3, VOP3P, VOPC, VINTRP,
  SMEM, MUBUF, MTBUF, FLAT, DS, MIMG, EXP
};
const char* rdna_family_name(RdnaFamily f);

enum class RdnaOperandKind {
  None, ScalarReg, VectorReg,
  InlineIntConst, InlineFloatConst, LiteralConst,
  VccLo, VccHi, ExecLo, ExecHi, M0, Null, Scc, VccZ, ExecZ
};

struct RdnaOperand {
  RdnaOperandKind kind = RdnaOperandKind::None;
  uint32_t reg = 0;      // register number for ScalarReg/VectorReg
  uint32_t value = 0;    // raw value for constants
  int32_t signed_value = 0;
};

struct RdnaInstruction {
  uint32_t raw0 = 0, raw1 = 0;  // retained for strict modifier validation
  uint32_t memory_offset = 0, memory_segment = 0;
  uint32_t memory_data = 0, memory_addr = 0, memory_saddr = 0;
  uint32_t pc = 0;             // byte offset in the code stream
  RdnaFamily family = RdnaFamily::Unknown;
  uint32_t opcode_id = 0;      // raw opcode number inside the family
  std::string mnemonic;        // empty when not in the named subset
  bool supported = false;      // true only for the named subset
  RdnaOperand dst, src0, src1;
  int32_t simm = 0;            // SOPP/SOPK signed immediate
  uint32_t branch_target = 0;  // SOPP branches: pc + 4 + simm*4
  uint32_t word_count = 1;     // words consumed (2 for 64-bit / literal)
};

// One decoded instruction or the first honest error. Truncation and
// out-of-range words are errors; unsupported opcodes are not.
struct RdnaDecodeResult {
  bool ok = false;
  std::string error;
  std::vector<RdnaInstruction> instructions;
};

RdnaFamily rdna_family_of(uint32_t first_word);
RdnaDecodeResult rdna_decode(const std::vector<uint32_t>& code);

}  // namespace pnr::shader
