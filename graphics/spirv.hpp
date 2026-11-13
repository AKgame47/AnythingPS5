// PNR SPIR-V module emission - MODULE 04 first slice.
// A small, explicit word-level emitter for the module shapes PNR needs.
// Not a general compiler: each built shape is validated externally
// (spirv-val when PNR_SPIRV_VAL is set) and by real execution through the
// Vulkan backend in tests. Everything not emitted here is Planned, never
// faked.
// Status: Implemented (compute write-back shape), Tested.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pnr::graphics {

// Well-known SPIR-V constants (from the SPIR-V specification).
namespace spv {
inline constexpr uint32_t kMagic = 0x07230203;
inline constexpr uint32_t kVersion10 = 0x00010000;
// opcodes used by this emitter
inline constexpr uint16_t OpCapability = 17;
inline constexpr uint16_t OpMemoryModel = 14;
inline constexpr uint16_t OpEntryPoint = 15;
inline constexpr uint16_t OpExecutionMode = 16;
inline constexpr uint16_t OpDecorate = 71;
inline constexpr uint16_t OpMemberDecorate = 72;
inline constexpr uint16_t OpTypeVoid = 19;
inline constexpr uint16_t OpTypeInt = 21;
inline constexpr uint16_t OpTypeStruct = 30;
inline constexpr uint16_t OpTypePointer = 32;
inline constexpr uint16_t OpTypeFunction = 33;
inline constexpr uint16_t OpConstant = 43;
inline constexpr uint16_t OpFunction = 54;
inline constexpr uint16_t OpFunctionEnd = 56;
inline constexpr uint16_t OpVariable = 59;
inline constexpr uint16_t OpAccessChain = 65;
inline constexpr uint16_t OpStore = 62;
inline constexpr uint16_t OpLabel = 248;
inline constexpr uint16_t OpReturn = 253;
// enum values used by this emitter
inline constexpr uint32_t CapabilityShader = 1;
inline constexpr uint32_t AddressingLogical = 0;
inline constexpr uint32_t MemoryModelGLSL450 = 1;
inline constexpr uint32_t ExecModelGLCompute = 5;
inline constexpr uint32_t ExecModeLocalSize = 17;
inline constexpr uint32_t StorageUniform = 2;
inline constexpr uint32_t DecorBufferBlock = 3;
inline constexpr uint32_t DecorOffset = 35;
inline constexpr uint32_t DecorDescriptorSet = 34;
inline constexpr uint32_t DecorBinding = 33;
}  // namespace spv

// Small straight-line uint IR. Operands index earlier nodes (SSA), not
// guest registers. Load.a is a word index; Store.a is a value node and
// Store.b is a word index. Indices must be less than buffer_words. The emitter
// returns empty for malformed IR instead of emitting invalid SPIR-V.
enum class U32Op { Constant, Load, Add, Sub, And, Or, Xor, Store, GuardedLoad, GuardedStore, InvocationIndex, SelectEqual, UniformInput, FAdd, FSub, FMul, CarryAdd, BorrowSub, GuardedLoad64, GuardedStore64, LoopAdd, LoopAddUniform, LoopCopy, LoopCopySource, LoopCopyDest };
// GuardedLoad.a is a runtime guest low-address node. GuardedStore.a is
// data and .b is the address node. Invalid loads return 0; stores skip.
// SelectEqual: a==b ? c : d; emits structured branch + uint Phi.
// LoopAdd: a=initial, b=invariant delta, c=literal count 1..64; structured loop.
// LoopAddUniform.c is matching UniformInput node; requires loop_count_uniform
// parameter 0..15 and its entry guard. Sentinel 16 means no runtime-count guard.
// LoopCopy: a/b=source/dest low-address nodes, c=count or UniformInput node,
// d=0 literal count / 1 runtime, e/f=source/dest byte steps, g/h=FLAT offsets.
// Result=last loaded dword. LoopCopySource/Dest.a=earlier LoopCopy node,
// results are final low pointers. Only translator performs two-axis alias
// proof; emitter always guards each memory operation inside the loop.
struct U32Node { U32Op op; uint32_t a = 0, b = 0, c = 0, d = 0, e = 0, f = 0, g = 0, h = 0; };
class SpirvModule {
public:
  // Emit a compute shader with LocalSize 1x1x1 that stores `value` into
  // the first uint of the SSBO at set=0 binding=0. This is the minimal
  // end-to-end shape: enough to prove emission, validation, upload and
  // execution for real, and the honest base wider emission grows from.
  static std::vector<uint32_t> compute_write_u32(uint32_t value);
  static std::vector<uint32_t> compute_u32(const std::vector<U32Node>& nodes,
                                           uint32_t buffer_words = 1,
                                           uint32_t guest_base_low = 0,
                                           uint32_t local_size_x = 1,
                                           uint32_t uniform_words = 0,
                                           uint32_t guest_base_high = 0,
                                           uint32_t loop_count_uniform = 16);
};

// Pack a NUL-terminated string into SPIR-V words (little-endian, padded).
void spirv_pack_string(std::vector<uint32_t>& out, const std::string& s);

}  // namespace pnr::graphics
