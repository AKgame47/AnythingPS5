#include "compat/services.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
  #include <stdlib.h>  // posix_memalign
#elif defined(_WIN32)
  #include <malloc.h>  // _aligned_malloc/_aligned_free
#endif

namespace pnr::compat {

// --- MemoryService ---------------------------------------------------------
void* MemoryService::allocate(std::size_t size, std::size_t alignment) {
  if (size == 0) return nullptr;
  if (alignment < sizeof(void*)) alignment = sizeof(void*);
  // alignment must be a power of two for posix_memalign
  if ((alignment & (alignment - 1)) != 0) return nullptr;
  void* p = nullptr;
#if defined(__unix__) || defined(__APPLE__)
  if (::posix_memalign(&p, alignment, size) != 0) return nullptr;
#else
  p = ::_aligned_malloc(size, alignment);  // Windows path (untested here)
#endif
  if (!p) return nullptr;
  std::lock_guard<std::mutex> lock(mutex_);
  live_[p] = size;
  return p;
}

void MemoryService::deallocate(void* p) {
  if (!p) return;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    live_.erase(p);
  }
#if defined(__unix__) || defined(__APPLE__)
  ::free(p);
#else
  ::_aligned_free(p);
#endif
}

std::size_t MemoryService::bytes_outstanding() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::size_t total = 0;
  for (const auto& [ptr, sz] : live_) total += sz;
  return total;
}

std::size_t MemoryService::live_allocations() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return live_.size();
}

// --- TimingService ---------------------------------------------------------
double TimingService::now_seconds() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration<double>(now).count();
}

void TimingService::sleep_ms(double ms) {
  if (ms <= 0) return;
  std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(ms));
}

// --- FileSystemService -----------------------------------------------------
FileSystemService::FileSystemService(std::string root) : root_(std::move(root)) {
  if (root_.empty()) root_ = ".";
  while (root_.size() > 1 && root_.back() == '/') root_.pop_back();
}

Status FileSystemService::resolve(const std::string& virtual_path,
                                  std::string& host_out) const {
  if (virtual_path.empty())
    return Status::Fail(30, "compat.fs", "empty virtual path");
  if (virtual_path[0] != '/')
    return Status::Fail(31, "compat.fs",
                        "virtual path must start with '/': " + virtual_path);

  // Split and normalize; reject anything that would escape the jail.
  std::vector<std::string> parts;
  std::string cur;
  for (char c : virtual_path.substr(1)) {
    if (c == '/') {
      if (cur == "..") {
        if (parts.empty())
          return Status::Fail(32, "compat.fs",
                              "path traversal rejected: " + virtual_path);
        parts.pop_back();
      } else if (!cur.empty() && cur != ".") {
        parts.push_back(cur);
      }
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (cur == "..") {
    if (parts.empty())
      return Status::Fail(32, "compat.fs",
                          "path traversal rejected: " + virtual_path);
    parts.pop_back();
  } else if (!cur.empty() && cur != ".") {
    parts.push_back(cur);
  }
  for (const auto& p : parts) {
    if (p.find('\\') != std::string::npos || p.find('\0') != std::string::npos)
      return Status::Fail(33, "compat.fs",
                          "illegal character in path: " + virtual_path);
  }

  host_out = root_;
  for (const auto& p : parts) host_out += "/" + p;
  return Status::Ok();
}

Status FileSystemService::write_file(const std::string& virtual_path,
                                     const std::string& data) const {
  std::string host;
  Status st = resolve(virtual_path, host);
  if (!st) return st;
  // Ensure parent directories exist.
  auto slash = host.find_last_of('/');
  if (slash != std::string::npos && slash > 0) {
    std::string dir = host.substr(0, slash);
    std::string cmd_check;  // no shell; use create_directories below
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec)
      return Status::Fail(34, "compat.fs",
                          "cannot create directory " + dir + ": " +
                              ec.message());
  }
  std::ofstream out(host, std::ios::binary | std::ios::trunc);
  if (!out) return Status::Fail(35, "compat.fs", "cannot open for write: " + host);
  out << data;
  if (!out) return Status::Fail(36, "compat.fs", "write failed: " + host);
  return Status::Ok();
}

Status FileSystemService::read_file(const std::string& virtual_path,
                                    std::string& out) const {
  std::string host;
  Status st = resolve(virtual_path, host);
  if (!st) return st;
  std::ifstream in(host, std::ios::binary);
  if (!in) return Status::Fail(37, "compat.fs", "cannot open for read: " + host);
  out.assign(std::istreambuf_iterator<char>(in),
             std::istreambuf_iterator<char>());
  return Status::Ok();
}

bool FileSystemService::exists(const std::string& virtual_path) const {
  std::string host;
  if (!resolve(virtual_path, host)) return false;
  std::error_code ec;
  return std::filesystem::exists(host, ec);
}

Status FileSystemService::create_dirs(const std::string& virtual_path) const {
  std::string host;
  Status st = resolve(virtual_path, host);
  if (!st) return st;
  std::error_code ec;
  std::filesystem::create_directories(host, ec);
  if (ec)
    return Status::Fail(38, "compat.fs",
                        "cannot create " + host + ": " + ec.message());
  return Status::Ok();
}

// --- ThreadService ---------------------------------------------------------
uint64_t ThreadService::spawn(std::function<void()> fn) {
  std::lock_guard<std::mutex> lock(mutex_);
  const uint64_t id = next_id_++;
  threads_.emplace(id, std::thread(std::move(fn)));
  return id;
}

void ThreadService::join_all() {
  std::map<uint64_t, std::thread> drained;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    drained.swap(threads_);
  }
  for (auto& [id, t] : drained)
    if (t.joinable()) t.join();
}

size_t ThreadService::live_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return threads_.size();
}

}  // namespace pnr::compat
