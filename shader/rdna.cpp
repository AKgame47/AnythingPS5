// See rdna.hpp for scope and attribution.
#include "shader/rdna.hpp"

#include <cstdio>

namespace pnr::shader {

const char* rdna_family_name(RdnaFamily f) {
  switch (f) {
    case RdnaFamily::SOP1: return "SOP1";
    case RdnaFamily::SOP2: return "SOP2";
    case RdnaFamily::SOPK: return "SOPK";
    case RdnaFamily::SOPC: return "SOPC";
    case RdnaFamily::SOPP: return "SOPP";
    case RdnaFamily::VOP1: return "VOP1";
    case RdnaFamily::VOP2: return "VOP2";
    case RdnaFamily::VOP3: return "VOP3";
    case RdnaFamily::VOP3P: return "VOP3P";
    case RdnaFamily::VOPC: return "VOPC";
    case RdnaFamily::VINTRP: return "VINTRP";
    case RdnaFamily::SMEM: return "SMEM";
    case RdnaFamily::MUBUF: return "MUBUF";
    case RdnaFamily::MTBUF: return "MTBUF";
    case RdnaFamily::FLAT: return "FLAT";
    case RdnaFamily::DS: return "DS";
    case RdnaFamily::MIMG: return "MIMG";
    case RdnaFamily::EXP: return "EXP";
    default: return "Unknown";
  }
}

// Family detection mirrors the RDNA encoding space (adapted from AnyPS5
// RdnaInstructionDecoder::GetRdnaInstructionFamily, GPL-2.0).
RdnaFamily rdna_family_of(uint32_t word) {
  if ((word & 0x80000000u) == 0u) {
    switch ((word >> 25u) & 0x3fu) {
      case 0x3eu: return RdnaFamily::VOPC;
      case 0x3fu: return RdnaFamily::VOP1;
      default: return RdnaFamily::VOP2;
    }
  }
  if ((word & 0xc0000000u) == 0x80000000u) {
    const uint32_t op = (word >> 23u) & 0x7fu;
    switch (op) {
      case 0x7du: return RdnaFamily::SOP1;
      case 0x7eu: return RdnaFamily::SOPC;
      case 0x7fu: return RdnaFamily::SOPP;
      default: return op >= 0x60u ? RdnaFamily::SOPK : RdnaFamily::SOP2;
    }
  }
  switch (word >> 26u) {
    case 0x32u: return RdnaFamily::VINTRP;
    case 0x33u: return RdnaFamily::VOP3P;
    case 0x35u: return RdnaFamily::VOP3;
    case 0x36u: return RdnaFamily::DS;
    case 0x37u: return RdnaFamily::FLAT;
    case 0x38u: return RdnaFamily::MUBUF;
    case 0x3au: return RdnaFamily::MTBUF;
    case 0x3cu: return RdnaFamily::MIMG;
    case 0x3du: return RdnaFamily::SMEM;
    case 0x3eu: return RdnaFamily::EXP;
    default: return RdnaFamily::Unknown;
  }
}

namespace {

bool is_two_word_family(RdnaFamily f) {
  switch (f) {
    case RdnaFamily::VOP3: case RdnaFamily::VOP3P: case RdnaFamily::DS:
    case RdnaFamily::FLAT: case RdnaFamily::MUBUF: case RdnaFamily::MTBUF:
    case RdnaFamily::MIMG: case RdnaFamily::SMEM:
      return true;
    default:
      return false;
  }
}

// Scalar source operand numbering (adapted from AnyPS5
// DecodeRdnaScalarSource, GPL-2.0).
RdnaOperand scalar_source(uint32_t code) {
  RdnaOperand o;
  if (code <= 105u) { o.kind = RdnaOperandKind::ScalarReg; o.reg = code; return o; }
  if (code >= 128u && code <= 192u) {
    o.kind = RdnaOperandKind::InlineIntConst;
    o.signed_value = static_cast<int32_t>(code - 128u);
    o.value = static_cast<uint32_t>(o.signed_value);
    return o;
  }
  if (code >= 193u && code <= 208u) {
    o.kind = RdnaOperandKind::InlineIntConst;
    o.signed_value = 192 - static_cast<int32_t>(code);
    o.value = static_cast<uint32_t>(o.signed_value);
    return o;
  }
  if (code >= 240u && code <= 247u) {
    // 0.5, -0.5, 1, -1, 2, -2, 4, -4 (stored as raw bits elsewhere; here
    // the index is enough for the analysis slice)
    o.kind = RdnaOperandKind::InlineFloatConst;
    o.value = code - 240u;
    o.signed_value = static_cast<int32_t>(code - 240u);
    return o;
  }
  if (code >= 256u && code <= 511u) {
    o.kind = RdnaOperandKind::VectorReg;
    o.reg = code - 256u;
    return o;
  }
  switch (code) {
    case 106u: o.kind = RdnaOperandKind::VccLo; return o;
    case 107u: o.kind = RdnaOperandKind::VccHi; return o;
    case 124u: o.kind = RdnaOperandKind::M0; return o;
    case 125u: o.kind = RdnaOperandKind::Null; return o;
    case 126u: o.kind = RdnaOperandKind::ExecLo; return o;
    case 127u: o.kind = RdnaOperandKind::ExecHi; return o;
    case 251u: o.kind = RdnaOperandKind::VccZ; return o;
    case 252u: o.kind = RdnaOperandKind::ExecZ; return o;
    case 253u: o.kind = RdnaOperandKind::Scc; return o;
    case 255u: o.kind = RdnaOperandKind::LiteralConst; return o;
    default: o.kind = RdnaOperandKind::None; return o;  // honestly unknown
  }
}

RdnaOperand scalar_dest(uint32_t code) {
  RdnaOperand o;
  if (code <= 105u) { o.kind = RdnaOperandKind::ScalarReg; o.reg = code; return o; }
  switch (code) {
    case 106u: o.kind = RdnaOperandKind::VccLo; return o;
    case 107u: o.kind = RdnaOperandKind::VccHi; return o;
    case 124u: o.kind = RdnaOperandKind::M0; return o;
    case 125u: o.kind = RdnaOperandKind::Null; return o;
    case 126u: o.kind = RdnaOperandKind::ExecLo; return o;
    case 127u: o.kind = RdnaOperandKind::ExecHi; return o;
    default: o.kind = RdnaOperandKind::None; return o;
  }
}

// Named opcode subset. Opcode NUMBERS are adapted from AnyPS5's decoder
// tables (GPL-2.0); AnythingPS5 names only what it can back.
struct OpName { uint32_t op; const char* name; };

const OpName kSop1[] = {
    {0x03, "s_mov_b32"}, {0x04, "s_mov_b64"}, {0x05, "s_cmov_b32"},
    {0x07, "s_not_b32"}, {0x0b, "s_brev_b32"},
    {0x0f, "s_bcnt1_i32_b32"}, {0x19, "s_sext_i32_i8"},
    {0x1a, "s_sext_i32_i16"}, {0x1f, "s_getpc_b64"},
    {0x20, "s_setpc_b64"}, {0x21, "s_swappc_b64"}, {0x22, "s_rfe_b64"},
    {0x24, "s_and_saveexec_b64"}, {0x25, "s_or_saveexec_b64"},
    {0x2e, "s_movrels_b32"}, {0x30, "s_movreld_b32"},
    {0x34, "s_abs_i32"},
};

const OpName kSop2[] = {
    {0x00, "s_add_u32"}, {0x01, "s_sub_u32"}, {0x02, "s_add_i32"},
    {0x03, "s_sub_i32"}, {0x04, "s_addc_u32"}, {0x05, "s_subb_u32"},
    {0x06, "s_min_i32"}, {0x07, "s_min_u32"}, {0x08, "s_max_i32"},
    {0x09, "s_max_u32"}, {0x0a, "s_cselect_b32"}, {0x0b, "s_cselect_b64"},
    {0x0e, "s_and_b32"}, {0x0f, "s_and_b64"}, {0x10, "s_or_b32"},
    {0x11, "s_or_b64"}, {0x12, "s_xor_b32"}, {0x13, "s_xor_b64"},
    {0x14, "s_andn2_b32"}, {0x16, "s_orn2_b32"}, {0x18, "s_nand_b32"},
    {0x1a, "s_nor_b32"}, {0x1c, "s_xnor_b32"},
    {0x1e, "s_lshl_b32"}, {0x20, "s_lshr_b32"}, {0x22, "s_ashr_i32"},
    {0x24, "s_bfm_b32"}, {0x26, "s_mul_i32"}, {0x27, "s_bfe_u32"},
};

const OpName kSopc[] = {
    {0x00, "s_cmp_eq_i32"}, {0x01, "s_cmp_lg_i32"}, {0x02, "s_cmp_gt_i32"},
    {0x03, "s_cmp_ge_i32"}, {0x04, "s_cmp_lt_i32"}, {0x05, "s_cmp_le_i32"},
    {0x06, "s_cmp_eq_u32"}, {0x07, "s_cmp_lg_u32"}, {0x08, "s_cmp_gt_u32"},
    {0x09, "s_cmp_ge_u32"}, {0x0a, "s_cmp_lt_u32"}, {0x0b, "s_cmp_le_u32"},
    {0x0c, "s_bitcmp0_b32"}, {0x0d, "s_bitcmp1_b32"},
    {0x12, "s_cmp_eq_u64"}, {0x13, "s_cmp_lg_u64"},
};

const OpName kSopk[] = {
    {0x00, "s_movk_i32"}, {0x01, "s_version"}, {0x02, "s_cmovk_i32"},
    {0x0f, "s_addk_i32"}, {0x10, "s_mulk_i32"},
    {0x12, "s_getreg_b32"}, {0x13, "s_setreg_b32"},
    {0x15, "s_setreg_imm32_b32"}, {0x16, "s_call_b64"},
    {0x17, "s_waitcnt"}, {0x18, "s_waitcnt"}, {0x19, "s_waitcnt"},
    {0x1a, "s_waitcnt"},
};

const OpName kSopp[] = {
    {0x00, "s_nop"}, {0x01, "s_endpgm"}, {0x02, "s_branch"},
    {0x03, "s_wakeup"}, {0x04, "s_cbranch_scc0"}, {0x05, "s_cbranch_scc1"},
    {0x06, "s_cbranch_vccz"}, {0x07, "s_cbranch_vccnz"},
    {0x08, "s_cbranch_execz"}, {0x09, "s_cbranch_execnz"},
    {0x0a, "s_barrier"}, {0x0c, "s_waitcnt"}, {0x0d, "s_sethalt"},
    {0x0e, "s_sleep"}, {0x0f, "s_setprio"}, {0x10, "s_sendmsg"},
    {0x11, "s_sendmsghalt"}, {0x12, "s_trap"}, {0x13, "s_icache_inv"},
    {0x22, "s_wait_idle"},
};

const OpName kVop1[] = {
    {0x00, "v_nop"}, {0x01, "v_mov_b32"}, {0x02, "v_readfirstlane_b32"},
    {0x05, "v_cvt_f32_i32"}, {0x06, "v_cvt_f32_u32"},
    {0x07, "v_cvt_u32_f32"}, {0x08, "v_cvt_i32_f32"},
    {0x0b, "v_cvt_f32_f16"}, {0x0c, "v_cvt_rpi_i32_f32"},
    {0x1b, "v_pipeflush"}, {0x20, "v_fract_f32"}, {0x23, "v_rndne_f32"},
    {0x24, "v_floor_f32"}, {0x25, "v_exp_f32"}, {0x27, "v_log_f32"},
    {0x2a, "v_rcp_f32"}, {0x2e, "v_rsq_f32"}, {0x33, "v_sqrt_f32"},
    {0x35, "v_sin_f32"}, {0x36, "v_cos_f32"},
};

const OpName kVop2[] = {
    {0x01, "v_cndmask_b32"}, {0x03, "v_add_f32"}, {0x04, "v_sub_f32"},
    {0x05, "v_subrev_f32"}, {0x08, "v_mul_f32"},
    {0x09, "v_mul_i32_i24"}, {0x0b, "v_mul_u32_u24"},
    {0x0f, "v_min_f32"}, {0x10, "v_max_f32"}, {0x11, "v_min_i32"},
    {0x12, "v_max_i32"}, {0x13, "v_min_u32"}, {0x14, "v_max_u32"},
    {0x16, "v_lshrrev_b32"}, {0x18, "v_ashrrev_i32"},
    {0x1a, "v_lshlrev_b32"}, {0x1b, "v_and_b32"}, {0x1c, "v_or_b32"},
    {0x1d, "v_xor_b32"}, {0x1e, "v_xnor_b32"}, {0x1f, "v_mac_f32"},
    {0x25, "v_add_nc_u32"}, {0x26, "v_sub_nc_u32"},
    {0x27, "v_subrev_nc_u32"}, {0x28, "v_addc_u32"},
    {0x29, "v_sub_co_ci_u32"},
};

const char* lookup(const OpName* table, size_t count, uint32_t op) {
  for (size_t i = 0; i < count; ++i)
    if (table[i].op == op) return table[i].name;
  return nullptr;
}

const char* family_mnemonic(RdnaFamily f, uint32_t op) {
  switch (f) {
    case RdnaFamily::SOP1: return lookup(kSop1, sizeof(kSop1)/sizeof(*kSop1), op);
    case RdnaFamily::SOP2: return lookup(kSop2, sizeof(kSop2)/sizeof(*kSop2), op);
    case RdnaFamily::SOPC: return lookup(kSopc, sizeof(kSopc)/sizeof(*kSopc), op);
    case RdnaFamily::SOPK: return lookup(kSopk, sizeof(kSopk)/sizeof(*kSopk), op);
    case RdnaFamily::SOPP: return lookup(kSopp, sizeof(kSopp)/sizeof(*kSopp), op);
    case RdnaFamily::VOP1: return lookup(kVop1, sizeof(kVop1)/sizeof(*kVop1), op);
    case RdnaFamily::VOP2: return lookup(kVop2, sizeof(kVop2)/sizeof(*kVop2), op);
    default: return nullptr;
  }
}

// If any source is a literal constant, the literal occupies the next word.
// Returns false when the literal word is missing (honest truncation error).
bool apply_literal(const std::vector<uint32_t>& code, size_t& wi,
                   RdnaInstruction& in, std::string& err) {
  for (RdnaOperand* o : {&in.src0, &in.src1}) {
    if (o->kind != RdnaOperandKind::LiteralConst) continue;
    if (wi + in.word_count >= code.size()) {
      err = "truncated literal constant after pc=" + std::to_string(in.pc);
      return false;
    }
    o->value = code[wi + in.word_count];
    o->signed_value = static_cast<int32_t>(o->value);
    ++in.word_count;
  }
  return true;
}

}  // namespace

RdnaDecodeResult rdna_decode(const std::vector<uint32_t>& code) {
  RdnaDecodeResult r;
  size_t wi = 0;
  while (wi < code.size()) {
    const uint32_t word = code[wi];
    RdnaInstruction in;
    in.raw0 = word;
    in.pc = static_cast<uint32_t>(wi * 4);
    in.family = rdna_family_of(word);
    if (in.family == RdnaFamily::Unknown) {
      r.error = "unknown RDNA encoding family at pc=" + std::to_string(in.pc) +
                " (word 0x" + [](uint32_t w) {
                  char b[16];
                  std::snprintf(b, sizeof b, "%08x", w);
                  return std::string(b);
                }(word) + "); not guessed";
      return r;
    }
    in.word_count = is_two_word_family(in.family) ? 2u : 1u;
    if (wi + in.word_count > code.size()) {
      r.error = "truncated " + std::string(rdna_family_name(in.family)) +
                " instruction at pc=" + std::to_string(in.pc);
      return r;
    }

    if (in.word_count == 2) in.raw1 = code[wi + 1];
    switch (in.family) {
      case RdnaFamily::SOPP: {
        in.opcode_id = (word >> 16u) & 0x7fu;
        in.simm = static_cast<int32_t>(static_cast<int16_t>(word & 0xffffu));
        in.branch_target = in.pc + 4u + static_cast<uint32_t>(in.simm * 4);
        break;
      }
      case RdnaFamily::SOP1: {
        in.opcode_id = (word >> 8u) & 0xffu;
        const uint32_t sdst = (word >> 16u) & 0x7fu;
        in.src0 = scalar_source(word & 0xffu);
        in.dst = scalar_dest(sdst);
        if (!apply_literal(code, wi, in, r.error)) return r;
        break;
      }
      case RdnaFamily::SOP2: {
        in.opcode_id = (word >> 23u) & 0x7fu;
        in.src0 = scalar_source(word & 0xffu);
        in.src1 = scalar_source((word >> 8u) & 0xffu);
        in.dst = scalar_dest((word >> 16u) & 0x7fu);
        if (!apply_literal(code, wi, in, r.error)) return r;
        break;
      }
      case RdnaFamily::SOPK: {
        in.opcode_id = (word >> 23u) & 0x1fu;
        in.dst = scalar_dest((word >> 16u) & 0x7fu);
        // compare/immediate ops sign-extend; add/mul-k treat as unsigned bits
        in.simm = static_cast<int32_t>(static_cast<int16_t>(word & 0xffffu));
        in.src0.kind = RdnaOperandKind::InlineIntConst;
        in.src0.signed_value = in.simm;
        in.src0.value = word & 0xffffu;
        break;
      }
      case RdnaFamily::SOPC: {
        in.opcode_id = (word >> 16u) & 0x7fu;
        in.src0 = scalar_source(word & 0xffu);
        in.src1 = scalar_source((word >> 8u) & 0xffu);
        in.dst.kind = RdnaOperandKind::Scc;
        if (!apply_literal(code, wi, in, r.error)) return r;
        break;
      }
      case RdnaFamily::VOP1: {
        in.opcode_id = (word >> 9u) & 0xffu;
        in.src0 = scalar_source(word & 0x1ffu);
        in.dst.kind = RdnaOperandKind::VectorReg;
        in.dst.reg = (word >> 17u) & 0xffu;
        if (in.opcode_id==2) in.dst=scalar_dest(in.dst.reg);
        if (!apply_literal(code, wi, in, r.error)) return r;
        break;
      }
      case RdnaFamily::VOP2: {
        in.opcode_id = (word >> 25u) & 0x3fu;
        in.src0 = scalar_source(word & 0x1ffu);
        in.src1.kind = RdnaOperandKind::VectorReg;
        in.src1.reg = (word >> 9u) & 0xffu;
        in.dst.kind = RdnaOperandKind::VectorReg;
        in.dst.reg = (word >> 17u) & 0xffu;
        if (!apply_literal(code, wi, in, r.error)) return r;
        break;
      }
      case RdnaFamily::VOPC: {
        in.opcode_id = (word >> 17u) & 0xffu;
        in.src0 = scalar_source(word & 0x1ffu);
        in.src1.kind = RdnaOperandKind::VectorReg;
        in.src1.reg = (word >> 9u) & 0xffu;
        in.dst.kind = RdnaOperandKind::VccLo;
        break;
      }
      case RdnaFamily::FLAT: {
        const uint32_t w1 = code[wi + 1];
        in.opcode_id = (word >> 18u) & 0x7fu;
        in.memory_offset = word & 0xfffu;
        in.memory_segment = (word >> 14u) & 3u;
        in.memory_addr = w1 & 0xffu;
        in.memory_data = (w1 >> 8u) & 0xffu;
        in.memory_saddr = (w1 >> 16u) & 0x7fu;
        if (in.opcode_id == 0x0cu) {
          in.dst.kind = RdnaOperandKind::VectorReg;
          in.dst.reg = w1 >> 24u;
          in.mnemonic = "flat_load_dword";
          in.supported = true;
        }
        if (in.opcode_id == 0x1cu) {
          in.mnemonic = "flat_store_dword";
          in.supported = true; // named/decoded, not general execution support
        }
        break;
      }
      case RdnaFamily::VOP3: {
        const uint32_t w1 = code[wi + 1];
        in.opcode_id = (word >> 16u) & 0x3ffu;
        in.dst.kind = RdnaOperandKind::VectorReg;
        in.dst.reg = word & 0xffu;
        in.src0 = scalar_source(w1 & 0x1ffu);
        in.src1 = scalar_source((w1 >> 9u) & 0x1ffu);
        break;
      }
      default:
        // Families recognized but not field-decoded yet (SMEM/MUBUF/...):
        // word count is still right, so program walking stays correct.
        in.opcode_id = is_two_word_family(in.family)
                           ? ((word >> 16u) & 0x3ffu)
                           : ((word >> 16u) & 0xffu);
        break;
    }

    if (const char* m = family_mnemonic(in.family, in.opcode_id)) {
      in.mnemonic = m;
      in.supported = true;
    }
    r.instructions.push_back(in);
    wi += in.word_count;
  }
  r.ok = true;
  return r;
}

}  // namespace pnr::shader
