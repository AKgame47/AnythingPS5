// Slice S4 tests: pad axis normalization + pad-style events through the
// virtual input device and mapper. Host-side model only, no SDL here (the
// live SDL pad path is exercised by pnr_pad_smoke in the window build).
#include <cmath>
#include <cstdio>
#include <string>

#include "services/input.hpp"

void pnr_pad_tests(int& pass, int& fail) {
  using namespace pnr::services;
  auto check = [&](bool ok, const std::string& l) {
    if (ok) ++pass; else ++fail;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", l.c_str());
  };
  auto near = [&](float a, float b) { return std::fabs(a - b) < 1e-6f; };

  // normalize_axis: neutral, full scale, clamping.
  check(near(normalize_axis(0, 32767, 0.0f), 0.0f), "pad axis zero");
  check(near(normalize_axis(32767, 32767, 0.0f), 1.0f), "pad axis full right");
  check(near(normalize_axis(-32767, 32767, 0.0f), -1.0f), "pad axis full left");
  check(near(normalize_axis(40000, 32767, 0.0f), 1.0f), "pad axis clamp high");
  check(near(normalize_axis(-40000, 32767, 0.0f), -1.0f), "pad axis clamp low");
  check(near(normalize_axis(16384, 32767, 0.0f), 16384.0f / 32767.0f), "pad axis half scale");

  // normalize_axis: deadzone edges and rescaling outside it.
  check(near(normalize_axis(1000, 32767, 0.25f), 0.0f), "pad axis inside deadzone");
  check(near(normalize_axis(-1000, 32767, 0.25f), 0.0f), "pad axis inside deadzone negative");
  check(near(normalize_axis(8192, 32768, 0.25f), 0.0f), "pad axis on deadzone edge");
  {
    // Just past the deadzone the rescaled value is small but non-zero.
    const float v = normalize_axis(9000, 32768, 0.25f);
    check(v > 0.0f && v < 0.05f, "pad axis just past deadzone");
    // Half of the remaining travel after the deadzone is 0.5.
    const float half = normalize_axis(20480, 32768, 0.25f);  // raw 0.625, (0.625-0.25)/0.75 = 0.5
    check(near(half, 0.5f), "pad axis rescaled mid travel");
    check(near(normalize_axis(-20480, 32768, 0.25f), -0.5f), "pad axis rescaled mid travel negative");
  }
  check(near(normalize_axis(32767, 32767, 0.25f), 1.0f), "pad axis rim with deadzone");

  // normalize_axis: degenerate inputs stay honest.
  check(near(normalize_axis(5, 0, 0.1f), 0.0f), "pad axis zero max");
  check(near(normalize_axis(5, -10, 0.1f), 0.0f), "pad axis negative max");
  check(near(normalize_axis(16384, 32767, -0.5f), 16384.0f / 32767.0f), "pad axis negative deadzone treated as 0");
  check(near(normalize_axis(100, 32767, 1.5f), 0.0f), "pad axis deadzone >= 1 kills small input");

  // Pad-style events through the scripted device: poll order and values.
  VirtualInputDevice dev;
  dev.push(InputEvent{"ABS_X", 16384});
  dev.push(InputEvent{"BTN_SOUTH", 1});
  dev.push(InputEvent{"BTN_SOUTH", 0});
  auto e1 = dev.poll();
  auto e2 = dev.poll();
  auto e3 = dev.poll();
  auto e4 = dev.poll();
  check(e1.has_value() && e1->control == "ABS_X" && e1->value == 16384, "pad virtual axis event");
  check(e2.has_value() && e2->control == "BTN_SOUTH" && e2->value == 1, "pad virtual button down");
  check(e3.has_value() && e3->control == "BTN_SOUTH" && e3->value == 0, "pad virtual button up");
  check(!e4.has_value(), "pad virtual queue drained");

  // Mapper translates pad controls into game actions like any other control.
  InputMapper map;
  auto st = map.load("jump = BTN_SOUTH\nsteer = ABS_X\n");
  check(bool(st), "pad mapping profile loads");
  auto j = map.translate(InputEvent{"BTN_SOUTH", 1});
  check(j.has_value() && j->control == "jump" && j->value == 1, "pad button maps to action");
  auto s = map.translate(InputEvent{"ABS_X", -20000});
  check(s.has_value() && s->control == "steer" && s->value == -20000, "pad axis maps to action with value");
  auto u = map.translate(InputEvent{"BTN_NORTH", 1});
  check(!u.has_value(), "unmapped pad control passes nothing");
}
