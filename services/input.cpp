#include "services/input.hpp"

#include <filesystem>
#include <fstream>

namespace pnr::services {

std::vector<InputDeviceInfo> EvdevInputBackend::enumerate() {
  std::vector<InputDeviceInfo> out;
  std::error_code ec;
  const std::filesystem::path dir("/dev/input");
  if (!std::filesystem::exists(dir, ec)) return out;  // no evdev on this host
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    const auto name = e.path().filename().string();
    if (name.rfind("event", 0) != 0) continue;
    InputDeviceInfo info;
    info.path = e.path().string();
    info.name = name;  // device names need ioctl access (permissions); path reported honestly
    info.kind = InputKind::unknown;
    out.push_back(std::move(info));
  }
  return out;
}

std::vector<InputDeviceInfo> VirtualInputDevice::enumerate() {
  return {InputDeviceInfo{"", "pnr-virtual-input", InputKind::keyboard}};
}

std::optional<InputEvent> VirtualInputDevice::poll() {
  if (queue_.empty()) return std::nullopt;
  auto e = queue_.front();
  queue_.erase(queue_.begin());
  return e;
}

Status InputMapper::load(const std::string& profile_text) {
  std::map<std::string, std::string> m;
  size_t pos = 0;
  int line_no = 0;
  while (pos <= profile_text.size()) {
    const size_t nl = profile_text.find('\n', pos);
    const std::string line = profile_text.substr(
        pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = (nl == std::string::npos) ? profile_text.size() + 1 : nl + 1;
    ++line_no;
    if (line.empty() || line[0] == '#') continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos)
      return Status::Fail(1, "input", "profile line " + std::to_string(line_no) +
                                          " has no '=': " + line);
    auto trim = [](std::string s) {
      const auto b = s.find_first_not_of(" \t\r");
      const auto e = s.find_last_not_of(" \t\r");
      return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
    };
    const std::string action = trim(line.substr(0, eq));
    const std::string control = trim(line.substr(eq + 1));
    if (action.empty() || control.empty())
      return Status::Fail(1, "input", "profile line " + std::to_string(line_no) +
                                          " has an empty side");
    m[action] = control;
  }
  map_ = std::move(m);
  return Status::Ok();
}

std::optional<std::string> InputMapper::control_for(const std::string& action) const {
  const auto it = map_.find(action);
  return it == map_.end() ? std::nullopt : std::optional<std::string>(it->second);
}

std::optional<InputEvent> InputMapper::translate(const InputEvent& raw) const {
  for (const auto& [action, control] : map_)
    if (control == raw.control) return InputEvent{action, raw.value};
  return std::nullopt;
}

std::shared_ptr<IInputBackend> make_input_backend() {
#if defined(__unix__) || defined(__APPLE__)
  return std::make_shared<EvdevInputBackend>();
#else
  return std::make_shared<VirtualInputDevice>();  // Windows raw input: Planned
#endif
}

}  // namespace pnr::services
