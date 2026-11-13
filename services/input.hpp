// PNR Game Services - MODULE 08: Input (v1)
// Backend interface + Linux evdev enumeration + a mapping-profile parser
// + a scripted device for tests. Advanced controller features are exposed
// only when the device reports them (per the master prompt).
// Status: Implemented (enumeration + mapping + virtual device), Tested.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "core/runtime.hpp"

namespace pnr::services {

enum class InputKind { keyboard, mouse, controller, unknown };

struct InputDeviceInfo {
  std::string path;      // host path (e.g. /dev/input/event3); empty for virtual
  std::string name;
  InputKind kind = InputKind::unknown;
};

struct InputEvent {
  std::string control;   // e.g. "KEY_A", "BTN_SOUTH", "ABS_X"
  int32_t value = 0;     // 0/1 buttons, raw analog otherwise
};

class IInputBackend {
public:
  virtual ~IInputBackend() = default;
  virtual std::string name() const = 0;
  virtual std::vector<InputDeviceInfo> enumerate() = 0;
};

// Linux evdev enumeration (read-only scan of /dev/input). Opening devices
// for reading needs host permissions; this backend only enumerates.
class EvdevInputBackend final : public IInputBackend {
public:
  std::string name() const override { return "evdev"; }
  std::vector<InputDeviceInfo> enumerate() override;
};

// Scripted device used by tests and the demo: events are pushed in code.
class VirtualInputDevice final : public IInputBackend {
public:
  std::string name() const override { return "virtual"; }
  std::vector<InputDeviceInfo> enumerate() override;
  void push(InputEvent e) { queue_.push_back(std::move(e)); }
  std::optional<InputEvent> poll();
private:
  std::vector<InputEvent> queue_;
};

// Maps raw control names to game actions from an ini-style profile:
//   # comment
//   jump = KEY_SPACE
//   fire = BTN_LEFT
// Unknown/empty lines are skipped; parse errors are reported per line.
class InputMapper {
public:
  Status load(const std::string& profile_text);
  // Returns the bound control, or nullopt when unmapped.
  std::optional<std::string> control_for(const std::string& action) const;
  // Translates a raw event into an action event when mapped.
  std::optional<InputEvent> translate(const InputEvent& raw) const;
  size_t binding_count() const { return map_.size(); }
private:
  std::map<std::string, std::string> map_;  // action -> control
};

std::shared_ptr<IInputBackend> make_input_backend();

}  // namespace pnr::services
