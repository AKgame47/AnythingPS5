// PNR PS-ABI shim - the in-process binding layer (v1)
// A shared library exporting the sce* symbols a PS-style binary imports.
// At launch the runtime points the game at this library (LD_LIBRARY_PATH)
// and at its jail (PNR_JAIL_ROOT); calls then do real work natively in the
// game process: kernel-style services over host equivalents, file calls
// through the SAME jailed FileSystemService as the host-side PsFileTable.
// (Application-level path routing, not a security boundary - see
// compat/services.hpp.)
// This is how the registry's prx-implemented promises get kept.
// Taxonomy per AnyPS5 (GPL-2.0); implementation original. Status: Implemented, Tested.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <filesystem>
#include <thread>
#include <vector>

#include <sys/mman.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "compat/services.hpp"

namespace {
pnr::compat::FileSystemService& jail() {
  static pnr::compat::FileSystemService fs([] {
    const char* r = std::getenv("PNR_JAIL_ROOT");
    return std::string(r != nullptr ? r : "/tmp/pnr_psabi_default_jail");
  }());
  return fs;
}
struct Mapping { void* start; std::size_t size; };
std::mutex g_mu;
std::vector<Mapping>& maps() { static std::vector<Mapping> m; return m; }
}  // namespace

extern "C" {

void* sceKernelAlloc(std::size_t size) {
  return size == 0 ? nullptr : std::malloc(size);
}
std::int32_t sceKernelFree(void* p) { std::free(p); return 0; }

std::int32_t sceKernelUsleep(std::uint32_t us) {
  if (us > 60u * 1000u * 1000u) return -2;
  std::this_thread::sleep_for(std::chrono::microseconds(us));
  return 0;
}
std::int32_t sceKernelSleep(std::uint32_t s) {
  std::this_thread::sleep_for(std::chrono::seconds(s));
  return 0;
}
std::int32_t sceKernelCreateThread(const char*, void (*entry)(void*), void* arg) {
  if (entry == nullptr) return -2;
  try { std::thread(entry, arg).detach(); } catch (...) { return -3; }
  return 0;
}

void* sceKernelMapMemory(std::size_t size, std::int64_t prot) {
  if (size == 0 || prot < 0 || prot > 7) return nullptr;
  void* p = ::mmap(nullptr, size, static_cast<int>(prot),
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) return nullptr;
  std::lock_guard<std::mutex> lk(g_mu);
  maps().push_back(Mapping{p, size});
  return p;
}
std::int32_t sceKernelMunmap(void* addr, std::size_t size) {
  if (addr == nullptr || size == 0) return -2;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    bool ours = false;
    for (auto it = maps().begin(); it != maps().end(); ++it)
      if (it->start == addr) { maps().erase(it); ours = true; break; }
    if (!ours) return -2;
  }
  return ::munmap(addr, size) == 0 ? 0 : -3;
}
std::int32_t sceKernelMTypeprotect(void* addr, std::size_t size, std::int64_t prot) {
  if (addr == nullptr || size == 0 || prot < 0 || prot > 7) return -2;
  return ::mprotect(addr, size, static_cast<int>(prot)) == 0 ? 0 : -3;
}

std::int64_t sceKernelOpen(const char* path, std::int64_t flags) {
  std::string host;
  if (path == nullptr || !jail().resolve(path, host)) return -2;
  int hf = 0;
  switch (flags & 0x3) {
    case 0: hf = O_RDONLY; break;
    case 1: hf = O_WRONLY; break;
    case 2: hf = O_RDWR; break;
    default: return -2;
  }
  if (flags & 0x0200) {  // CREAT: parents inside the jail first
    const auto slash = host.find_last_of('/');
    if (slash != std::string::npos && slash > 0) {
      std::error_code ec;
      std::filesystem::create_directories(host.substr(0, slash), ec);
      if (ec) return -3;
    }
    hf |= O_CREAT;
  }
  if (flags & 0x0400) hf |= O_TRUNC;
  if (flags & 0x0008) hf |= O_APPEND;
  const int fd = ::open(host.c_str(), hf, 0644);
  return fd < 0 ? -3 : fd;
}
std::int64_t sceKernelRead(std::int64_t fd, void* buf, std::size_t n) {
  const ssize_t r = ::read(static_cast<int>(fd), buf, n);
  return r < 0 ? -3 : r;
}
std::int64_t sceKernelWrite(std::int64_t fd, const void* buf, std::size_t n) {
  const ssize_t w = ::write(static_cast<int>(fd), buf, n);
  return w < 0 ? -3 : w;
}
std::int32_t sceKernelClose(std::int64_t fd) {
  return ::close(static_cast<int>(fd)) == 0 ? 0 : -3;
}
std::int64_t sceKernelLseek(std::int64_t fd, std::int64_t off, std::int64_t whence) {
  if (whence < 0 || whence > 2) return -2;
  const off_t r = ::lseek(static_cast<int>(fd), off, static_cast<int>(whence));
  return r < 0 ? -3 : r;
}
std::int32_t sceKernelFstat(std::int64_t fd, std::int64_t* size_out) {
  if (size_out == nullptr) return -2;
  struct stat st {};
  if (::fstat(static_cast<int>(fd), &st) != 0) return -3;
  *size_out = st.st_size;
  return 0;
}

}  // extern "C"
