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

std::vector<uint32_t> SpirvModule::compute_u32(const std::vector<U32Node>& nodes,
                                                uint32_t buffer_words) {
  if (!buffer_words || buffer_words > 65536) return {};
  // SPIR-V 1.0 Uniform BufferBlock containing a fixed-size uint array.
  // IDs 1..12 retain the original emitter's meanings; 13=array type,
  // 14=array length. Every memory index is checked before emission.
  auto m = compute_write_u32(0);
  size_t function = 5;
  while (function < m.size() && (m[function] & 0xffffu) != spv::OpFunction)
    function += m[function] >> 16u;
  if (function >= m.size()) return {};
  m.resize(function);
  // Rebuild declarations in legal dependency order. Decorations precede types.
  size_t types = 5;
  while (types < m.size() && (m[types] & 0xffffu) != spv::OpTypeVoid)
    types += m[types] >> 16u;
  m.resize(types);
  inst(m, spv::OpDecorate, {13, 6, 4}); // ArrayStride = 6, uint stride 4
  inst(m, spv::OpTypeVoid, {4});
  inst(m, spv::OpTypeFunction, {5, 4});
  inst(m, spv::OpTypeInt, {6, 32, 0});
  inst(m, spv::OpConstant, {6, 14, buffer_words});
  inst(m, 28, {13, 6, 14}); // OpTypeArray (validated by spirv-val)
  inst(m, spv::OpTypeStruct, {3, 13});
  inst(m, spv::OpTypePointer, {7, spv::StorageUniform, 3});
  inst(m, spv::OpTypePointer, {8, spv::StorageUniform, 6});
  inst(m, spv::OpConstant, {6, 10, 0});
  inst(m, spv::OpVariable, {7, 2, spv::StorageUniform});
  uint32_t next = 15;
  std::vector<uint32_t> ids(nodes.size(), 0), indices(nodes.size(), 0);
  for (size_t i = 0; i < nodes.size(); ++i) {
    const auto& n = nodes[i];
    if (n.op == U32Op::Constant) {
      ids[i] = next++;
      inst(m, spv::OpConstant, {6, ids[i], n.a});
    } else if (n.op == U32Op::Load || n.op == U32Op::Store) {
      const uint32_t index = n.op == U32Op::Load ? n.a : n.b;
      if (index >= buffer_words) return {};
      indices[i] = next++;
      inst(m, spv::OpConstant, {6, indices[i], index});
    }
  }
  inst(m, spv::OpFunction, {4, 1, 0, 5});
  inst(m, spv::OpLabel, {11});
  for (size_t i = 0; i < nodes.size(); ++i) {
    const auto& n = nodes[i];
    if (n.op == U32Op::Constant) continue;
    uint32_t access = 0;
    if (n.op == U32Op::Load || n.op == U32Op::Store) {
      access = next++;
      inst(m, spv::OpAccessChain, {8, access, 2, 10, indices[i]});
    }
    if (n.op == U32Op::Load) {
      ids[i] = next++;
      inst(m, 61, {6, ids[i], access});
      continue;
    }
    if (n.a >= i || !ids[n.a]) return {};
    if (n.op == U32Op::Store) { inst(m, spv::OpStore, {access, ids[n.a]}); continue; }
    if (n.b >= i || !ids[n.b]) return {};
    uint16_t op = 0;
    switch (n.op) {
      case U32Op::Add: op = 128; break;
      case U32Op::Sub: op = 130; break;
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
