#include "ape/ape.hpp"

#include <algorithm>
#include <cmath>

namespace pnr::ape {

PerfClass classify(const HardwareFacts& f) {
  if (f.cpu_logical_cores == 0) return PerfClass::Unknown;
  const double gib = f.ram_total_bytes ? static_cast<double>(*f.ram_total_bytes) / (1024.0*1024.0*1024.0) : 0.0;
  const unsigned c = f.cpu_logical_cores;
  if (c >= 12 && gib >= 32.0) return PerfClass::D_highend;
  if (c >= 8 && gib >= 16.0) return PerfClass::C_midrange;
  if (c >= 4 && gib >= 8.0) return PerfClass::B_low;
  return PerfClass::A_budget;
}

const char* perf_class_name(PerfClass c) {
  switch (c) {
    case PerfClass::A_budget: return "A (budget / ultra low-end)";
    case PerfClass::B_low: return "B (low-end)";
    case PerfClass::C_midrange: return "C (midrange)";
    case PerfClass::D_highend: return "D (high-end)";
    case PerfClass::Unknown: return "unknown";
  }
  return "unknown";
}

void FrameWindow::recompute() {
  if (frame_ms.empty()) { avg_ms = p95_ms = variance = 0.0; return; }
  double sum = 0.0;
  for (double v : frame_ms) sum += v;
  avg_ms = sum / frame_ms.size();
  double var = 0.0;
  for (double v : frame_ms) var += (v - avg_ms) * (v - avg_ms);
  variance = var / frame_ms.size();
  std::vector<double> sorted = frame_ms;
  std::sort(sorted.begin(), sorted.end());
  size_t idx = static_cast<size_t>(std::ceil(0.95 * sorted.size())) - 1;
  p95_ms = sorted[std::min(idx, sorted.size() - 1)];
}

const char* bottleneck_name(Bottleneck b) {
  switch (b) {
    case Bottleneck::none: return "none";
    case Bottleneck::gpu_suspected: return "gpu_suspected";
    case Bottleneck::cpu_suspected: return "cpu_suspected";
    case Bottleneck::memory: return "memory";
    case Bottleneck::unknown: return "unknown";
  }
  return "unknown";
}

Bottleneck AdaptiveController::classify_bottleneck(const Telemetry& t) const {
  if (t.frame_avg_ms <= obj_.target_ms()) return Bottleneck::none;
  if (t.cpu_util >= 0.9) return Bottleneck::cpu_suspected;
  if (t.mem_available_bytes && t.mem_total_bytes && *t.mem_total_bytes > 0 &&
      *t.mem_available_bytes < *t.mem_total_bytes / 20)
    return Bottleneck::memory;
  if (t.cpu_util >= 0.0) return Bottleneck::gpu_suspected;
  return Bottleneck::unknown;
}

Adjustment AdaptiveController::decide(const Telemetry& t, QualitySettings& s) {
  Adjustment a;
  if (pending_) return a;  // stabilize before another change
  const Bottleneck b = classify_bottleneck(t);
  if (b == Bottleneck::none || b == Bottleneck::unknown) return a;
  baseline_avg_ms_ = t.frame_avg_ms;
  a.changed = true;
  if (b == Bottleneck::cpu_suspected && s.background_tasks > 1) {
    a.field = "background_tasks";
    a.from = s.background_tasks;
    a.to = std::max(1, s.background_tasks / 2);
    a.reason = "cpu util >= 90%: reduce background work";
  } else if (b == Bottleneck::memory && s.resolution_scale_pct > 50) {
    a.field = "resolution_scale_pct";
    a.from = s.resolution_scale_pct;
    a.to = std::max(50, s.resolution_scale_pct - 10);
    a.reason = "memory pressure: reduce texture residency via scale";
  } else if (s.resolution_scale_pct > 50) {
    a.field = "resolution_scale_pct";
    a.from = s.resolution_scale_pct;
    a.to = std::max(50, s.resolution_scale_pct - 10);
    a.reason = "frame time above target: reduce render resolution";
  } else if (s.fps_cap > 30) {
    a.field = "fps_cap";
    a.from = s.fps_cap;
    a.to = 30;
    a.reason = "cannot hold target: restrict frame-rate cap";
  } else {
    a.changed = false;
    return a;
  }
  pending_ = true;
  last_ = a;
  return a;
}

void AdaptiveController::evaluate(const Telemetry& t, QualitySettings& s) {
  if (!pending_) return;
  pending_ = false;
  if (t.frame_avg_ms >= baseline_avg_ms_) {
    // No improvement: revert the change honestly.
    if (last_.field == "background_tasks") s.background_tasks = last_.from;
    else if (last_.field == "resolution_scale_pct") s.resolution_scale_pct = last_.from;
    else if (last_.field == "fps_cap") s.fps_cap = last_.from;
    last_.to = last_.from;
    last_.reason += " (reverted: no improvement)";
  }
}

}  // namespace pnr::ape
