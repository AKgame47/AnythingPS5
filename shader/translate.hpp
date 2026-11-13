// PNR restricted straight-line integer-compute translator. GPL-2.0.
// Experimental, Tested with synthetic instructions, not commercial shaders.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "core/runtime.hpp"
namespace pnr::graphics { class VulkanBackend; }
namespace pnr::shader {
struct ComputeTranslation {
  bool ok = false;
  std::string error;
  std::vector<uint32_t> spirv; // empty on EVERY failure
  bool full64_pointers = false; // runtime pair guard/carry-aware FLAT offset
  bool host_float32 = false; // Vulkan host FP policy, not PS5 exact modes
  uint32_t required_uniform_words = 0; // initial s0..sN-1, runtime push constants
  bool conditional_selection = false; // restricted forward SCC0 skip
  uint32_t local_size_x = 1; // one workgroup, explicit test invocation ABI
  bool guarded_memory = false; // invalid runtime loads=0 / stores skipped
  uint32_t required_buffer_words = 0; // host io must contain at least this many
  bool stored_value_known = false; // false for runtime-dependent results
  uint32_t stored_value = 0; // inspect only when stored_value_known
};
// Explicit test ABI: one workgroup, 1..64 invocations, straight-line VGPR SSA.
// With local_size_x>1, v0 starts as LocalInvocationIndex (0..size-1).
// Default size=1 preserves legacy undefined initial VGPRs. Multi-invocation
// memory must have provable aligned affine addresses; store stride=4 and
// input/output ranges cannot overlap. No shared-memory/barrier/wave model.
// Constant guest addresses + unsigned FLAT offsets are checked statically.
// Runtime low/high address pairs are guarded in single-invocation mode,
// including FLAT offset carry and bindings crossing a low-word boundary.
// OOB/misaligned/offset-overflow loads=0, stores skip. Scalar unsigned
// add/addc and sub/subb provide pair arithmetic; no GPU virtual memory.
// Multi-invocation mode retains conservative affine low-word restrictions.
// Host must bind at least required_buffer_words words; zero/oversize ranges
// and base+size overflow fail. Legacy callers default to a single word.
// Supports v_mov, flat_load/store_dword, uint32 add/sub/and/or/xor.
// Experimental unmodified VOP2 f32 add/sub/mul bitcasts raw registers to
// binary32, emits NoContraction, bitcasts result back. Host Vulkan default
// FP policy only: no PS5 rounding/denorm/NaN payload/signaling guarantee.
// Inline float selectors/SDWA/DPP/VOP3 modifiers remain unsupported.
// Conditional subset: readfirstlane to SGPR (single invocation only), s_mov_b32,
// s_cmp_eq_u32, one forward s_cbranch_scc0 over v_mov-only body. Registers
// assigned in body must also be defined on skipped path. Branch/Phi merge;
// no loops, nesting, branch memory effects or general control flow.
// Explicit uniform ABI: uniform_words=0..16 initializes s0..sN-1 from
// runtime Vulkan push constants, shared across invocations. Values are not
// baked into SPIR-V. Exact count required at checked dispatch. Not a Sony
// user-data/descriptor ABI. Initial SGPRs outside the declared range undefined.
// No wave/EXEC/asynchronous waitcnt semantics or real-game shader ABI.
ComputeTranslation translate_compute_words(const std::vector<uint32_t>& code,
                                           uint64_t ssbo_guest_base,
                                           uint32_t buffer_words = 1,
                                           uint32_t local_size_x = 1,
                                           uint32_t uniform_words = 0);
// Explicit little-endian byte input. Rejects partial words, never casts an
// unaligned byte buffer to uint32_t*. Same restricted ABI as above.
ComputeTranslation translate_compute_bytes(const std::vector<uint8_t>& code,
                                           uint64_t ssbo_guest_base,
                                           uint32_t buffer_words = 1,
                                           uint32_t local_size_x = 1,
                                           uint32_t uniform_words = 0);
// Checked execution route: refuses failed translations/null/short io before
// any Vulkan call. Use this instead of raw backend dispatch for translations.
Status run_translated_compute(graphics::VulkanBackend& backend,
                              const ComputeTranslation& translation,
                              std::vector<uint32_t>* io,
                              const std::vector<uint32_t>& uniforms = {});
} // namespace pnr::shader
