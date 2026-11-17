#include "loader/guest_runtime.hpp"

#include <pthread.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <limits>
#include <mutex>

namespace pnr::loader {

namespace {
constexpr int kMaxGuestThreads = 32;
inline std::uint64_t raw_get_fs() {
  std::uint64_t v = 0;
  long r = 158;  // SYS_arch_prctl
  __asm__ volatile("syscall" : "+a"(r) : "D"(0x1003), "S"(&v) : "rcx", "r11", "memory");
  return v;
}
inline void raw_set_fs(std::uint64_t v) {
  long r = 158;
  __asm__ volatile("syscall" : "+a"(r) : "D"(0x1002), "S"(v) : "rcx", "r11", "memory");
}

struct FsPair { std::atomic<std::uint64_t> guest{0}; std::uint64_t host = 0; };
FsPair g_fs[kMaxGuestThreads + 1];
std::atomic<int> g_active{0};
TlsTemplate g_tls;
std::atomic<bool> g_have_tls{false};

bool fs_register(std::uint64_t guest, std::uint64_t host) {
  for (auto& p : g_fs) {
    std::uint64_t z = 0;
    if (p.guest.load() == 0) {
      p.host = host;
      if (p.guest.compare_exchange_strong(z, guest)) { g_active.fetch_add(1); return true; }
    }
  }
  return false;
}
void fs_unregister(std::uint64_t guest) {
  for (auto& p : g_fs) {
    std::uint64_t g = guest;
    if (p.guest.compare_exchange_strong(g, 0)) { g_active.fetch_sub(1); return; }
  }
}

struct Tls {
  std::uint8_t* mem = nullptr;
  std::size_t size = 0;
  std::uint64_t fs = 0;
};
bool tls_alloc(const TlsTemplate& t, Tls* out) {
  const TlsLayout l = tls_layout(t);
  void* m = ::mmap(nullptr, l.size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m == MAP_FAILED) return false;
  out->mem = static_cast<std::uint8_t*>(m);
  out->size = l.size;
  out->fs = reinterpret_cast<std::uint64_t>(out->mem) + l.fs_offset;
  std::uint64_t host_tcb[8] = {};
  const std::uint64_t hfs = raw_get_fs();
  if (hfs) std::memcpy(host_tcb, reinterpret_cast<void*>(hfs), sizeof host_tcb);
  tls_build(t, l, out->mem, out->fs, host_tcb);
  return true;
}

// No stack protector, no TLS access: FS is the guest base while these run.
__attribute__((noinline, optimize("no-stack-protector"))) std::int64_t call_with_fs(std::int64_t (*fn)(void*, void*), void* a, void* b,
                                                                                   std::uint64_t guest_fs, std::uint64_t host_fs) {
  raw_set_fs(guest_fs);
  const std::int64_t r = fn(a, b);
  raw_set_fs(host_fs);
  return r;
}

struct Thr {
  pthread_t th{};
  std::int64_t entry = 0, arg = 0, ret = 0;
  bool started = false, joined = false;
};

__attribute__((optimize("no-stack-protector"))) std::int64_t guest_thread_tramp(void* a, void*) {
  auto* t = static_cast<Thr*>(a);
  return reinterpret_cast<std::int64_t (*)(std::int64_t)>(t->entry)(t->arg);
}
void run_thread_dtors(std::uint64_t guest_fs, std::uint64_t host_fs);  // defined below

void* thread_main(void* p) {
  auto* t = static_cast<Thr*>(p);
  Tls tl;
  if (!tls_alloc(g_tls, &tl)) { t->ret = -1; return nullptr; }
  const std::uint64_t host_fs = raw_get_fs();
  if (!fs_register(tl.fs, host_fs)) { ::munmap(tl.mem, tl.size); t->ret = -1; return nullptr; }
  const std::int64_t r = call_with_fs(&guest_thread_tramp, t, nullptr, tl.fs, host_fs);
  run_thread_dtors(tl.fs, host_fs);  // S34: this thread's __cxa_thread_atexit_impl handlers
  fs_unregister(tl.fs);
  ::munmap(tl.mem, tl.size);
  t->ret = r;
  return nullptr;
}

std::mutex g_thr_mu;
std::vector<Thr*>& thr_list() { static std::vector<Thr*> v; return v; }

// ---- Module lifecycle (slices S32/S34): atexit table + the loaded module's fini list.
// Lives in the forked child: written before guest code starts, read during the exit sequence.
// S34: entries carry one argument (__cxa_atexit(fn, arg, dso); plain atexit registers arg=0),
// and __cxa_thread_atexit_impl registers per calling thread, keyed by that thread's host FS.
struct AtexitEntry { std::uint64_t fn = 0, arg = 0; };
std::mutex g_exit_mu;
std::vector<AtexitEntry> g_atexit;  // LIFO order at run time
std::map<std::uint64_t, std::vector<AtexitEntry>> g_tls_atexit;  // host FS -> handlers, LIFO
GuestLifecycle g_lc;
constexpr std::size_t kMaxAtexit = 64;

// The guest FS base registered for the thread whose HOST FS base is `host_fs`, or 0.
std::uint64_t current_guest_fs(std::uint64_t host_fs) {
  for (auto& p : g_fs)
    if (p.host == host_fs) {
      const std::uint64_t g = p.guest.load(std::memory_order_relaxed);
      if (g) return g;
    }
  return 0;
}

// Exit order: the exiting thread's __cxa_thread_atexit_impl handlers (LIFO), then atexit /
// __cxa_atexit handlers (LIFO), then the module fini list in (already reversed) order. The
// thread-handler-first order matches glibc (__call_tls_dtors before the atexit list). Every
// call runs with the guest FS base via call_with_fs and receives its registered argument.
void run_thread_dtors(std::uint64_t guest_fs, std::uint64_t host_fs) {
  std::vector<AtexitEntry> dtors;
  { std::lock_guard<std::mutex> l(g_exit_mu);
    auto it = g_tls_atexit.find(host_fs);
    if (it != g_tls_atexit.end()) { dtors.swap(it->second); g_tls_atexit.erase(it); } }
  using Fn2 = std::int64_t (*)(void*, void*);
  for (auto it = dtors.rbegin(); it != dtors.rend(); ++it)
    call_with_fs(reinterpret_cast<Fn2>(it->fn), reinterpret_cast<void*>(it->arg), nullptr, guest_fs, host_fs);
}

void run_exit_sequence(std::uint64_t guest_fs, std::uint64_t host_fs) {
  run_thread_dtors(guest_fs, host_fs);
  std::vector<AtexitEntry> handlers;
  GuestLifecycle lc;
  { std::lock_guard<std::mutex> l(g_exit_mu); handlers.swap(g_atexit); lc = g_lc; }
  using Fn2 = std::int64_t (*)(void*, void*);
  for (auto it = handlers.rbegin(); it != handlers.rend(); ++it)
    call_with_fs(reinterpret_cast<Fn2>(it->fn), reinterpret_cast<void*>(it->arg), nullptr, guest_fs, host_fs);
  for (const std::uint64_t f : lc.fini) call_with_fs(reinterpret_cast<Fn2>(f), nullptr, nullptr, guest_fs, host_fs);
}
}  // namespace

// ---------------- TLS
TlsLayout tls_layout(const TlsTemplate& t) {
  std::uint64_t a = t.align < 1 ? 1 : (t.align > 4096 ? 4096 : t.align);
  while (a & (a - 1)) a &= a - 1;  // power of two (round down)
  if (a < 16) a = 16;
  TlsLayout l;
  const std::uint64_t data = (t.memsz + a - 1) / a * a;
  l.tls_start = kPad;  // zeroed guard space below the TLS data
  l.fs_offset = kPad + static_cast<std::size_t>(data);
  l.size = l.fs_offset + kTcbSize;
  return l;
}

void tls_build(const TlsTemplate& t, const TlsLayout& l, std::uint8_t* block, std::uint64_t fs_value, const std::uint64_t* host_tcb) {
  std::memset(block, 0, l.size);
  // Variable at TLS offset o (0 <= o < memsz) lives at fs - round_up(memsz) + o.
  if (t.init && t.filesz) std::memcpy(block + l.tls_start, t.init, static_cast<std::size_t>(t.filesz < t.memsz ? t.filesz : t.memsz));
  std::memcpy(block + l.fs_offset, &fs_value, 8);       // TCB self pointer
  std::memcpy(block + l.fs_offset + 0x10, &fs_value, 8);  // glibc-style header.self
  if (host_tcb) {
    std::memcpy(block + l.fs_offset + 0x28, &host_tcb[5], 8);  // stack protector canary
    std::memcpy(block + l.fs_offset + 0x30, &host_tcb[6], 8);  // pointer guard
  }
}

// ---------------- Sony constants
bool translate_sony_prot(std::int64_t prot, int* hp, std::string* why) {
  const std::int64_t known = SONY_PROT_CPU_READ | SONY_PROT_CPU_WRITE | SONY_PROT_CPU_EXEC | SONY_PROT_GPU_READ | SONY_PROT_GPU_WRITE;
  if (prot & ~known) { if (why) *why = "unknown protection bits"; return false; }
  int p = 0;
  if (prot & SONY_PROT_CPU_READ) p |= PROT_READ;
  if (prot & SONY_PROT_CPU_WRITE) p |= PROT_READ | PROT_WRITE;  // CPU write implies read
  if (prot & SONY_PROT_CPU_EXEC) p |= PROT_EXEC;
  *hp = p;  // GPU bits accepted, ignored
  return true;
}

bool translate_sony_map_flags(std::int64_t flags, std::int64_t fd, int* hf, std::string* why) {
  const std::int64_t known = SONY_MAP_SHARED | SONY_MAP_PRIVATE | SONY_MAP_FIXED | SONY_MAP_NO_OVERWRITE | SONY_MAP_ANON | SONY_MAP_NOCORE | SONY_MAP_NO_COALESCE;
  if (flags & ~known) { if (why) *why = "unknown map flag bits"; return false; }
  if (!(flags & SONY_MAP_ANON)) { if (why) *why = "only anonymous mappings are supported"; return false; }
  if (static_cast<std::int32_t>(fd) != -1 && fd != 0) { if (why) *why = "anonymous mapping with a file descriptor"; return false; }
  if ((flags & SONY_MAP_SHARED) && (flags & SONY_MAP_PRIVATE)) { if (why) *why = "SHARED and PRIVATE together"; return false; }
  if ((flags & SONY_MAP_NO_OVERWRITE) && !(flags & SONY_MAP_FIXED)) { if (why) *why = "NO_OVERWRITE needs FIXED"; return false; }
  int f = MAP_ANONYMOUS | ((flags & SONY_MAP_SHARED) ? MAP_SHARED : MAP_PRIVATE);
  if (flags & SONY_MAP_FIXED) f |= (flags & SONY_MAP_NO_OVERWRITE) ? MAP_FIXED_NOREPLACE : MAP_FIXED;
  *hf = f;
  return true;
}

bool translate_sony_clock(std::int64_t id, int* h) {
  switch (id) {
    case 0: case 9: case 10: case 13: *h = CLOCK_REALTIME; return true;
    case 2: *h = CLOCK_PROCESS_CPUTIME_ID; return true;
    case 4: case 5: case 7: case 8: case 11: case 12: *h = CLOCK_MONOTONIC; return true;
    case 14: *h = CLOCK_THREAD_CPUTIME_ID; return true;
    default: return false;
  }
}

std::int64_t sony_mmap(std::int64_t addr, std::int64_t len, std::int64_t prot, std::int64_t flags, std::int64_t fd, std::int64_t) {
  int hp = 0, hf = 0;
  if (len <= 0 || !translate_sony_prot(prot, &hp, nullptr) || !translate_sony_map_flags(flags, fd, &hf, nullptr)) return -1;
  void* p = ::mmap(reinterpret_cast<void*>(addr), static_cast<std::size_t>(len), hp, hf, -1, 0);
  return p == MAP_FAILED ? -1 : reinterpret_cast<std::int64_t>(p);
}

std::int64_t sony_clock_gettime(std::int64_t id, std::int64_t ts_ptr) {
  int c = 0;
  if (!ts_ptr || !translate_sony_clock(id, &c)) return -2;  // SCE EINVAL (PNR value, see prx.hpp)
  timespec ts{};
  if (::clock_gettime(c, &ts) != 0) return -3;
  std::memcpy(reinterpret_cast<void*>(ts_ptr), &ts, sizeof ts);
  return 0;
}

// ---------------- entry parameters
std::vector<std::uint8_t> build_entry_args(const std::vector<std::string>& argv) {
  const std::size_t n = argv.size();
  const std::size_t head = (1 + n + 2) * 8;
  std::size_t total = head;
  for (const auto& s : argv) total += s.size() + 1;
  std::vector<std::uint8_t> b(total, 0);
  const std::uint64_t argc = n;
  std::memcpy(b.data(), &argc, 8);
  std::size_t so = head;
  for (std::size_t i = 0; i < n; ++i) {
    std::memcpy(&b[so], argv[i].c_str(), argv[i].size() + 1);
    const std::uint64_t ptr = reinterpret_cast<std::uint64_t>(b.data()) + so;
    std::memcpy(&b[8 + 8 * i], &ptr, 8);
    so += argv[i].size() + 1;
  }
  return b;  // NOTE: pointers are valid while the returned vector's buffer is not moved or reallocated
}

// ---------------- process-level
std::int64_t run_guest_main(const TlsTemplate& t, std::int64_t (*fn)(void*, void*), void* a, void* b) {
  return run_guest_main(t, fn, a, b, GuestLifecycle{});
}

std::int64_t run_guest_main(const TlsTemplate& t, std::int64_t (*fn)(void*, void*), void* a, void* b, const GuestLifecycle& lc) {
  g_tls = t;
  g_have_tls = true;
  { std::lock_guard<std::mutex> l(g_exit_mu); g_lc = lc; g_atexit.clear(); }
  Tls tl;
  if (!tls_alloc(t, &tl)) return -1;
  const std::uint64_t host_fs = raw_get_fs();
  if (!fs_register(tl.fs, host_fs)) return -1;
  using Fn2 = std::int64_t (*)(void*, void*);
  for (const std::uint64_t f : lc.init) call_with_fs(reinterpret_cast<Fn2>(f), a, b, tl.fs, host_fs);
  const std::int64_t r = call_with_fs(fn, a, b, tl.fs, host_fs);
  run_exit_sequence(tl.fs, host_fs);
  fs_unregister(tl.fs);
  return r;
}

std::int64_t guest_atexit_register(std::uint64_t fn) { return guest_cxa_atexit_register(fn, 0); }

std::int64_t guest_cxa_atexit_register(std::uint64_t fn, std::uint64_t arg) {
  if (!fn || !guest_active()) return -2;
  std::lock_guard<std::mutex> l(g_exit_mu);
  if (g_atexit.size() >= kMaxAtexit) return -1;
  g_atexit.push_back({fn, arg});
  return 0;
}

std::int64_t guest_cxa_thread_atexit_register(std::uint64_t fn, std::uint64_t arg) {
  if (!fn || !guest_active()) return -2;
  const std::uint64_t host_fs = raw_get_fs();  // inside a thunk the FS is already the host base
  if (!host_fs || !current_guest_fs(host_fs)) return -2;  // not a thread running guest code
  std::lock_guard<std::mutex> l(g_exit_mu);
  auto& v = g_tls_atexit[host_fs];
  if (v.size() >= kMaxAtexit) return -1;
  v.push_back({fn, arg});
  return 0;
}

[[noreturn]] void guest_exit_with(int code) {
  std::fflush(stdout);
  std::fflush(stderr);
  if (guest_active()) {
    const std::uint64_t host_fs = raw_get_fs();
    const std::uint64_t g = current_guest_fs(host_fs);
    if (g) run_exit_sequence(g, host_fs);
  }
  // Handlers above may have printed: flush again before the unbuffered _exit.
  std::fflush(stdout);
  std::fflush(stderr);
  ::_exit(code);
}

__attribute__((optimize("no-stack-protector"))) std::int64_t call_guest_function(std::uint64_t addr, std::int64_t a, std::int64_t b) {
  if (!addr) return -2;
  const std::uint64_t host_fs = raw_get_fs();
  const std::uint64_t g = current_guest_fs(host_fs);
  if (!g) return -2;
  using Fn2 = std::int64_t (*)(std::int64_t, std::int64_t);
  raw_set_fs(g);
  const std::int64_t r = reinterpret_cast<Fn2>(addr)(a, b);
  raw_set_fs(host_fs);
  return r;
}

bool guest_active() { return g_active.load(std::memory_order_relaxed) != 0; }

__attribute__((optimize("no-stack-protector"))) std::uint64_t guest_to_host() {
  if (!g_active.load(std::memory_order_relaxed)) return 0;
  const std::uint64_t cur = raw_get_fs();
  for (auto& p : g_fs)
    if (p.guest.load(std::memory_order_relaxed) == cur) { raw_set_fs(p.host); return cur; }
  return 0;
}
__attribute__((optimize("no-stack-protector"))) void host_to_guest(std::uint64_t fs) {
  if (fs) raw_set_fs(fs);
}

// ---------------- threads and mutexes (host calls: FS is already the host base here)
std::int64_t guest_thread_create(std::int64_t* out, std::int64_t entry, std::int64_t arg) {
  if (!out || !entry || !g_have_tls.load()) return -2;
  auto* t = new Thr;
  t->entry = entry; t->arg = arg;
  if (::pthread_create(&t->th, nullptr, &thread_main, t) != 0) { delete t; return -3; }
  t->started = true;
  { std::lock_guard<std::mutex> l(g_thr_mu); thr_list().push_back(t); }
  *out = reinterpret_cast<std::int64_t>(t);
  return 0;
}
std::int64_t guest_thread_join(std::int64_t h, std::int64_t* ret) {
  Thr* t = nullptr;
  {
    std::lock_guard<std::mutex> l(g_thr_mu);
    auto& v = thr_list();
    for (std::size_t i = 0; i < v.size(); ++i)
      if (reinterpret_cast<std::int64_t>(v[i]) == h) { t = v[i]; v.erase(v.begin() + static_cast<long>(i)); break; }
  }
  if (!t) return -2;
  ::pthread_join(t->th, nullptr);
  if (ret) *ret = t->ret;
  delete t;
  return 0;
}
std::int64_t guest_thread_self() { return static_cast<std::int64_t>(::pthread_self()); }

namespace {
struct GuestMutex {
  pthread_mutex_t native{};
  bool initialized = false;
  std::atomic<unsigned> held{0};
  ~GuestMutex() { if (initialized) ::pthread_mutex_destroy(&native); }
};
std::mutex g_mutex_mu;
std::map<std::int64_t, std::shared_ptr<GuestMutex>> g_mutexes;
std::int64_t g_mutex_next = 0x100000;
std::shared_ptr<GuestMutex> find_mutex(std::int64_t id) {
  std::lock_guard<std::mutex> lock(g_mutex_mu);
  const auto it = g_mutexes.find(id);
  return it == g_mutexes.end() ? nullptr : it->second;
}
}  // namespace

std::int64_t guest_mutex_op(int op, std::int64_t* handle) {
  if (!handle) return -2;
  if (op == 0) {
    auto m = std::make_shared<GuestMutex>();
    if (::pthread_mutex_init(&m->native, nullptr) != 0) return -3;
    m->initialized = true;
    std::lock_guard<std::mutex> lock(g_mutex_mu);
    if (g_mutex_next == std::numeric_limits<std::int64_t>::max()) return -3;
    const auto id = g_mutex_next;
    g_mutexes.emplace(id, m);
    ++g_mutex_next;
    *handle = id;  // publish only after native init and registry insertion
    return 0;
  }
  if (op == 3) {
    std::lock_guard<std::mutex> lock(g_mutex_mu);
    const auto it = g_mutexes.find(*handle);
    if (it == g_mutexes.end()) return -2;
    // Lookups retain shared ownership through native calls, including lock waiters and
    // condition waits. Successful locks retain a held count until a successful unlock.
    if (it->second.use_count() != 1 || it->second->held.load() != 0) return -3;
    if (::pthread_mutex_destroy(&it->second->native) != 0) return -3;
    it->second->initialized = false;
    g_mutexes.erase(it);
    *handle = 0;
    return 0;
  }
  if (op != 1 && op != 2) return -2;
  auto m = find_mutex(*handle);
  if (!m) return -2;
  if (op == 1) {
    if (::pthread_mutex_lock(&m->native) != 0) return -3;
    m->held.fetch_add(1);
  } else {
    if (::pthread_mutex_unlock(&m->native) != 0) return -3;
    m->held.fetch_sub(1);
  }
  return 0;
}

// ---- Kernel sync objects (slice S1)
namespace {
struct Cond {
  pthread_cond_t c{};
  bool initialized = false;
  ~Cond() { if (initialized) ::pthread_cond_destroy(&c); }
};
struct CondAttributes {
  pthread_condattr_t value{};
  bool initialized = false;
  ~CondAttributes() { if (initialized) ::pthread_condattr_destroy(&value); }
};
struct Sema { std::mutex m; std::condition_variable cv; std::int64_t count; std::int64_t max; bool dead = false; int waiters = 0; };
std::mutex g_sync_mu;
std::int64_t g_sync_next = 0x5000;
std::map<std::int64_t, std::shared_ptr<Cond>> g_conds;
std::map<std::int64_t, std::shared_ptr<Sema>> g_semas;
std::shared_ptr<Cond> find_cond(std::int64_t h) { std::lock_guard<std::mutex> g(g_sync_mu); auto i = g_conds.find(h); return i == g_conds.end() ? nullptr : i->second; }
std::shared_ptr<Sema> find_sema(std::int64_t h) { std::lock_guard<std::mutex> g(g_sync_mu); auto i = g_semas.find(h); return i == g_semas.end() ? nullptr : i->second; }
}  // namespace

std::int64_t guest_cond_init(std::int64_t* handle) {
  if (!handle) return -2;
  auto c = std::make_shared<Cond>();
  CondAttributes attributes;
  if (::pthread_condattr_init(&attributes.value) != 0) return -3;
  attributes.initialized = true;
  if (::pthread_condattr_setclock(&attributes.value, CLOCK_MONOTONIC) != 0) return -3;
  if (::pthread_cond_init(&c->c, &attributes.value) != 0) return -3;
  c->initialized = true;
  std::lock_guard<std::mutex> g(g_sync_mu);
  if (g_sync_next == std::numeric_limits<std::int64_t>::max()) return -3;
  const auto id = g_sync_next;
  if (!g_conds.emplace(id, c).second) return -3;
  ++g_sync_next;
  *handle = id;
  return 0;
}
std::int64_t guest_cond_wait(std::int64_t cond, std::int64_t* mh, std::int64_t timeout_us) {
  auto c = find_cond(cond);
  if (!c || !mh) return -2;
  auto mutex = find_mutex(*mh);
  if (!mutex) return -2;
  auto* m = &mutex->native;
  if (timeout_us < 0) return ::pthread_cond_wait(&c->c, m) == 0 ? 0 : -3;
  timespec ts{}; ::clock_gettime(CLOCK_MONOTONIC, &ts);
  const std::int64_t ns = ts.tv_nsec + (timeout_us % 1000000) * 1000;
  ts.tv_sec += timeout_us / 1000000 + ns / 1000000000; ts.tv_nsec = ns % 1000000000;
  const int r = ::pthread_cond_timedwait(&c->c, m, &ts);
  return r == 0 ? 0 : (r == ETIMEDOUT ? -4 : -3);
}
std::int64_t guest_cond_wake(std::int64_t cond, bool all) {
  auto c = find_cond(cond); if (!c) return -2;
  return (all ? ::pthread_cond_broadcast(&c->c) : ::pthread_cond_signal(&c->c)) == 0 ? 0 : -3;
}
std::int64_t guest_cond_destroy(std::int64_t* handle) {
  if (!handle) return -2;
  std::lock_guard<std::mutex> g(g_sync_mu);
  auto i = g_conds.find(*handle); if (i == g_conds.end()) return -2;
  // Wait/wake lookups keep a reference until their native call has returned. Refuse
  // before pthread_cond_destroy: destroying with blocked waiters is undefined by POSIX.
  if (i->second.use_count() != 1) return -3;
  if (::pthread_cond_destroy(&i->second->c) != 0) return -3;
  i->second->initialized = false;
  g_conds.erase(i);
  *handle = 0;
  return 0;
}
std::int64_t guest_sema_create(std::int64_t* handle, std::int64_t initial, std::int64_t max) {
  if (!handle || max <= 0 || max > 0x7fffffff || initial < 0 || initial > max) return -2;
  auto s = std::make_shared<Sema>(); s->count = initial; s->max = max;
  std::lock_guard<std::mutex> g(g_sync_mu);
  *handle = g_sync_next++; g_semas[*handle] = s; return 0;
}
std::int64_t guest_sema_wait(std::int64_t sema, std::int64_t need, std::int64_t timeout_us) {
  auto s = find_sema(sema);
  if (!s || need <= 0 || need > s->max) return -2;
  std::unique_lock<std::mutex> l(s->m);
  auto ready = [&] { return s->dead || s->count >= need; };
  ++s->waiters;
  bool ok = true;
  if (timeout_us < 0) s->cv.wait(l, ready);
  else ok = s->cv.wait_for(l, std::chrono::microseconds(timeout_us), ready);
  --s->waiters;
  if (s->dead) return -2;
  if (!ok) return -4;
  s->count -= need; return 0;
}
std::int64_t guest_sema_signal(std::int64_t sema, std::int64_t count) {
  auto s = find_sema(sema);
  if (!s || count <= 0) return -2;
  std::lock_guard<std::mutex> l(s->m);
  // count is untrusted and can be INT64_MAX. Check remaining capacity before addition;
  // the invariant 0 <= s->count <= s->max makes this subtraction nonnegative and safe.
  if (count > s->max - s->count) return -3;  // refused, count unchanged
  s->count += count; s->cv.notify_all(); return 0;
}
std::int64_t guest_sema_destroy(std::int64_t* handle) {
  if (!handle) return -2;
  std::shared_ptr<Sema> s;
  { std::lock_guard<std::mutex> g(g_sync_mu);
    auto i = g_semas.find(*handle); if (i == g_semas.end()) return -2;
    s = i->second; g_semas.erase(i); *handle = 0; }
  { std::lock_guard<std::mutex> l(s->m); s->dead = true; }   // wakes waiters with an error
  s->cv.notify_all(); return 0;
}

}  // namespace pnr::loader
