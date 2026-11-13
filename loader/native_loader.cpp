#include "loader/native_loader.hpp"

#include <chrono>

#if defined(__unix__) || defined(__APPLE__)
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <unistd.h>
  #define PNR_POSIX 1
#endif

namespace pnr::loader {

ParseResult NativeLoader::validate(const std::string& path) {
  ParseResult r = parse_elf(path);
  if (!r.ok) return r;
  if (!is_x86_64(r.info)) {
    r.ok = false;
    r.error = "ELF machine " + std::to_string(r.info.machine) +
              " is not x86-64 (62); only x86-64 targets are in scope";
    return r;
  }
  if (!is_executable_type(r.info)) {
    r.ok = false;
    r.error = "ELF type " + std::to_string(r.info.elf_type) +
              " is not an executable (ET_EXEC/ET_DYN)";
    return r;
  }
  return r;
}

LaunchResult NativeLoader::launch(const std::string& path,
                                  const std::vector<std::string>& args) {
  LaunchResult out;

  ParseResult v = validate(path);
  if (!v.ok) {
    out.error = "validation failed: " + v.error;
    return out;
  }
  out.validated = true;
  out.elf = v.info;

#ifndef PNR_POSIX
  out.error = "native process launch is implemented for POSIX hosts only "
              "(Windows backend: Planned)";
  return out;
#else
  const auto t0 = std::chrono::steady_clock::now();

  pid_t pid = ::fork();
  if (pid < 0) {
    out.error = "fork() failed with errno " + std::to_string(errno);
    return out;
  }
  if (pid == 0) {
    // Child: build argv and exec. On failure, exit 127 (shell convention).
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
  do {
    got = ::waitpid(pid, &status, 0);
  } while (got < 0 && errno == EINTR);
  if (got < 0) {
    out.error = "waitpid() failed with errno " + std::to_string(errno);
    return out;
  }

  const auto t1 = std::chrono::steady_clock::now();
  out.wall_seconds = std::chrono::duration<double>(t1 - t0).count();
  out.finished = true;
  if (WIFEXITED(status)) {
    out.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    out.signaled = true;
    out.signal = WTERMSIG(status);
  }
  return out;
#endif
}

}  // namespace pnr::loader
