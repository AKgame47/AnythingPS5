// S57: actual post-open allocation failure injection, isolated from other suites.
#include "prx/prx_file.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>

namespace {
int armed_index = -1, fail_after = -1, pending_fd = -1;
unsigned closes = 0;
}
void* operator new(std::size_t n) {
  if (fail_after == 0) { fail_after = -1; throw std::bad_alloc(); }
  if (fail_after > 0) --fail_after;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
extern "C" int __real_open(const char*, int, ...);
extern "C" int __real_close(int);
extern "C" int __wrap_open(const char* path, int flags, ...) {
  int fd;
  if (flags & O_CREAT) {
    va_list args; va_start(args, flags);
    const auto mode = va_arg(args, unsigned int); va_end(args);
    fd = __real_open(path, flags, mode);
  } else fd = __real_open(path, flags);
  if (fd >= 0 && armed_index >= 0) {
    pending_fd = fd;
    fail_after = armed_index;
    armed_index = -1;
  }
  return fd;
}
extern "C" int __wrap_close(int fd) {
  if (fd == pending_fd) ++closes;
  return __real_close(fd);
}

int main() {
  char root[] = "/tmp/pnr_file_handoff_XXXXXX";
  if (!::mkdtemp(root)) return 2;
  const std::string guest = "/long-path-for-entry-allocation-check.txt";
  const std::string host = std::string(root) + guest;
  const int seed = __real_open(host.c_str(), O_CREAT | O_WRONLY, 0600);
  if (seed < 0) return 2;
  __real_close(seed);
  unsigned failed = 0;
  auto check = [&](bool ok, const char* label, int index) {
    std::printf("%s file-handoff[%d]: %s\n", ok ? "PASS" : "FAIL", index, label);
    if (!ok) ++failed;
  };
  for (int index : {0, 1}) {
    pnr::prx::PsFileTable table{pnr::compat::FileSystemService(root)};
    const auto before = closes;
    bool caught = false;
    armed_index = index;
    try { table.open(guest, pnr::prx::SCE_KERNEL_O_RDONLY); }
    catch (const std::bad_alloc&) { caught = true; }
    armed_index = fail_after = -1;
    check(caught, "post-open bad_alloc propagates", index);
    check(pending_fd >= 0 && closes == before + 1 && ::fcntl(pending_fd, F_GETFD) == -1,
          "pending host descriptor closes exactly once", index);
    if (closes == before && pending_fd >= 0) __real_close(pending_fd);
    pending_fd = -1;
    const auto fd = table.open(guest, pnr::prx::SCE_KERNEL_O_RDONLY);
    check(fd == 3, "failed insertion consumes no guest descriptor number", index);
    check(table.close(fd) == pnr::prx::SCE_OK,
          "subsequent successful open closes normally", index);
  }
  ::unlink(host.c_str()); ::rmdir(root);
  return failed ? 1 : 0;
}
