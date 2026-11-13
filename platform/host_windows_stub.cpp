// Windows backend for the host abstraction (IF-OS-01).
// Status: Implemented (hwinfo via Win32 API), process launch Blocked:
// PNR loads ELF binaries, a Linux format; Windows cannot exec them
// natively. Honest error instead of a fake launch.
#include "platform/os.hpp"

#include <chrono>
#include <thread>

#ifndef _WIN32
#error "host_windows_stub.cpp is only for Windows builds"
#endif
#include <malloc.h>
#include <windows.h>

namespace pnr::platform {
namespace {

class WindowsPlatform final : public IHostPlatform {
public:
  std::string name() const override { return "windows"; }

  unsigned cpu_logical_cores() const override {
    unsigned n = std::thread::hardware_concurrency();
    return n ? n : 1;
  }

  std::optional<MemoryInfo> memory() const override {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (!::GlobalMemoryStatusEx(&ms)) return std::nullopt;
    MemoryInfo info;
    info.total_bytes = ms.ullTotalPhys;
    info.available_bytes = ms.ullAvailPhys;
    return info;
  }

  double monotonic_seconds() const override {
    const auto t = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<double>(t).count();
  }

  ProcessResult launch_process(const std::string& path,
                               const std::vector<std::string>&) override {
    ProcessResult out;
    out.error = "native launch unsupported on Windows: '" + path +
                "' is an ELF (Linux) binary; Windows process launch is "
                "Blocked pending a PE-format toolchain path";
    return out;
  }

  std::optional<bool> on_battery() const override {
    SYSTEM_POWER_STATUS sps{};
    if (!::GetSystemPowerStatus(&sps)) return std::nullopt;
    if (sps.ACLineStatus == 255) return std::nullopt;  // unknown
    return sps.ACLineStatus == 0;
  }
};

}  // namespace

std::shared_ptr<IHostPlatform> make_host_platform() {
  return std::make_shared<WindowsPlatform>();
}

}  // namespace pnr::platform
