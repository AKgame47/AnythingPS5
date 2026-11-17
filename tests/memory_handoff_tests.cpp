// S54: isolated Linux fault injection. No production hooks or guest ABI involved.
#include "compat/services.hpp"
#include <cstdio>
#include <cstdlib>
#include <new>
#include <type_traits>

static_assert(!std::is_copy_constructible_v<pnr::compat::MemoryService>);
static_assert(!std::is_copy_assignable_v<pnr::compat::MemoryService>);
static_assert(!std::is_move_constructible_v<pnr::compat::MemoryService>);
static_assert(!std::is_move_assignable_v<pnr::compat::MemoryService>);

namespace {
bool fail_next_new = false;
bool capture_next_aligned = false;
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
  if (result == 0 && (fail_next_new || capture_next_aligned)) {
    pending = *out;
    capture_next_aligned = false;
  }
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
  const unsigned before_scope = frees;
  {
    pnr::compat::MemoryService scoped;
    capture_next_aligned = true;
    void* live = scoped.allocate(64, 32);
    check(live && scoped.bytes_outstanding() == 64,
          "teardown: outstanding allocation tracked before scope exit");
  }
  check(frees == before_scope + 1 && pending == nullptr,
        "teardown: scope exit frees the still-owned allocation exactly once");
  if (pending) { __real_free(pending); pending = nullptr; }
  const unsigned before_explicit = frees;
  {
    pnr::compat::MemoryService scoped;
    capture_next_aligned = true;
    void* live = scoped.allocate(64, 32);
    scoped.deallocate(live);
    check(frees == before_explicit + 1 && scoped.live_allocations() == 0,
          "teardown: explicit deallocation releases before destruction");
  }
  check(frees == before_explicit + 1,
        "teardown: destructor does not free an explicitly released block again");
  return failed ? 1 : 0;
}
