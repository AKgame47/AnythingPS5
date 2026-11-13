#include "shader/translate.hpp"
#include "shader/rdna.hpp"
#include "graphics/spirv.hpp"
#include "graphics/vulkan.hpp"
#include <array>
#include <optional>
namespace pnr::shader {
ComputeTranslation translate_compute_words(const std::vector<uint32_t>& code,
                                           uint64_t base, uint32_t buffer_words) {
  auto fail = [](const std::string& msg) {
    ComputeTranslation r; r.error = msg; return r;
  };
  if (!buffer_words || buffer_words > 65536) return fail("buffer word count must be 1..65536");
  const uint64_t size = uint64_t(buffer_words) * 4;
  if (base > UINT64_MAX - (size - 1)) return fail("guest range overflows uint64");
  if (base % 4) return fail("guest base must be uint32 aligned");
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
  bool ended = false, stored = false, guarded = false;
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
      // ALU values are runtime SSA, even on constant inputs. Address
      // uses therefore go through emitted low-word guards below.
      regs[i.dst.reg] = Reg{static_cast<uint32_t>(nodes.size()-1), {}};
    } else if (i.family == RdnaFamily::FLAT &&
               (i.opcode_id == 0x1c || i.opcode_id == 0x0c)) {
      const bool load = i.opcode_id == 0x0c;
      const uint32_t expected = 0xdc000000u | (i.opcode_id << 18u);
      const uint32_t high_mask = load ? 0x00ffff00u : 0xffff0000u;
      // Load: VDST is allowed, VDATA must be zero. Store: VDST is zero.
      if ((i.raw0 & ~0x7ffu) != expected || (i.raw1 & high_mask) != 0x007f0000u)
        return fail("unsupported FLAT modifiers/address mode" + at);
      if (!load && stored) return fail("multiple stores are not supported" + at);
      if (i.memory_addr == 255) return fail("address register pair overflow" + at);
      auto lo = regs[i.memory_addr], hi = regs[i.memory_addr + 1];
      if (!lo || !hi || !hi->constant)
        return fail("undefined address or dynamic high address word" + at);
      bool dynamic = !lo->constant;
      uint32_t index = 0;
      if (dynamic) {
        // Bounded low-word indexing only. No implicit carry or truncation
        // of a 64-bit pointer; unsupported shapes fail at translation time.
        if (*hi->constant != uint32_t(base >> 32u))
          return fail("runtime address high word differs from binding" + at);
        if (uint64_t(uint32_t(base)) + size > 0x100000000ull)
          return fail("runtime binding crosses a 32-bit address boundary" + at);
        if (i.memory_offset)
          return fail("runtime address with immediate offset not supported" + at);
        guarded = true;
      } else {
        uint64_t addr = uint64_t(*lo->constant) | (uint64_t(*hi->constant) << 32u);
        const uint64_t offset = i.memory_offset;
        if (addr > UINT64_MAX - offset) return fail("address plus offset overflows" + at);
        addr += offset;
        if (addr < base || addr - base >= size || (addr - base) % 4)
          return fail("memory access is outside/alignment-invalid for SSBO range" + at);
        index = static_cast<uint32_t>((addr - base) / 4);
      }
      if (load) {
        nodes.push_back({dynamic ? U32Op::GuardedLoad : U32Op::Load,
                         dynamic ? lo->node : index});
        regs[i.dst.reg] = Reg{static_cast<uint32_t>(nodes.size()-1), {}};
      } else {
        auto data = regs[i.memory_data];
        if (!data) return fail("undefined store data VGPR" + at);
        nodes.push_back({dynamic ? U32Op::GuardedStore : U32Op::Store,
                         data->node, dynamic ? lo->node : index});
        stored = true; value = dynamic ? std::optional<uint32_t>{} : data->constant;
      }
    } else if (i.family == RdnaFamily::SOPP && i.opcode_id == 1) {
      if (i.raw0 != 0xbf810000u) return fail("unsupported s_endpgm immediate" + at);
      ended = true;
    } else return fail("unsupported instruction " + std::string(rdna_family_name(i.family)) + at);
  }
  if (!ended) return fail("missing final s_endpgm");
  if (!stored) return fail("missing flat_store_dword");
  ComputeTranslation r;
  r.guarded_memory = guarded;
  r.required_buffer_words = buffer_words;
  r.stored_value_known = value.has_value();
  r.stored_value = value.value_or(0); // meaningful only when known=true
  r.spirv = graphics::SpirvModule::compute_u32(nodes, buffer_words, uint32_t(base));
  if (r.spirv.empty()) return fail("invalid internal uint IR");
  r.ok = true;
  return r;
}
ComputeTranslation translate_compute_bytes(const std::vector<uint8_t>& code,
                                           uint64_t base, uint32_t buffer_words) {
  if (code.size() % 4) {
    ComputeTranslation r; r.error = "RDNA byte length is not a multiple of four"; return r;
  }
  std::vector<uint32_t> words;
  words.reserve(code.size() / 4);
  for (size_t i = 0; i < code.size(); i += 4)
    words.push_back(uint32_t(code[i]) | (uint32_t(code[i+1]) << 8u) |
                    (uint32_t(code[i+2]) << 16u) | (uint32_t(code[i+3]) << 24u));
  return translate_compute_words(words, base, buffer_words);
}
Status run_translated_compute(graphics::VulkanBackend& backend,
                              const ComputeTranslation& translation,
                              std::vector<uint32_t>* io) {
  if (!translation.ok || translation.spirv.empty() ||
      !translation.required_buffer_words || translation.required_buffer_words > 65536)
    return Status::Fail(101, "shader", "invalid translation for dispatch");
  if (!io || io->size() < translation.required_buffer_words)
    return Status::Fail(102, "shader", "host buffer shorter than translated SSBO range");
  return backend.run_compute_words(translation.spirv, io);
}
} // namespace pnr::shader
