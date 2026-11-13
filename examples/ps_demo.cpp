// PNR PS-style demo "game" (developer-controlled).
// Its own code calls ONLY sce* system APIs - every one of them is an
// import resolved at launch by the PNR runtime and dispatched through
// the PS-ABI shim (libpnr_psabi.so). If the work below really happened,
// it exits 0 and leaves proof inside the jail at /psdemo_proof.txt.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" {
void* sceKernelAlloc(std::size_t);
std::int32_t sceKernelFree(void*);
std::int32_t sceKernelUsleep(std::uint32_t);
std::int32_t sceKernelCreateThread(const char*, void (*)(void*), void*);
void* sceKernelMapMemory(std::size_t, std::int64_t);
std::int32_t sceKernelMunmap(void*, std::size_t);
std::int32_t sceKernelMTypeprotect(void*, std::size_t, std::int64_t);
std::int64_t sceKernelOpen(const char*, std::int64_t);
std::int64_t sceKernelRead(std::int64_t, void*, std::size_t);
std::int64_t sceKernelWrite(std::int64_t, const void*, std::size_t);
std::int32_t sceKernelClose(std::int64_t);
std::int64_t sceKernelLseek(std::int64_t, std::int64_t, std::int64_t);
std::int32_t sceKernelFstat(std::int64_t, std::int64_t*);
}

static int fail(const char* what) {
  std::printf("ps_demo: FAIL %s\n", what);
  return 1;
}

int main() {
  // 1. kernel heap: alloc, fill, free
  auto* block = static_cast<char*>(sceKernelAlloc(256));
  if (block == nullptr) return fail("sceKernelAlloc");
  for (int i = 0; i < 256; ++i) block[i] = static_cast<char>(i);
  if (sceKernelFree(block) != 0) return fail("sceKernelFree");
  std::printf("ps_demo: heap alloc/fill/free ok\n");

  // 2. memory mapping: map RW, write, protect RO, unmap
  void* mm = sceKernelMapMemory(4096, 0x1 | 0x2);
  if (mm == nullptr) return fail("sceKernelMapMemory");
  std::memset(mm, 0x11, 4096);
  if (sceKernelMTypeprotect(mm, 4096, 0x1) != 0) return fail("MTypeprotect");
  if (sceKernelMunmap(mm, 4096) != 0) return fail("sceKernelMunmap");
  std::printf("ps_demo: map/protect/unmap ok\n");

  // 3. jailed file I/O: create, write, stat, seek, read back
  const char* payload = "proof the sce file calls work";
  const std::int64_t fd = sceKernelOpen("/psdemo_proof.txt", 0x1 | 0x0200 | 0x0400);
  if (fd < 0) return fail("sceKernelOpen");
  if (sceKernelWrite(fd, payload, std::strlen(payload)) !=
      static_cast<std::int64_t>(std::strlen(payload)))
    return fail("sceKernelWrite");
  std::int64_t size = 0;
  if (sceKernelFstat(fd, &size) != 0 || size != 29) return fail("sceKernelFstat");
  if (sceKernelClose(fd) != 0) return fail("sceKernelClose");
  // read-back on a fresh read-only descriptor
  const std::int64_t rfd = sceKernelOpen("/psdemo_proof.txt", 0x0);
  if (rfd < 0) return fail("sceKernelOpen ro");
  char back[64] = {};
  if (sceKernelRead(rfd, back, 29) != 29 || std::strcmp(back, payload) != 0)
    return fail("sceKernelRead");
  if (sceKernelLseek(rfd, 0, 0) != 0) return fail("sceKernelLseek");
  if (sceKernelClose(rfd) != 0) return fail("sceKernelClose ro");
  std::printf("ps_demo: jailed file write/stat/seek/read ok\n");

  // 4. timing: usleep really sleeps
  const auto t0 = std::chrono::steady_clock::now();
  if (sceKernelUsleep(20000) != 0) return fail("sceKernelUsleep");
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - t0).count();
  if (us < 15000) return fail("sleep too short");
  std::printf("ps_demo: usleep measured %lldus\n", static_cast<long long>(us));

  // 5. threads: entry really runs
  std::atomic<int> hits{0};
  auto* entry = +[](void* a) { ++*static_cast<std::atomic<int>*>(a); };
  if (sceKernelCreateThread("worker", entry, &hits) != 0)
    return fail("sceKernelCreateThread");
  for (int i = 0; i < 200 && hits.load() == 0; ++i) sceKernelUsleep(1000);
  if (hits.load() != 1) return fail("thread entry");
  std::printf("ps_demo: thread ran\n");

  std::printf("ps_demo: ALL SCE CALLS DISPATCHED OK\n");
  return 0;
}
