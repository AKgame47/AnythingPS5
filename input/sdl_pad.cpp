// PNR live pad input over SDL2, GPL-2.0. Slice S4. See sdl_pad.hpp.
//
// Control-name convention (PNR's own; matches the evdev/C5 pad model so the
// same events can feed pnr::prx::pad_apply_host_event):
//   axes:   0 ABS_X   1 ABS_Y   2 ABS_RX   3 ABS_RY   4 ABS_Z   5 ABS_RZ
//           (beyond 5: ABS_<n>)
//   buttons:0 BTN_SOUTH 1 BTN_EAST 2 BTN_WEST 3 BTN_NORTH 4 BTN_TL 5 BTN_TR
//           6 BTN_SELECT 7 BTN_START 8 BTN_THUMBL 9 BTN_THUMBR
//           (beyond 9: BTN_<n>)
//   hats:   hat 0 -> ABS_HAT0X / ABS_HAT0Y with -1/0/+1
// This is a positional convention, not a physical-layout promise: SDL's
// controller database is not consulted, so an unusual pad can report its
// buttons in a different order. Untested on physical hardware.
#include "input/sdl_pad.hpp"

#include <SDL.h>

namespace pnr::input {
namespace {

pnr::Status fail(const std::string& msg) {
  return pnr::Status::Fail(1, "input", msg);
}

const char* axis_name(int i) {
  switch (i) {
    case 0: return "ABS_X";
    case 1: return "ABS_Y";
    case 2: return "ABS_RX";
    case 3: return "ABS_RY";
    case 4: return "ABS_Z";
    case 5: return "ABS_RZ";
    default: break;
  }
  return nullptr;  // caller formats ABS_<n>
}

const char* button_name(int i) {
  switch (i) {
    case 0: return "BTN_SOUTH";
    case 1: return "BTN_EAST";
    case 2: return "BTN_WEST";
    case 3: return "BTN_NORTH";
    case 4: return "BTN_TL";
    case 5: return "BTN_TR";
    case 6: return "BTN_SELECT";
    case 7: return "BTN_START";
    case 8: return "BTN_THUMBL";
    case 9: return "BTN_THUMBR";
    default: break;
  }
  return nullptr;  // caller formats BTN_<n>
}

}  // namespace

pnr::Status SdlPadSource::open_first(std::unique_ptr<SdlPadSource>* out) {
  if (!out) return fail("SdlPadSource::open_first: null out");
  out->reset();
  if (SDL_WasInit(SDL_INIT_JOYSTICK) == 0 && SDL_InitSubSystem(SDL_INIT_JOYSTICK) != 0)
    return fail(std::string("SDL joystick init failed: ") + SDL_GetError());
  if (SDL_NumJoysticks() < 1)
    return fail("no SDL joystick connected (a pad is optional; the demo runs without one)");
  SDL_Joystick* joy = SDL_JoystickOpen(0);
  if (!joy) return fail(std::string("SDL_JoystickOpen failed: ") + SDL_GetError());
  auto pad = std::unique_ptr<SdlPadSource>(new SdlPadSource());
  pad->joy_ = joy;
  const char* n = SDL_JoystickName(joy);
  pad->name_ = n ? n : "unnamed joystick";
  pad->n_axes_ = SDL_JoystickNumAxes(joy);
  pad->n_buttons_ = SDL_JoystickNumButtons(joy);
  pad->n_hats_ = SDL_JoystickNumHats(joy);
  if (pad->n_axes_ < 0 || pad->n_buttons_ < 0 || pad->n_hats_ < 0)
    return fail(std::string("SDL joystick query failed: ") + SDL_GetError());
  *out = std::move(pad);
  return pnr::Status::Ok();
}

size_t SdlPadSource::poll(std::vector<pnr::services::InputEvent>* out) {
  size_t emitted = 0;
  if (!joy_ || !out) return emitted;
  if (SDL_JoystickGetAttached(reinterpret_cast<SDL_Joystick*>(joy_)) == SDL_FALSE) {
    joy_ = nullptr;  // honest detach: further polls report nothing
    return emitted;
  }
  SDL_Joystick* joy = reinterpret_cast<SDL_Joystick*>(joy_);
  if (first_poll_) {
    prev_axes_.assign(size_t(n_axes_), 0);
    prev_buttons_.assign(size_t(n_buttons_), 0);
    prev_hats_.assign(size_t(n_hats_), 0);
    // Seed from the real current state so a held control is not invented as
    // a fresh event; the first poll reports only what differs from neutral.
    for (int i = 0; i < n_axes_; ++i) prev_axes_[size_t(i)] = SDL_JoystickGetAxis(joy, i);
    for (int i = 0; i < n_buttons_; ++i) prev_buttons_[size_t(i)] = uint8_t(SDL_JoystickGetButton(joy, i));
    for (int i = 0; i < n_hats_; ++i) prev_hats_[size_t(i)] = uint8_t(SDL_JoystickGetHat(joy, i));
    first_poll_ = false;
    return emitted;
  }
  auto push = [&](std::string control, int32_t value) {
    out->push_back(pnr::services::InputEvent{std::move(control), value});
    ++emitted;
  };
  for (int i = 0; i < n_axes_; ++i) {
    const int16_t v = SDL_JoystickGetAxis(joy, i);
    if (v == prev_axes_[size_t(i)]) continue;
    prev_axes_[size_t(i)] = v;
    if (const char* n = axis_name(i)) push(n, int32_t(v));
    else push("ABS_" + std::to_string(i), int32_t(v));
  }
  for (int i = 0; i < n_buttons_; ++i) {
    const uint8_t v = uint8_t(SDL_JoystickGetButton(joy, i));
    if (v == prev_buttons_[size_t(i)]) continue;
    prev_buttons_[size_t(i)] = v;
    if (const char* n = button_name(i)) push(n, int32_t(v));
    else push("BTN_" + std::to_string(i), int32_t(v));
  }
  for (int i = 0; i < n_hats_; ++i) {
    const uint8_t v = uint8_t(SDL_JoystickGetHat(joy, i));
    if (v == prev_hats_[size_t(i)]) continue;
    prev_hats_[size_t(i)] = v;
    if (i == 0) {
      const int x = (v & SDL_HAT_RIGHT) ? 1 : (v & SDL_HAT_LEFT) ? -1 : 0;
      const int y = (v & SDL_HAT_UP) ? -1 : (v & SDL_HAT_DOWN) ? 1 : 0;  // evdev style: up is negative
      push("ABS_HAT0X", x);
      push("ABS_HAT0Y", y);
    } else {
      push("ABS_HAT" + std::to_string(i) + "X", 0);
      push("ABS_HAT" + std::to_string(i) + "Y", 0);
    }
  }
  return emitted;
}

SdlPadSource::~SdlPadSource() {
  if (joy_) SDL_JoystickClose(reinterpret_cast<SDL_Joystick*>(joy_));
}

pnr::Status SdlVirtualPad::attach(int axes, int buttons, int hats,
                                  std::unique_ptr<SdlVirtualPad>* out) {
  if (!out) return fail("SdlVirtualPad::attach: null out");
  out->reset();
  if (axes < 0 || buttons < 0 || hats < 0)
    return fail("SdlVirtualPad::attach: negative layout");
  if (SDL_WasInit(SDL_INIT_JOYSTICK) == 0 && SDL_InitSubSystem(SDL_INIT_JOYSTICK) != 0)
    return fail(std::string("SDL joystick init failed: ") + SDL_GetError());
  const int idx = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                                            axes, buttons, hats);
  if (idx < 0)
    return fail(std::string("SDL_JoystickAttachVirtual failed: ") + SDL_GetError());
  SDL_Joystick* joy = SDL_JoystickOpen(idx);
  if (!joy) {
    SDL_JoystickDetachVirtual(idx);
    return fail(std::string("SDL_JoystickOpen(virtual) failed: ") + SDL_GetError());
  }
  auto pad = std::unique_ptr<SdlVirtualPad>(new SdlVirtualPad());
  pad->device_index_ = idx;
  pad->joy_ = joy;
  *out = std::move(pad);
  return pnr::Status::Ok();
}

