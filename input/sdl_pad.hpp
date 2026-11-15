// PNR live pad input over SDL2, GPL-2.0. Slice S4.
// A LIVE joystick source for the services/input model: the existing backends
// only enumerate devices or replay scripted events; this one opens the first
// SDL joystick and turns real pad state into PNR InputEvents with the same
// control names the evdev/C5 pad model already uses (BTN_SOUTH..., ABS_X...).
// The axis/button-index to control-name mapping is PNR's own convention
// (documented in sdl_pad.cpp); SDL's controller database is NOT used, so on
// an unusual physical pad the physical layout can differ from the names.
// Verified against SDL's virtual joystick (SDL_JoystickAttachVirtual) under
// Xvfb; a physical USB/Bluetooth pad is Untested (no hardware in the sandbox).
// Built only with -DPNR_BUILD_WINDOW=ON (needs SDL2), same policy as the
// window presenter and pnr-ui.
// Status: Implemented, Tested (virtual pad); hardware pad Untested.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "core/runtime.hpp"
#include "services/input.hpp"

namespace pnr::input {

class SdlPadSource {
public:
  SdlPadSource(const SdlPadSource&) = delete;
  SdlPadSource& operator=(const SdlPadSource&) = delete;
  ~SdlPadSource();

  // Opens the first SDL joystick. Initializes the SDL joystick subsystem if
  // needed. Honest failure when no joystick is present (that is BLOCKED for
  // a caller that requires a pad, not a crash).
  static pnr::Status open_first(std::unique_ptr<SdlPadSource>* out);

  // Reads the current pad state (SDL must have pumped events this frame -
  // SDL_PollEvent/SDL_PumpEvents does that) and appends one InputEvent per
  // control whose value changed since the previous poll. Analog axes report
  // raw SDL int16 magnitudes (-32768..32767) as int32, matching the evdev
  // style values the C5 pad model consumes. Returns the number of events.
  size_t poll(std::vector<pnr::services::InputEvent>* out);

  const std::string& device_name() const { return name_; }
  int axes() const { return n_axes_; }
  int buttons() const { return n_buttons_; }
  int hats() const { return n_hats_; }

private:
  SdlPadSource() = default;
  void* joy_ = nullptr;  // SDL_Joystick*, opaque to this header
  std::string name_;
  int n_axes_ = 0, n_buttons_ = 0, n_hats_ = 0;
  std::vector<int16_t> prev_axes_;
  std::vector<uint8_t> prev_buttons_;
  std::vector<uint8_t> prev_hats_;
  bool first_poll_ = true;
};

// Test/demo helper: an SDL VIRTUAL joystick (SDL 2.0.14+). Hosts with no
// physical pad - CI containers, Xvfb sandboxes - still exercise the real SDL
// joystick path end to end: the virtual device is attached through SDL, opened
// by SdlPadSource like any joystick, and driven through SDL's own setters.
class SdlVirtualPad {
public:
  SdlVirtualPad(const SdlVirtualPad&) = delete;
  SdlVirtualPad& operator=(const SdlVirtualPad&) = delete;
  ~SdlVirtualPad();

  // Attaches a virtual joystick with the given layout. Honest failure when
  // the SDL build lacks virtual joystick support.
  static pnr::Status attach(int axes, int buttons, int hats,
                            std::unique_ptr<SdlVirtualPad>* out);

  pnr::Status set_axis(int axis, int16_t value);
  pnr::Status set_button(int button, bool down);
  pnr::Status set_hat(int hat, uint8_t value);

private:
  SdlVirtualPad() = default;
  int device_index_ = -1;   // SDL joystick device index, for detach
  void* joy_ = nullptr;     // SDL_Joystick* used for the virtual setters
};

}  // namespace pnr::input
