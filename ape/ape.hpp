// PNR APE - MODULE 05, Adaptive Performance Engine (v1)
// Deterministic rules only: the master prompt defers ML-based tuning until
// real benchmarking data exists. Hardware classification is an honest
// heuristic from measured host facts - GPU facts are unavailable without a
// Vulkan probe (Blocked, Module 04), so classification is CPU/RAM-based and
// marked partial. Status: Implemented, Tested.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pnr::ape {

enum class PerfClass { A_budget, B_low, C_midrange, D_highend, Unknown };

struct HardwareFacts {
  unsigned cpu_logical_cores = 0;
  std::optional<uint64_t> ram_total_bytes;
  std::optional<uint64_t> ram_available_bytes;
  std::optional<bool> on_battery;  // nullopt = unknown (desktop, or unreadable)
};

// CPU/RAM heuristic only (GPU unknown in this build). Documented boundaries:
// D: >=12 logical cores and >=32 GiB RAM; C: >=8 and >=16 GiB;
// B: >=4 and >=8 GiB; otherwise A. Unknown RAM never upgrades a class.
PerfClass classify(const HardwareFacts& f);
const char* perf_class_name(PerfClass c);

// Rolling frame-time statistics for the controller.
struct FrameWindow {
  std::vector<double> frame_ms;
  double avg_ms = 0.0;
  double p95_ms = 0.0;
  double variance = 0.0;
  void recompute();
};

struct PerformanceObjective {
  double target_fps = 60.0;
  double target_ms() const { return 1000.0 / target_fps; }
};

enum class Bottleneck { none, gpu_suspected, cpu_suspected, memory, unknown };
const char* bottleneck_name(Bottleneck b);

struct Telemetry {
  double frame_avg_ms = 0.0;
  double cpu_util = -1.0;  // 0..1, negative = unknown (not fabricated)
  std::optional<uint64_t> mem_available_bytes;
  std::optional<uint64_t> mem_total_bytes;
  std::optional<bool> on_battery;
};

struct QualitySettings {
  int resolution_scale_pct = 100;  // 50..100
  int effects_level = 2;           // 0..3
  int background_tasks = 4;        // allowed background workers, >=1
  int fps_cap = 60;                // 30/40/60/120
};

struct Adjustment {
  bool changed = false;
  std::string field;
  int from = 0;
  int to = 0;
  std::string reason;
};

// Deterministic closed-loop controller: at most one limited change per
// decision, an explicit stabilization window, then keep or revert based on
// re-measurement (anti-oscillation, per the master prompt).
class AdaptiveController {
public:
  explicit AdaptiveController(PerformanceObjective obj) : obj_(obj) {}
  Bottleneck classify_bottleneck(const Telemetry& t) const;
  Adjustment decide(const Telemetry& t, QualitySettings& s);
  // Call after stabilization_frames with fresh telemetry; reverts the last
  // change if it did not improve frame time.
  void evaluate(const Telemetry& t, QualitySettings& s);
  bool pending() const { return pending_; }
  int stabilization_frames = 120;
private:
  PerformanceObjective obj_;
  bool pending_ = false;
  Adjustment last_;
  double baseline_avg_ms_ = 0.0;
};

}  // namespace pnr::ape
