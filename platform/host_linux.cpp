// Linux/POSIX backend for the host abstraction.
#include "platform/os.hpp"

#include <chrono>
#include <cstring>
#include <fstream>
#include <thread>

#if defined(__unix__) || defined(__APPLE__)
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <unistd.h>
  #define PNR_POSIX 1
#endif

namespace pnr::platform {
namespace {

class LinuxPlatform final : public IHostPlatform {
public:
  std::string name() const override { return "linux"; }

  unsigned cpu_logical_cores() const override {
    unsigned n = std::thread::hardware_concurrency();
    return n ? n : 1;
  }

  std::optional<MemoryInfo> memory() const override {
    std::ifstream mi("/proc/meminfo");
    if (!mi) return std::nullopt;  // non-/proc host: reported missing
    MemoryInfo info;
    std::string key, unit;
    uint64_t value = 0;
    bool got_total = false;
    while (mi >> key >> value >> unit) {
      if (key == "MemTotal:") { info.total_bytes = value * 1024; got_total = true; }
      else if (key == "MemAvailable:") info.available_bytes = value * 1024;
    }
    if (!got_total) return std::nullopt;
    return info;
  }

  double monotonic_seconds() const override {
    const auto t = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<double>(t).count();
  }

  ProcessResult launch_process(const std::string& path,
                               const std::vector<std::string>& args) override {
    ProcessResult out;
#ifndef PNR_POSIX
    out.error = "process launch unimplemented on this host";
    return out;
#else
    const auto t0 = std::chrono::steady_clock::now();
    pid_t pid = ::fork();
    if (pid < 0) {
      out.error = "fork() failed, errno " + std::to_string(errno);
      return out;
    }
    if (pid == 0) {
      std::vector<char*> argv;
      argv.push_back(const_cast<char*>(path.c_str()));
      for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
      argv.push_back(nullptr);
      ::execv(path.c_str(), argv.data());
      _exit(127);
    }
    out.started = true;
    int status = 0;
    pid_t got;
    do { got = ::waitpid(pid, &status, 0); } while (got < 0 && errno == EINTR);
    if (got < 0) {
      out.error = "waitpid() failed, errno " + std::to_string(errno);
      return out;
    }
    const auto t1 = std::chrono::steady_clock::now();
    out.wall_seconds = std::chrono::duration<double>(t1 - t0).count();
    out.finished = true;
    if (WIFEXITED(status)) out.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) { out.signaled = true; out.signal = WTERMSIG(status); }
    return out;
#endif
  }

  std::optional<bool> on_battery() const override {
    std::ifstream ac("/sys/class/power_supply/AC/online");
    if (!ac) {
      std::ifstream adp("/sys/class/power_supply/ADP1/online");
      if (!adp) return std::nullopt;  // desktop or unknown: reported missing
      int v; if (!(adp >> v)) return std::nullopt;
      return v == 0;
    }
    int v; if (!(ac >> v)) return std::nullopt;
    return v == 0;
  }
};

}  // namespace

std::shared_ptr<IHostPlatform> make_host_platform() {
  return std::make_shared<LinuxPlatform>();
}

}  // namespace pnr::platform
