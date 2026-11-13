// PNR Laptop Optimization Manager - MODULE 06 (v1)
// Resolves effective caps from a power mode. Thermal/refresh-rate telemetry
// is reported as unknown when the host cannot provide it (never fabricated).
// Status: Implemented, Tested.
#pragma once
#include <optional>
#include <string>
#include "ape/ape.hpp"

namespace pnr::laptop {

enum class PowerMode { battery_saver, balanced, max_performance };
const char* power_mode_name(PowerMode m);

struct Caps {
  int fps_cap = 60;
  int min_resolution_scale_pct = 100;  // APE may not scale below this
  int max_background_tasks = 4;
};

Caps caps_for(PowerMode mode);

// Default mode from host state: battery -> saver, plugged -> balanced.
// Unknown battery state -> balanced (documented, not assumed to be AC).
PowerMode default_mode(const ape::HardwareFacts& f);

// Apply caps as bounds to quality settings.
void apply_caps(const Caps& c, ape::QualitySettings& s);

}  // namespace pnr::laptop
