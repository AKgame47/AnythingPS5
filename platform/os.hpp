// PNR Host OS Abstraction - MODULE 07, interfaces IF-OS-01.. (v1)
// One common API; Linux backend implemented, Windows backend Planned.
// The core runtime never includes OS headers directly.
// Status: Implemented (Linux), Tested.
#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pnr::platform {

struct ProcessResult {
  bool started = false;
  bool finished = false;
  int exit_code = -1;
  bool signaled = false;
  int signal = 0;
  double wall_seconds = 0.0;
  std::string error;
};

struct MemoryInfo {
  uint64_t total_bytes = 0;
  uint64_t available_bytes = 0;
};

class IHostPlatform {
public:
  virtual ~IHostPlatform() = default;
  virtual std::string name() const = 0;   // "linux" | "windows"
  virtual unsigned cpu_logical_cores() const = 0;
  virtual std::optional<MemoryInfo> memory() const = 0;
  virtual double monotonic_seconds() const = 0;
  // Runs a validated binary as a supervised child process.
  virtual ProcessResult launch_process(
      const std::string& path, const std::vector<std::string>& args) = 0;
  // Power/thermal telemetry: unsupported readings return nullopt with a
  // note in power_note(); never fabricated numbers.
  virtual std::optional<bool> on_battery() const = 0;
};

// Returns the backend for the compiling host. On an OS without a backend
// the factory returns a stub whose operations report unsupported.
std::shared_ptr<IHostPlatform> make_host_platform();

}  // namespace pnr::platform
