// PNR Loader - Native Loader (part B of MODULE 02)
// Executes a validated native x86-64 test program in a SEPARATE process
// (crash isolation, security rule: never run imported code in the
// launcher's process). In-process segment mapping (mmap + relocations)
// is Phase 1 follow-up work and is documented as Planned, not faked.
// Status: Implemented (Linux/POSIX). Windows backend: Planned.
#pragma once

#include <string>
#include <vector>

#include "loader/elf_parser.hpp"

namespace pnr::loader {

struct LaunchResult {
  bool validated = false;   // executable format validated
  bool started = false;     // child process actually spawned
  bool finished = false;    // waitpid returned
  int exit_code = -1;       // valid when finished && !signaled
  bool signaled = false;
  int signal = 0;
  double wall_seconds = 0.0;
  std::string error;        // set on any failure stage
  ElfInfo elf;              // validated metadata
};

class NativeLoader {
public:
  // Validates the binary, then fork()+execve() it as a child process,
  // waits for completion and measures wall time. Child inherits
  // stdout/stderr so game output stays visible (captured later).
  LaunchResult launch(const std::string& path,
                      const std::vector<std::string>& args);

  // Pre-flight validation only (STEP 1 of the launch workflow).
  ParseResult validate(const std::string& path);
};

}  // namespace pnr::loader
