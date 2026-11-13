#include "shader/translate.hpp"
#include "shader/rdna.hpp"
#include "graphics/spirv.hpp"
#include "graphics/vulkan.hpp"
#include <array>
#include <optional>
namespace pnr::shader {
ComputeTranslation translate_compute_words(const std::vector<uint32_t>& code,
                                           uint64_t base, uint32_t buffer_words, uint32_t local_size_x, uint32_t uniform_words) {
  auto fail = [](const std::string& msg) {
    ComputeTranslation r; r.error = msg; return r;
  };
  if(uniform_words>16) return fail("uniform count limited to 16 SGPR inputs");
  if (!local_size_x || local_size_x > 64) return fail("local size must be 1..64");
  if (!buffer_words || buffer_words > 65536) return fail("buffer word count must be 1..65536");
  const uint64_t size = uint64_t(buffer_words) * 4;
  if (base > UINT64_MAX - (size - 1)) return fail("guest range overflows uint64");
  if (base % 4) return fail("guest base must be uint32 aligned");
  if (code.empty()) return fail("empty compute program");
  const auto d = rdna_decode(code);
  if (!d.ok) return fail("decode: " + d.error);
  using graphics::U32Op;
  using graphics::U32Node;
  struct Reg {
    uint32_t node; std::optional<uint32_t> constant;
    std::optional<uint32_t> origin = {}, stride = {};
  };
  std::array<std::optional<Reg>, 256> regs{};
  std::array<std::optional<Reg>, 106> sregs{};
  std::vector<U32Node> nodes;
  // Only multi-invocation mode initializes v0. Legacy single mode keeps
  // undefined-v0 behavior. This is PNR test ABI, not a PS5 launch ABI.
  if (local_size_x > 1) {
    nodes.push_back({U32Op::InvocationIndex});
    regs[0]=Reg{0, {}, 0, 1};
  }
  for(uint32_t j=0;j<uniform_words;++j) {
    nodes.push_back({U32Op::UniformInput,j});
    sregs[j]=Reg{uint32_t(nodes.size()-1),{}};
  }
  struct Range { uint64_t first, last; };
  std::vector<Range> reads;
  std::optional<Range> writes;
  auto source = [&](const RdnaOperand& o) -> std::optional<Reg> {
    if (o.kind == RdnaOperandKind::VectorReg) return regs[o.reg];
    if (o.kind == RdnaOperandKind::ScalarReg && o.reg<sregs.size()) return sregs[o.reg];
    if (o.kind != RdnaOperandKind::LiteralConst &&
        o.kind != RdnaOperandKind::InlineIntConst) return {};
    nodes.push_back({U32Op::Constant, o.value});
    return Reg{static_cast<uint32_t>(nodes.size()-1), o.value, o.value, 0};
  };
  bool ended = false, stored = false, guarded = false, conditional = false, floating = false, full64 = false;
  struct Compare { uint32_t a,b; };
  Compare scc{};bool scc_defined=false;
  uint32_t carry_flag=0;bool carry_defined=false;
  bool merge_active=false;
  uint32_t merge_pc=0;
  std::array<std::optional<Reg>,256> before_branch{};
  std::array<bool,256> assigned{};
  Compare branch_compare{};
  std::optional<uint32_t> value;
  for (const auto& i : d.instructions) {
    const auto at = " at pc=" + std::to_string(i.pc);
    if (ended) return fail("instructions after s_endpgm" + at);
    if (merge_active && i.pc==merge_pc) {
      for (uint32_t reg=0;reg<256;++reg) if(assigned[reg]) {
        if(!before_branch[reg] || !regs[reg]) return fail("conditional VGPR undefined on skipped path"+at);
        nodes.push_back({U32Op::SelectEqual,branch_compare.a,branch_compare.b,
                         regs[reg]->node,before_branch[reg]->node});
        regs[reg]=Reg{uint32_t(nodes.size()-1),{}};
      }
      merge_active=false;
    }
    if (merge_active && !(i.family==RdnaFamily::VOP1 && i.opcode_id==1))
      return fail("conditional body permits only v_mov_b32"+at);
    if (i.family==RdnaFamily::VOP1 && i.opcode_id==2) {
      if(local_size_x!=1 || i.dst.kind!=RdnaOperandKind::ScalarReg ||
         i.src0.kind!=RdnaOperandKind::VectorReg || !regs[i.src0.reg])
        return fail("readfirstlane requires defined VGPR, ordinary SGPR, single invocation"+at);
      sregs[i.dst.reg]=regs[i.src0.reg];
    } else if(i.family==RdnaFamily::SOP1 && i.opcode_id==3) {
      auto v=source(i.src0);
      if(i.dst.kind!=RdnaOperandKind::ScalarReg || !v)
        return fail("unsupported/undefined scalar move"+at);
      sregs[i.dst.reg]=*v;
    } else if(i.family==RdnaFamily::SOP2 &&
              (i.opcode_id==0 || i.opcode_id==1 || i.opcode_id==4 || i.opcode_id==5)) {
      if(local_size_x!=1) return fail("scalar carry arithmetic restricted to single invocation"+at);
      auto a=source(i.src0),b=source(i.src1);
      const bool use_carry=i.opcode_id>=4, sub=i.opcode_id==1 || i.opcode_id==5;
      if(!a || !b || i.dst.kind!=RdnaOperandKind::ScalarReg || (use_carry && !carry_defined))
        return fail("undefined scalar operands/carry or unsupported destination"+at);
      uint32_t cin;
      if(use_carry)cin=carry_flag;
      else {nodes.push_back({U32Op::Constant,0});cin=uint32_t(nodes.size()-1);}
      nodes.push_back({sub?U32Op::Sub:U32Op::Add,a->node,b->node});
      auto tmp=uint32_t(nodes.size()-1);
      nodes.push_back({sub?U32Op::Sub:U32Op::Add,tmp,cin});
      sregs[i.dst.reg]=Reg{uint32_t(nodes.size()-1),{}};
      nodes.push_back({sub?U32Op::BorrowSub:U32Op::CarryAdd,a->node,b->node,cin});
      carry_flag=uint32_t(nodes.size()-1);carry_defined=true;scc_defined=false;
    } else if(i.family==RdnaFamily::SOPC && i.opcode_id==6) {
      if(local_size_x!=1) return fail("scalar comparison restricted to one invocation"+at);
      auto a=source(i.src0),b=source(i.src1);
      if(!a || !b) return fail("undefined compare source"+at);
      scc=Compare{a->node,b->node};scc_defined=true;carry_defined=false;
    } else if(i.family==RdnaFamily::SOPP && i.opcode_id==4) {
      if(local_size_x!=1 || conditional || !scc_defined || i.simm<=0)
        return fail("unsupported branch: needs single forward SCC0 with defined compare"+at);
      bool boundary=false;
      for(const auto& target:d.instructions) boundary |= target.pc==i.branch_target;
      if(!boundary) return fail("branch target is not an instruction boundary"+at);
      merge_active=true;merge_pc=i.branch_target;before_branch=regs;assigned.fill(false);
      branch_compare=scc;conditional=true;
    } else if (i.family == RdnaFamily::VOP1 && i.opcode_id == 1) {
      const auto v = source(i.src0);
      if (!v) return fail("undefined VGPR or unsupported v_mov source" + at);
      if(merge_active) assigned[i.dst.reg]=true;
      regs[i.dst.reg] = *v; // SSA alias: later overwrites do not change this value
    } else if (i.family==RdnaFamily::VOP2 &&
               (i.opcode_id==3 || i.opcode_id==4 || i.opcode_id==8)) {
      // Raw uint register storage, IEEE binary32 arithmetic under Vulkan's
      // host policy. No SDWA/DPP, source modifiers, denorm/rounding modes.
      auto a=source(i.src0), b=source(i.src1);
      if(!a || !b) return fail("undefined/unsupported float source"+at);
      const auto op=i.opcode_id==3?U32Op::FAdd:i.opcode_id==4?U32Op::FSub:U32Op::FMul;
      nodes.push_back({op,a->node,b->node});
      regs[i.dst.reg]=Reg{uint32_t(nodes.size()-1),{}};
      floating=true;
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
      Reg result{static_cast<uint32_t>(nodes.size()-1), {}};
      if (a->origin && a->stride && b->origin && b->stride) {
        uint64_t origin=0, stride=0; bool affine=false;
        if (op==U32Op::Add) {
          origin=uint64_t(*a->origin)+*b->origin;
          stride=uint64_t(*a->stride)+*b->stride; affine=true;
        } else if(op==U32Op::Sub && *a->origin>=*b->origin && *a->stride>=*b->stride) {
          origin=*a->origin-*b->origin; stride=*a->stride-*b->stride; affine=true;
        }
        if (affine && origin+stride*(local_size_x-1)<=UINT32_MAX) {
          result.origin=uint32_t(origin); result.stride=uint32_t(stride);
        }
      }
      regs[i.dst.reg] = result;
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
      if (!lo || !hi) return fail("undefined address VGPR pair"+at);
      bool wide_dynamic=!hi->constant;
      bool dynamic=wide_dynamic || !lo->constant;
      const bool use64=dynamic && (wide_dynamic || *hi->constant!=uint32_t(base>>32u) ||
                           uint64_t(uint32_t(base))+size>0x100000000ull || i.memory_offset);
      uint32_t index=0;
      if(dynamic)guarded=true;
      else {
        uint64_t addr=uint64_t(*lo->constant)|(uint64_t(*hi->constant)<<32u);
        if(addr>UINT64_MAX-i.memory_offset)return fail("address plus offset overflows"+at);
        addr+=i.memory_offset;
        if(addr<base || addr-base>=size || (addr-base)%4)
          return fail("memory access outside/alignment-invalid for SSBO range"+at);
        index=uint32_t((addr-base)/4);
      }
      full64 |= use64;
      if(use64 && local_size_x>1) return fail("64-bit dynamic pointer mode restricted to one invocation"+at);
      if (local_size_x > 1) {
        // Prove all invocation addresses before allowing parallel access.
        // This prevents output collisions and read/write data races in
        // the intentionally narrow one-store test ABI, without barriers.
        const uint64_t first=dynamic ? lo->origin.value_or(UINT32_MAX) :
                                       uint64_t(*lo->constant)+i.memory_offset;
        const uint64_t stride=dynamic ? lo->stride.value_or(UINT32_MAX) : 0;
        const uint64_t last=first+stride*(local_size_x-1);
        if ((dynamic && (!lo->origin || !lo->stride)) || first%4 || stride%4 ||
            first<uint32_t(base) || last>uint64_t(uint32_t(base))+size-4)
          return fail("multi-invocation address not provably affine/in-range" + at);
        if (load) reads.push_back({first,last});
        else {
          if(stride!=4) return fail("multi-invocation store requires disjoint stride-four addresses"+at);
          writes=Range{first,last};
        }
      }
      if (load) {
        if(use64)nodes.push_back({U32Op::GuardedLoad64,lo->node,hi->node,0,i.memory_offset});
        else nodes.push_back({dynamic ? U32Op::GuardedLoad : U32Op::Load,
                         dynamic ? lo->node : index});
        regs[i.dst.reg] = Reg{static_cast<uint32_t>(nodes.size()-1), {}};
      } else {
        auto data = regs[i.memory_data];
        if (!data) return fail("undefined store data VGPR" + at);
        if(use64)nodes.push_back({U32Op::GuardedStore64,lo->node,hi->node,data->node,i.memory_offset});
        else nodes.push_back({dynamic ? U32Op::GuardedStore : U32Op::Store,
                         data->node, dynamic ? lo->node : index});
        stored = true; value = dynamic ? std::optional<uint32_t>{} : data->constant;
      }
    } else if (i.family == RdnaFamily::SOPP && i.opcode_id == 1) {
      if (i.raw0 != 0xbf810000u) return fail("unsupported s_endpgm immediate" + at);
      ended = true;
    } else return fail("unsupported instruction " + std::string(rdna_family_name(i.family)) + at);
  }
  if (merge_active) return fail("unresolved conditional merge");
  if (!ended) return fail("missing final s_endpgm");
  if (!stored) return fail("missing flat_store_dword");
  if (local_size_x>1 && writes) for(const auto& read:reads)
    if(read.first<=writes->last && writes->first<=read.last)
      return fail("multi-invocation load/output ranges overlap; barriers not supported");
  ComputeTranslation r;
  r.full64_pointers=full64;
  r.host_float32=floating;
  r.required_uniform_words=uniform_words;
  r.conditional_selection = conditional;
  r.local_size_x = local_size_x;
  r.guarded_memory = guarded;
  r.required_buffer_words = buffer_words;
  r.stored_value_known = value.has_value();
  r.stored_value = value.value_or(0); // meaningful only when known=true
  r.spirv = graphics::SpirvModule::compute_u32(nodes, buffer_words, uint32_t(base), local_size_x, uniform_words, uint32_t(base>>32u));
  if (r.spirv.empty()) return fail("invalid internal uint IR");
  r.ok = true;
  return r;
}
ComputeTranslation translate_compute_bytes(const std::vector<uint8_t>& code,
                                           uint64_t base, uint32_t buffer_words, uint32_t local_size_x, uint32_t uniform_words) {
  if (code.size() % 4) {
    ComputeTranslation r; r.error = "RDNA byte length is not a multiple of four"; return r;
  }
  std::vector<uint32_t> words;
  words.reserve(code.size() / 4);
  for (size_t i = 0; i < code.size(); i += 4)
    words.push_back(uint32_t(code[i]) | (uint32_t(code[i+1]) << 8u) |
                    (uint32_t(code[i+2]) << 16u) | (uint32_t(code[i+3]) << 24u));
  return translate_compute_words(words, base, buffer_words, local_size_x, uniform_words);
}
Status run_translated_compute(graphics::VulkanBackend& backend,
                              const ComputeTranslation& translation,
                              std::vector<uint32_t>* io,
                              const std::vector<uint32_t>& uniforms) {
  if (!translation.ok || translation.spirv.empty() ||
      !translation.local_size_x || translation.local_size_x>64 ||
      !translation.required_buffer_words || translation.required_buffer_words > 65536)
    return Status::Fail(101, "shader", "invalid translation for dispatch");
  if(translation.required_uniform_words>16 || uniforms.size()!=translation.required_uniform_words)
    return Status::Fail(103,"shader","uniform input count does not match translation");
  if (!io || io->size() < translation.required_buffer_words)
    return Status::Fail(102, "shader", "host buffer shorter than translated SSBO range");
  return backend.run_compute_words(translation.spirv, io, uniforms);
}
} // namespace pnr::shader
