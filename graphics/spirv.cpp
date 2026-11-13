#include "graphics/spirv.hpp"

namespace pnr::graphics {

void spirv_pack_string(std::vector<uint32_t>& out, const std::string& s) {
  const size_t words = (s.size() + 4) / 4;  // includes NUL padding
  const size_t base = out.size();
  out.resize(base + words, 0);
  for (size_t i = 0; i < s.size(); ++i) {
    out[base + i / 4] |= static_cast<uint32_t>(
        static_cast<unsigned char>(s[i])) << ((i % 4) * 8);
  }
}

namespace {
void inst(std::vector<uint32_t>& out, uint16_t op,
          std::initializer_list<uint32_t> ops) {
  out.push_back((static_cast<uint32_t>(ops.size() + 1) << 16) | op);
  for (uint32_t w : ops) out.push_back(w);
}
}  // namespace

std::vector<uint32_t> SpirvModule::compute_write_u32(uint32_t value) {
  // Layout (SPIR-V 1.0, Vulkan 1.0 SSBO rules: Uniform + BufferBlock):
  //   struct Buf { uint x; };  set 0 binding 0
  //   void main() { buf.x = value; }
  // ids: 1=main 2=buf-var 3=Buf-struct 4=void 5=voidfn 6=uint 7=ptr-u-Buf
  //      8=ptr-u-uint 9=const-value 10=const-zero 11=label 12=access
  std::vector<uint32_t> m;
  m.reserve(64);
  // header patched after ids are known
  m.push_back(spv::kMagic);
  m.push_back(spv::kVersion10);
  m.push_back(0);          // generator: none (hand-emitted)
  m.push_back(13);         // bound: max id + 1
  m.push_back(0);          // schema
  inst(m, spv::OpCapability, {spv::CapabilityShader});
  inst(m, spv::OpMemoryModel, {spv::AddressingLogical, spv::MemoryModelGLSL450});
  {
    // OpEntryPoint GLCompute %1 "main"
    // (interface list holds only Input/Output variables; the SSBO is
    // Uniform storage and must NOT be listed - spirv-val enforces this)
    std::vector<uint32_t> ep;
    ep.push_back(spv::ExecModelGLCompute);
    ep.push_back(1);
    spirv_pack_string(ep, "main");
    m.push_back((static_cast<uint32_t>(ep.size() + 1) << 16) | spv::OpEntryPoint);
    for (uint32_t w : ep) m.push_back(w);
  }
  inst(m, spv::OpExecutionMode, {1, spv::ExecModeLocalSize, 1, 1, 1});
  inst(m, spv::OpDecorate, {3, spv::DecorBufferBlock});
  inst(m, spv::OpMemberDecorate, {3, 0, spv::DecorOffset, 0});
  inst(m, spv::OpDecorate, {2, spv::DecorDescriptorSet, 0});
  inst(m, spv::OpDecorate, {2, spv::DecorBinding, 0});
  inst(m, spv::OpTypeVoid, {4});
  inst(m, spv::OpTypeFunction, {5, 4});
  inst(m, spv::OpTypeInt, {6, 32, 0});
  inst(m, spv::OpTypeStruct, {3, 6});
  inst(m, spv::OpTypePointer, {7, spv::StorageUniform, 3});
  inst(m, spv::OpTypePointer, {8, spv::StorageUniform, 6});
  inst(m, spv::OpConstant, {6, 9, value});
  inst(m, spv::OpConstant, {6, 10, 0});
  inst(m, spv::OpVariable, {7, 2, spv::StorageUniform});
  inst(m, spv::OpFunction, {4, 1, 0, 5});
  inst(m, spv::OpLabel, {11});
  inst(m, spv::OpAccessChain, {8, 12, 2, 10});
  inst(m, spv::OpStore, {12, 9});
  inst(m, spv::OpReturn, {});
  inst(m, spv::OpFunctionEnd, {});
  return m;
}

std::vector<uint32_t> SpirvModule::compute_u32(const std::vector<U32Node>& nodes) {
  // Reuse the validated declarations, replacing the function body. IDs
  // 1..12 are reserved by compute_write_u32; new SSA values start at 13.
  auto m = compute_write_u32(0);
  size_t function = 5;
  while (function < m.size() && (m[function] & 0xffffu) != spv::OpFunction)
    function += m[function] >> 16u;
  if (function >= m.size()) return {};
  m.resize(function);
  uint32_t next = 13;
  std::vector<uint32_t> ids(nodes.size(), 0);
  for (size_t i = 0; i < nodes.size(); ++i) {
    const auto& n = nodes[i];
    if (n.op == U32Op::Constant) {
      ids[i] = next++;
      inst(m, spv::OpConstant, {6, ids[i], n.a});
    }
  }
  inst(m, spv::OpFunction, {4, 1, 0, 5});
  inst(m, spv::OpLabel, {11});
  inst(m, spv::OpAccessChain, {8, 12, 2, 10});
  for (size_t i = 0; i < nodes.size(); ++i) {
    const auto& n = nodes[i];
    if (n.op == U32Op::Constant) continue;
    if (n.op == U32Op::Load) {
      ids[i] = next++;
      inst(m, 61, {6, ids[i], 12}); // OpLoad, verified with spirv-as
      continue;
    }
    if (n.a >= i || !ids[n.a]) return {};
    if (n.op == U32Op::Store) { inst(m, spv::OpStore, {12, ids[n.a]}); continue; }
    if (n.b >= i || !ids[n.b]) return {};
    uint16_t op = 0;
    switch (n.op) {
      case U32Op::Add: op = 128; break; // OpIAdd
      case U32Op::Sub: op = 130; break; // OpISub
      case U32Op::And: op = 199; break;
      case U32Op::Or: op = 197; break;
      case U32Op::Xor: op = 198; break;
      default: return {};
    }
    ids[i] = next++;
    inst(m, op, {6, ids[i], ids[n.a], ids[n.b]});
  }
  inst(m, spv::OpReturn, {});
  inst(m, spv::OpFunctionEnd, {});
  m[3] = next;
  return m;
}

}  // namespace pnr::graphics
