// PNR PRX - libkernel file calls (v1)
// sceKernelOpen/Read/Write/Close/Lseek/Fstat wired over the jailed
// FileSystemService: every path is resolved through the jail before any
// host syscall touches it. Descriptor table is ours; fds outside it or
// after close fail EINVAL, never touch the host fd space blindly.
// Status: Implemented, Tested.
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "compat/services.hpp"
#include "prx/prx.hpp"

namespace pnr::prx {

// PS-style open flags (subset; anything else is EINVAL).
constexpr std::int64_t SCE_KERNEL_O_RDONLY = 0x0000;
constexpr std::int64_t SCE_KERNEL_O_WRONLY = 0x0001;
constexpr std::int64_t SCE_KERNEL_O_RDWR   = 0x0002;
constexpr std::int64_t SCE_KERNEL_O_CREAT  = 0x0200;
constexpr std::int64_t SCE_KERNEL_O_TRUNC  = 0x0400;
constexpr std::int64_t SCE_KERNEL_O_APPEND = 0x0008;

constexpr std::int64_t SCE_KERNEL_SEEK_SET = 0;
constexpr std::int64_t SCE_KERNEL_SEEK_CUR = 1;
constexpr std::int64_t SCE_KERNEL_SEEK_END = 2;

struct SceKernelStat {
  std::int64_t size = 0;
  bool is_directory = false;
};

// One jailed descriptor table for one filesystem area.
class PsFileTable {
 public:
  explicit PsFileTable(compat::FileSystemService fs);

  // Returns fd >= 3 on success, or a negative SCE_KERNEL_ERROR_*.
  std::int64_t open(const std::string& path, std::int64_t flags);
  // Returns bytes read/written >= 0, or negative SCE_KERNEL_ERROR_*.
  std::int64_t read(std::int64_t fd, void* buf, std::size_t n);
  std::int64_t write(std::int64_t fd, const void* buf, std::size_t n);
  SceInt32 close(std::int64_t fd);
  // Returns new offset >= 0, or negative SCE_KERNEL_ERROR_*.
  std::int64_t lseek(std::int64_t fd, std::int64_t offset, std::int64_t whence);
  SceInt32 fstat(std::int64_t fd, SceKernelStat* out);

 private:
  compat::FileSystemService fs_;
  struct Entry { int host_fd = -1; std::string path; };
  std::mutex mu_;
  std::map<std::int64_t, Entry> entries_;
  std::int64_t next_fd_ = 3;
};

}  // namespace pnr::prx
