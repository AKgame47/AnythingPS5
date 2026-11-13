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
  std::array<std::optional<uint32_t>, 256> regs{};
  bool ended = false, stored = false;
  uint32_t value = 0;
  for (const auto& i : d.instructions) {
    const auto at = " at pc=" + std::to_string(i.pc);
    if (ended) return fail("instructions after s_endpgm" + at);
    if (i.family == RdnaFamily::VOP1 && i.opcode_id == 1) {
      std::optional<uint32_t> v;
      if (i.src0.kind == RdnaOperandKind::LiteralConst ||
          i.src0.kind == RdnaOperandKind::InlineIntConst) v = i.src0.value;
      else if (i.src0.kind == RdnaOperandKind::VectorReg) v = regs[i.src0.reg];
      else return fail("unsupported v_mov source (SDWA/DPP/SGPR/float)" + at);
      if (!v) return fail("read of undefined VGPR" + at);
      regs[i.dst.reg] = *v;
    } else if (i.family == RdnaFamily::FLAT && i.opcode_id == 0x1c) {
      // Accept only the exact unmodified flat segment encoding. Reserved,
      // cache, LDS, offset and segment bits must all be zero. The scalar
      // address field must be the no-scalar sentinel; VDST/reserved zero.
      if (i.raw0 != (0xdc000000u | (0x1cu << 18u)) ||
          (i.raw1 & 0xffff0000u) != 0x007f0000u)
        return fail("unsupported FLAT modifiers/address mode" + at);
      if (stored) return fail("multiple stores are not supported" + at);
      if (i.memory_addr == 255) return fail("address register pair overflow" + at);
      auto lo = regs[i.memory_addr], hi = regs[i.memory_addr + 1];
      auto data = regs[i.memory_data];
      if (!lo || !hi || !data) return fail("undefined store address/data VGPR" + at);
      uint64_t addr = uint64_t(*lo) | (uint64_t(*hi) << 32u);
      if (addr != base) return fail("store address is outside the single-word test ABI" + at);
      stored = true; value = *data;
    } else if (i.family == RdnaFamily::SOPP && i.opcode_id == 1) {
      if (i.raw0 != 0xbf810000u) return fail("unsupported s_endpgm immediate" + at);
      ended = true;
    } else return fail("unsupported instruction " + std::string(rdna_family_name(i.family)) + at);
  }
  if (!ended) return fail("missing final s_endpgm");
  if (!stored) return fail("missing flat_store_dword");
  ComputeTranslation r;
  r.stored_value = value;
  r.spirv = graphics::SpirvModule::compute_write_u32(value);
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
