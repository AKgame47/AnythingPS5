// Bounded two-axis loop memory proof. GPL-2.0.
// Used by the strict guarded dword-copy loop translator before lowering.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace pnr::shader {
struct LoopMemoryAccess {
  bool store = false;
  uint64_t origin = 0; // byte offset from aligned bounded SSBO guest base
  uint64_t lane_stride = 0;
  uint64_t iteration_stride = 0;
};
struct LoopMemoryProof {
  bool ok = false;
  std::string error;
  uint32_t checked_points = 0; // zero on failure
};
// Prove aligned/in-range uint32 accesses at origin + lane*lane_stride +
// iteration*iteration_stride. Limits: words 1..65536, lanes/iterations 1..64,
// accesses 1..32. All store points must be distinct and disjoint from ALL
// reads, across every lane, iteration and access. Read/read aliasing allowed.
// Pass iterations=64 for a runtime 1..64 count (proves the whole domain).
// No wrap, barriers, in-place updates, atomics, or sequential-dependence
// exceptions are inferred. This is not a sandbox or GPU runtime guard.
LoopMemoryProof prove_loop_memory(const std::vector<LoopMemoryAccess>& accesses,
                                 uint32_t buffer_words,uint32_t lanes,
                                 uint32_t iterations);
} // namespace pnr::shader