pnr::Status SdlVirtualPad::set_axis(int axis, int16_t value) {
  if (!joy_) return fail("SdlVirtualPad: not attached");
  if (SDL_JoystickSetVirtualAxis(reinterpret_cast<SDL_Joystick*>(joy_), axis, value) != 0)
    return fail(std::string("SDL_JoystickSetVirtualAxis failed: ") + SDL_GetError());
  return pnr::Status::Ok();
}

pnr::Status SdlVirtualPad::set_button(int button, bool down) {
  if (!joy_) return fail("SdlVirtualPad: not attached");
  if (SDL_JoystickSetVirtualButton(reinterpret_cast<SDL_Joystick*>(joy_), button,
                                   down ? Uint8(1) : Uint8(0)) != 0)
    return fail(std::string("SDL_JoystickSetVirtualButton failed: ") + SDL_GetError());
  return pnr::Status::Ok();
}

pnr::Status SdlVirtualPad::set_hat(int hat, uint8_t value) {
  if (!joy_) return fail("SdlVirtualPad: not attached");
  if (SDL_JoystickSetVirtualHat(reinterpret_cast<SDL_Joystick*>(joy_), hat, value) != 0)
    return fail(std::string("SDL_JoystickSetVirtualHat failed: ") + SDL_GetError());
  return pnr::Status::Ok();
}

SdlVirtualPad::~SdlVirtualPad() {
  if (joy_) SDL_JoystickClose(reinterpret_cast<SDL_Joystick*>(joy_));
  if (device_index_ >= 0) SDL_JoystickDetachVirtual(device_index_);
}

}  // namespace pnr::input
