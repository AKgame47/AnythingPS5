#include "laptop/laptop.hpp"

#include <algorithm>

namespace pnr::laptop {

const char* power_mode_name(PowerMode m) {
  switch (m) {
    case PowerMode::battery_saver: return "battery_saver";
    case PowerMode::balanced: return "balanced";
    case PowerMode::max_performance: return "max_performance";
  }
  return "balanced";
}

Caps caps_for(PowerMode mode) {
  Caps c;
  switch (mode) {
    case PowerMode::battery_saver:
      c.fps_cap = 30;                  // energy efficiency, stable frame rate
      c.min_resolution_scale_pct = 50;
      c.max_background_tasks = 1;
      break;
    case PowerMode::balanced:
      c.fps_cap = 60;
      c.min_resolution_scale_pct = 75;
      c.max_background_tasks = 2;
      break;
    case PowerMode::max_performance:
      c.fps_cap = 120;
      c.min_resolution_scale_pct = 100;
      c.max_background_tasks = 4;
      break;
  }
  return c;
}

PowerMode default_mode(const ape::HardwareFacts& f) {
  if (f.on_battery && *f.on_battery) return PowerMode::battery_saver;
  return PowerMode::balanced;  // plugged in OR unknown: balanced, never assumed AC
}

void apply_caps(const Caps& c, ape::QualitySettings& s) {
  s.fps_cap = std::min(s.fps_cap, c.fps_cap);
  s.resolution_scale_pct = std::max(s.resolution_scale_pct, c.min_resolution_scale_pct);
  s.background_tasks = std::min(s.background_tasks, c.max_background_tasks);
}

}  // namespace pnr::laptop
