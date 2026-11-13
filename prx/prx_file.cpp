#include "prx/prx_file.hpp"

#include <fcntl.h>
#include <sys/stat.h>

#include <filesystem>
#include <unistd.h>

namespace pnr::prx {

PsFileTable::PsFileTable(compat::FileSystemService fs) : fs_(std::move(fs)) {}

std::int64_t PsFileTable::open(const std::string& path, std::int64_t flags) {
  const std::int64_t known = SCE_KERNEL_O_CREAT | SCE_KERNEL_O_TRUNC |
                             SCE_KERNEL_O_APPEND;
  if ((flags & ~known & ~0x3) != 0 || (flags & 0x3) > SCE_KERNEL_O_RDWR)
    return SCE_KERNEL_ERROR_EINVAL;
  std::string host;
  if (!fs_.resolve(path, host)) return SCE_KERNEL_ERROR_EINVAL;  // jail says no
  int hflags = 0;
  switch (flags & 0x3) {
    case SCE_KERNEL_O_RDONLY: hflags = O_RDONLY; break;
    case SCE_KERNEL_O_WRONLY: hflags = O_WRONLY; break;
    case SCE_KERNEL_O_RDWR:   hflags = O_RDWR;   break;
  }
  if (flags & SCE_KERNEL_O_CREAT)  hflags |= O_CREAT;
  if (flags & SCE_KERNEL_O_TRUNC)  hflags |= O_TRUNC;
  if (flags & SCE_KERNEL_O_APPEND) hflags |= O_APPEND;
  if (flags & SCE_KERNEL_O_CREAT) {
    // Same courtesy as FileSystemService::write_file: parents are created
    // inside the jail before the host open.
    const auto slash = host.find_last_of('/');
    if (slash != std::string::npos && slash > 0) {
      std::error_code ec;
      std::filesystem::create_directories(host.substr(0, slash), ec);
      if (ec) return SCE_KERNEL_ERROR_UNKNOWN;
    }
  }
  const int hfd = ::open(host.c_str(), hflags, 0644);
  if (hfd < 0) return SCE_KERNEL_ERROR_UNKNOWN;
  std::lock_guard<std::mutex> lk(mu_);
  const std::int64_t fd = next_fd_++;
  entries_[fd] = Entry{hfd, path};
  return fd;
}

std::int64_t PsFileTable::read(std::int64_t fd, void* buf, std::size_t n) {
  if (buf == nullptr && n > 0) return SCE_KERNEL_ERROR_EINVAL;
  std::lock_guard<std::mutex> lk(mu_);
  auto it = entries_.find(fd);
  if (it == entries_.end()) return SCE_KERNEL_ERROR_EINVAL;
  const ssize_t r = ::read(it->second.host_fd, buf, n);
  return r < 0 ? SCE_KERNEL_ERROR_UNKNOWN : static_cast<std::int64_t>(r);
}

std::int64_t PsFileTable::write(std::int64_t fd, const void* buf, std::size_t n) {
  if (buf == nullptr && n > 0) return SCE_KERNEL_ERROR_EINVAL;
  std::lock_guard<std::mutex> lk(mu_);
  auto it = entries_.find(fd);
  if (it == entries_.end()) return SCE_KERNEL_ERROR_EINVAL;
  const ssize_t w = ::write(it->second.host_fd, buf, n);
  return w < 0 ? SCE_KERNEL_ERROR_UNKNOWN : static_cast<std::int64_t>(w);
}

SceInt32 PsFileTable::close(std::int64_t fd) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = entries_.find(fd);
  if (it == entries_.end()) return SCE_KERNEL_ERROR_EINVAL;
  ::close(it->second.host_fd);
  entries_.erase(it);
  return SCE_OK;
}

std::int64_t PsFileTable::lseek(std::int64_t fd, std::int64_t offset,
                                std::int64_t whence) {
  int hw = 0;
  switch (whence) {
    case SCE_KERNEL_SEEK_SET: hw = SEEK_SET; break;
    case SCE_KERNEL_SEEK_CUR: hw = SEEK_CUR; break;
    case SCE_KERNEL_SEEK_END: hw = SEEK_END; break;
    default: return SCE_KERNEL_ERROR_EINVAL;
  }
  std::lock_guard<std::mutex> lk(mu_);
  auto it = entries_.find(fd);
  if (it == entries_.end()) return SCE_KERNEL_ERROR_EINVAL;
  const off_t r = ::lseek(it->second.host_fd, offset, hw);
  return r < 0 ? SCE_KERNEL_ERROR_UNKNOWN : static_cast<std::int64_t>(r);
}

SceInt32 PsFileTable::fstat(std::int64_t fd, SceKernelStat* out) {
  if (out == nullptr) return SCE_KERNEL_ERROR_EINVAL;
  std::lock_guard<std::mutex> lk(mu_);
  auto it = entries_.find(fd);
  if (it == entries_.end()) return SCE_KERNEL_ERROR_EINVAL;
  struct stat st {};
  if (::fstat(it->second.host_fd, &st) != 0) return SCE_KERNEL_ERROR_UNKNOWN;
  out->size = static_cast<std::int64_t>(st.st_size);
  out->is_directory = S_ISDIR(st.st_mode);
  return SCE_OK;
}

}  // namespace pnr::prx
