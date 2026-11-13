// PNR Compatibility Layer - MODULE 03 (first slice)
// Console-style application services mapped to host equivalents.
// These are REAL host-backed services used by the demo game; they are
// not mocks and they are not PS5 API names (no proprietary API is
// invented - rule 10). A future authorized console frontend would bind
// its own names onto these documented behaviors.
// Status: Implemented (unit-tested on Linux).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "core/runtime.hpp"

namespace pnr::compat {

// Memory Services: aligned allocation + usage tracking.
class MemoryService {
public:
  void* allocate(std::size_t size, std::size_t alignment = 16);
  void deallocate(void* p);
  std::size_t bytes_outstanding() const;
  std::size_t live_allocations() const;

private:
  mutable std::mutex mutex_;
  std::map<void*, std::size_t> live_;
};

// Timing Services: monotonic clock + sleep.
class TimingService {
public:
  // Seconds on a monotonic clock (steady across wall-clock changes).
  static double now_seconds();
  static void sleep_ms(double ms);
};

// Filesystem Services: virtual paths jailed under a root directory.
// NOTE: application-level path routing, NOT an OS-enforced security
// sandbox - code that calls the host directly bypasses it. Trusted
// developer-built content only until Module 11 lands a real boundary.
// Virtual path "/saves/a.txt" maps to "<root>/saves/a.txt".
// Path traversal escaping the root is rejected explicitly.
class FileSystemService {
public:
  explicit FileSystemService(std::string root);

  // Resolves a virtual path to a host path. Fails on traversal attempts,
  // absolute host paths, and empty paths.
  Status resolve(const std::string& virtual_path, std::string& host_out) const;

  Status write_file(const std::string& virtual_path,
                    const std::string& data) const;
  Status read_file(const std::string& virtual_path, std::string& out) const;
  bool exists(const std::string& virtual_path) const;
  Status create_dirs(const std::string& virtual_path) const;
  const std::string& root() const { return root_; }

private:
  std::string root_;
};

// Thread Services: spawn + join with ids.
class ThreadService {
public:
  uint64_t spawn(std::function<void()> fn);
  void join_all();
  size_t live_count() const;

private:
  mutable std::mutex mutex_;
  uint64_t next_id_ = 1;
  std::map<uint64_t, std::thread> threads_;
};

}  // namespace pnr::compat
