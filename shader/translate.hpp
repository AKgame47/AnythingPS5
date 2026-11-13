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
  bool guarded_memory = false; // invalid runtime loads=0 / stores skipped
  uint32_t required_buffer_words = 0; // host io must contain at least this many
  bool stored_value_known = false; // false for runtime-dependent results
  uint32_t stored_value = 0; // inspect only when stored_value_known
};
// Explicit test ABI: one active invocation, straight-line VGPR SSA.
// Constant guest addresses + unsigned FLAT offsets are checked statically.
// Runtime low-word addresses (constant high word matching base) are guarded
// in emitted SPIR-V: OOB/misaligned loads=0, stores skip. Runtime immediates
// must be zero and the binding cannot cross a 32-bit low-word boundary.
// Not full 64-bit dynamic addressing or GPU virtual memory.
// Host must bind at least required_buffer_words words; zero/oversize ranges
// and base+size overflow fail. Legacy callers default to a single word.
// Supports v_mov, flat_load/store_dword, uint32 add/sub/and/or/xor.
// No wave/EXEC/asynchronous waitcnt semantics or real-game shader ABI.
ComputeTranslation translate_compute_words(const std::vector<uint32_t>& code,
                                           uint64_t ssbo_guest_base,
                                           uint32_t buffer_words = 1);
// Explicit little-endian byte input. Rejects partial words, never casts an
// unaligned byte buffer to uint32_t*. Same restricted ABI as above.
ComputeTranslation translate_compute_bytes(const std::vector<uint8_t>& code,
                                           uint64_t ssbo_guest_base,
                                           uint32_t buffer_words = 1);
// Checked execution route: refuses failed translations/null/short io before
// any Vulkan call. Use this instead of raw backend dispatch for translations.
Status run_translated_compute(graphics::VulkanBackend& backend,
                              const ComputeTranslation& translation,
                              std::vector<uint32_t>* io);
} // namespace pnr::shader
