// S56: isolated allocation failures during ThreadService::spawn, no production hooks.
#include "compat/services.hpp"
#include <sys/wait.h>
#include <unistd.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>

namespace { int fail_after = -1; }
void* operator new(std::size_t n) {
  if (fail_after == 0) { fail_after = -1; throw std::bad_alloc(); }
  if (fail_after > 0) --fail_after;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

int main() {
  unsigned failed = 0;
  // Two allocations: registry node and std::thread state. Isolate each negative control,
  // since pre-fix map failure destroys a joinable thread and calls terminate.
  for (int index : {0, 1}) {
    std::fflush(stdout);
    const auto child = ::fork();
    if (child == 0) {
      ::alarm(5);
      pnr::compat::ThreadService service;
      std::atomic<int> ran{0};
      bool caught = false;
      fail_after = index;
      try { service.spawn([&] { ran.fetch_add(1); }); }
      catch (const std::bad_alloc&) { caught = true; }
      fail_after = -1;
      service.join_all();
      if (!caught || service.live_count() != 0 || ran.load() != 0) ::_exit(1);
      const auto id = service.spawn([&] { ran.fetch_add(1); });
      service.join_all();
      if (id != 1 || ran.load() != 1 || service.live_count() != 0) ::_exit(2);
      ::_exit(0);
    }
    int status = 0;
    const bool waited = child > 0 && ::waitpid(child, &status, 0) == child;
    const bool ok = waited && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    std::printf("%s thread-handoff: allocation failure %d rolls back without launch or ID consumption\n",
                ok ? "PASS" : "FAIL", index);
    if (!ok) ++failed;
  }
  std::fflush(stdout);
  const auto child = ::fork();
  if (child == 0) {
    ::alarm(5);
    pnr::compat::ThreadService service;
    std::atomic<bool> published{false}, refused{false}, finished{false};
    service.spawn([&] {
      while (!published.load()) std::this_thread::yield();
      try { service.join_all(); }
      catch (const std::logic_error&) { refused.store(true); }
      finished.store(true);
    });
    service.spawn([] {});
    published.store(true);
    while (!finished.load()) std::this_thread::yield();
    if (!refused.load() || service.live_count() != 2) ::_exit(1);
    service.join_all();
    if (service.live_count() != 0) ::_exit(2);
    ::_exit(0);
  }
  int status = 0;
  const bool waited = child > 0 && ::waitpid(child, &status, 0) == child;
  const bool ok = waited && WIFEXITED(status) && WEXITSTATUS(status) == 0;
  std::printf("%s thread-join: self-join refuses before draining, owner later joins both threads\n",
              ok ? "PASS" : "FAIL");
  if (!ok) ++failed;
  return failed ? 1 : 0;
}
