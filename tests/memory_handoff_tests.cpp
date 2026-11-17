// S54: isolated Linux fault injection. No production hooks or guest ABI involved.
#include "compat/services.hpp"
#include <cstdio>
#include <cstdlib>
#include <new>

namespace {
bool fail_next_new = false;
void* pending = nullptr;
unsigned frees = 0;
}

void* operator new(std::size_t n) {
  if (fail_next_new) { fail_next_new = false; throw std::bad_alloc(); }
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

extern "C" int __real_posix_memalign(void**, std::size_t, std::size_t);
extern "C" void __real_free(void*);
extern "C" int __wrap_posix_memalign(void** out, std::size_t alignment, std::size_t size) {
  const int result = __real_posix_memalign(out, alignment, size);
  if (result == 0 && fail_next_new) pending = *out;
  return result;
}
extern "C" void __wrap_free(void* p) {
  if (p && p == pending) { ++frees; pending = nullptr; }
  __real_free(p);
}

int main() {
  unsigned failed = 0;
  auto check = [&](bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failed;
  };
  pnr::compat::MemoryService memory;
  bool caught = false;
  fail_next_new = true;  // posix_memalign succeeds, then the tracking-map node allocation fails
  try { memory.allocate(256, 64); }
  catch (const std::bad_alloc&) { caught = true; }
  fail_next_new = false;
  check(caught, "handoff: map-node bad_alloc propagates unchanged");
  check(frees == 1 && pending == nullptr, "handoff: untracked aligned allocation freed exactly once");
  check(memory.live_allocations() == 0 && memory.bytes_outstanding() == 0,
        "handoff: failed allocation leaves tracking empty");
  // Clean the old implementation's leaked block so the negative control itself leaves no leak.
  if (pending) { __real_free(pending); pending = nullptr; }
  void* p = memory.allocate(128, 64);
  check(p && memory.live_allocations() == 1 && memory.bytes_outstanding() == 128,
        "handoff: service remains usable after map insertion failure");
  memory.deallocate(p);
  check(memory.live_allocations() == 0 && memory.bytes_outstanding() == 0,
        "handoff: subsequent successful allocation deallocates normally");
  return failed ? 1 : 0;
}
