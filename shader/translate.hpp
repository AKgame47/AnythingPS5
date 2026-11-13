// PNR restricted constant-compute translator. GPL-2.0.
// Experimental, Tested with synthetic instructions, not commercial shaders.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace pnr::shader {
struct ComputeTranslation {
  bool ok = false;
  std::string error;
  std::vector<uint32_t> spirv; // empty on EVERY failure
  uint32_t stored_value = 0;
};
// Explicit test ABI: one active invocation, straight-line constant VGPRs,
// one flat_store_dword to exactly ssbo_guest_base, final s_endpgm.
// Guest address base is supplied by the caller, NOT a host pointer. It maps
// only to uint 0 of descriptor set 0 binding 0. No GPU virtual-memory mapping,
// wave semantics, resource descriptors, dynamic addressing, or real-game ABI.
// Moves are constant-folded before using the compute_write_u32 emitter.
ComputeTranslation translate_compute_words(const std::vector<uint32_t>& code,
                                           uint64_t ssbo_guest_base);
// Explicit little-endian byte input. Rejects partial words, never casts an
// unaligned byte buffer to uint32_t*. Same restricted ABI as above.
ComputeTranslation translate_compute_bytes(const std::vector<uint8_t>& code,
                                           uint64_t ssbo_guest_base);
} // namespace pnr::shader
