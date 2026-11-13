#include "shader/translate.hpp"
#include "shader/rdna.hpp"
#include "graphics/spirv.hpp"
#include <array>
#include <optional>
namespace pnr::shader {
ComputeTranslation translate_compute_words(const std::vector<uint32_t>& code,
                                           uint64_t base) {
  auto fail = [](const std::string& msg) {
    ComputeTranslation r; r.error = msg; return r;
  };
  if (code.empty()) return fail("empty compute program");
  const auto d = rdna_decode(code);
  if (!d.ok) return fail("decode: " + d.error);
  using graphics::U32Op;
  using graphics::U32Node;
  struct Reg { uint32_t node; std::optional<uint32_t> constant; };
  std::array<std::optional<Reg>, 256> regs{};
  std::vector<U32Node> nodes;
  auto source = [&](const RdnaOperand& o) -> std::optional<Reg> {
    if (o.kind == RdnaOperandKind::VectorReg) return regs[o.reg];
    if (o.kind != RdnaOperandKind::LiteralConst &&
        o.kind != RdnaOperandKind::InlineIntConst) return {};
    nodes.push_back({U32Op::Constant, o.value});
    return Reg{static_cast<uint32_t>(nodes.size()-1), o.value};
  };
  bool ended = false, stored = false;
  std::optional<uint32_t> value;
  for (const auto& i : d.instructions) {
    const auto at = " at pc=" + std::to_string(i.pc);
    if (ended) return fail("instructions after s_endpgm" + at);
    if (i.family == RdnaFamily::VOP1 && i.opcode_id == 1) {
      const auto v = source(i.src0);
      if (!v) return fail("undefined VGPR or unsupported v_mov source" + at);
      regs[i.dst.reg] = *v; // SSA alias: later overwrites do not change this value
    } else if (i.family == RdnaFamily::VOP2 &&
               (i.opcode_id == 0x25 || i.opcode_id == 0x26 ||
                i.opcode_id == 0x1b || i.opcode_id == 0x1c || i.opcode_id == 0x1d)) {
      const auto a = source(i.src0), b = source(i.src1);
      if (!a || !b) return fail("undefined VGPR or unsupported ALU source" + at);
      U32Op op = U32Op::Add;
      switch (i.opcode_id) {
        case 0x26: op = U32Op::Sub; break;
        case 0x1b: op = U32Op::And; break;
        case 0x1c: op = U32Op::Or; break;
        case 0x1d: op = U32Op::Xor; break;
      }
      nodes.push_back({op, a->node, b->node});
      // Emit arithmetic even on constants. Conservatively disallow its
      // result as an address until address analysis is implemented.
      regs[i.dst.reg] = Reg{static_cast<uint32_t>(nodes.size()-1), {}};
    } else if (i.family == RdnaFamily::FLAT &&
               (i.opcode_id == 0x1c || i.opcode_id == 0x0c)) {
      const bool load = i.opcode_id == 0x0c;
      const uint32_t expected = 0xdc000000u | (i.opcode_id << 18u);
      const uint32_t high_mask = load ? 0x00ffff00u : 0xffff0000u;
      // Load: VDST is allowed, VDATA must be zero. Store: VDST is zero.
      if (i.raw0 != expected || (i.raw1 & high_mask) != 0x007f0000u)
        return fail("unsupported FLAT modifiers/address mode" + at);
      if (!load && stored) return fail("multiple stores are not supported" + at);
      if (i.memory_addr == 255) return fail("address register pair overflow" + at);
      auto lo = regs[i.memory_addr], hi = regs[i.memory_addr + 1];
      if (!lo || !hi || !lo->constant || !hi->constant)
        return fail("undefined/dynamic address VGPR pair" + at);
      uint64_t addr = uint64_t(*lo->constant) | (uint64_t(*hi->constant) << 32u);
      if (addr != base) return fail("address is outside the single-word test ABI" + at);
      if (load) {
        nodes.push_back({U32Op::Load});
        regs[i.dst.reg] = Reg{static_cast<uint32_t>(nodes.size()-1), {}};
      } else {
        auto data = regs[i.memory_data];
        if (!data) return fail("undefined store data VGPR" + at);
        nodes.push_back({U32Op::Store, data->node});
        stored = true; value = data->constant;
      }
    } else if (i.family == RdnaFamily::SOPP && i.opcode_id == 1) {
      if (i.raw0 != 0xbf810000u) return fail("unsupported s_endpgm immediate" + at);
      ended = true;
    } else return fail("unsupported instruction " + std::string(rdna_family_name(i.family)) + at);
  }
  if (!ended) return fail("missing final s_endpgm");
  if (!stored) return fail("missing flat_store_dword");
  ComputeTranslation r;
  r.stored_value_known = value.has_value();
  r.stored_value = value.value_or(0); // meaningful only when known=true
  r.spirv = graphics::SpirvModule::compute_u32(nodes);
  if (r.spirv.empty()) return fail("invalid internal uint IR");
  r.ok = true;
  return r;
}
ComputeTranslation translate_compute_bytes(const std::vector<uint8_t>& code,
                                           uint64_t base) {
  if (code.size() % 4) {
    ComputeTranslation r; r.error = "RDNA byte length is not a multiple of four"; return r;
  }
  std::vector<uint32_t> words;
  words.reserve(code.size() / 4);
  for (size_t i = 0; i < code.size(); i += 4)
    words.push_back(uint32_t(code[i]) | (uint32_t(code[i+1]) << 8u) |
                    (uint32_t(code[i+2]) << 16u) | (uint32_t(code[i+3]) << 24u));
  return translate_compute_words(words, base);
}
} // namespace pnr::shader
