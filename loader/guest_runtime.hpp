// PNR Loader - guest runtime for translated modules (Core slice C2), GPL-2.0.
// Linux x86-64 only. Provides: x86-64 TLS/TCB layout (variant II), per-thread guest FS base
// switching around host calls, PlayStation-style entry parameters, Sony mmap/prot/clock
// translation, and guest threads.
//
// Status: Implemented, Tested with SYNTHETIC modules. Honest limits:
//  - Sony constant values: PROT/MAP bits other than the FreeBSD-standard ones (SHARED, PRIVATE,
//    FIXED, ANON) come from public homebrew header knowledge and are NOT verified on a console.
//    Unknown bits are refused, never guessed. Clock ids follow FreeBSD numbering, also unverified.
//  - guest threads: up to 32 at once. Host calls from a guest thread switch FS to the host base.
//  - GPU protection bits are accepted and ignored (no GPU memory model yet).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pnr::loader {

struct TlsTemplate {
  const std::uint8_t* init = nullptr;  // initialised data (PT_TLS file part), in the mapped image
  std::uint64_t filesz = 0, memsz = 0, align = 1;
};

// A built TLS block: [tls data][TCB]. fs_offset is the offset of the TCB (what FS points at).
struct TlsLayout {
  std::size_t size = 0, fs_offset = 0, tls_start = 0;
};
constexpr std::size_t kTcbSize = 0x100, kPad = 64;

// Pure: computes the layout for a template. align is clamped to [1, 4096] and rounded to a power of two.
TlsLayout tls_layout(const TlsTemplate& t);
// Pure: fills `block` (size >= layout.size, zeroed by this call). TCB word 0 and word 2 hold the
// TCB address `fs_value`; words at 0x28/0x30 are copied from `host_tcb` when non-null.
void tls_build(const TlsTemplate& t, const TlsLayout& l, std::uint8_t* block, std::uint64_t fs_value,
               const std::uint64_t* host_tcb);

// ---- Sony constants -> host (pure)
constexpr int SONY_PROT_CPU_READ = 1, SONY_PROT_CPU_WRITE = 2, SONY_PROT_CPU_EXEC = 4,
              SONY_PROT_GPU_READ = 0x10, SONY_PROT_GPU_WRITE = 0x20;
constexpr int SONY_MAP_SHARED = 1, SONY_MAP_PRIVATE = 2, SONY_MAP_FIXED = 0x10, SONY_MAP_NO_OVERWRITE = 0x80,
              SONY_MAP_ANON = 0x1000, SONY_MAP_NOCORE = 0x20000, SONY_MAP_NO_COALESCE = 0x400000;
// Returns false (and sets *why) for unknown bits.
bool translate_sony_prot(std::int64_t prot, int* host_prot, std::string* why);
bool translate_sony_map_flags(std::int64_t flags, std::int64_t fd, int* host_flags, std::string* why);
bool translate_sony_clock(std::int64_t id, int* host_clock);
// POSIX-style libkernel mmap: returns the address or -1 (MAP_FAILED).
std::int64_t sony_mmap(std::int64_t addr, std::int64_t len, std::int64_t prot, std::int64_t flags, std::int64_t fd,
                       std::int64_t off);
std::int64_t sony_clock_gettime(std::int64_t id, std::int64_t ts_ptr);

// ---- Entry parameters. Layout in guest memory: u64 argc, u64 argv[argc], u64 0, u64 0 (no envp),
// then the strings. Passed in rdi; rsi receives the exit handler (PNR convention: ends the process, status 0).
struct EntryArgs {
  std::vector<std::uint64_t> words;
  std::vector<char> strings;
};
// Builds a block with real pointers into itself.
std::vector<std::uint8_t> build_entry_args(const std::vector<std::string>& argv);

// ---- Module lifecycle (slice S32): DT_INIT, DT_INIT_ARRAY, DT_FINI_ARRAY, DT_FINI, libc atexit.
// Ordering follows the System V gABI: DT_INIT first, then DT_INIT_ARRAY in file order; on exit the
// atexit handlers run LIFO, then DT_FINI_ARRAY in reverse file order, then DT_FINI. PlayStation's
// exact init-argument convention is UNVERIFIED on hardware; PNR passes the same (args, exit)
// pair the entry point receives.
struct GuestLifecycle {
  std::vector<std::uint64_t> init;  // absolute guest addresses, in call order
  std::vector<std::uint64_t> fini;  // absolute guest addresses, in call order (already reversed)
};

// ---- Process-level guest runtime (call in the forked child only)
// Installs TLS for the calling thread, runs fn(rdi=a, rsi=b), restores FS. Returns rax.
std::int64_t run_guest_main(const TlsTemplate& t, std::int64_t (*fn)(void*, void*), void* a, void* b);
// Lifecycle variant: runs lc.init (in order) before fn, and the exit sequence (atexit handlers
// LIFO, then lc.fini in order) after fn returns, all with the guest FS base.
std::int64_t run_guest_main(const TlsTemplate& t, std::int64_t (*fn)(void*, void*), void* a, void* b,
                            const GuestLifecycle& lc);
// libc atexit binding: registers a guest function pointer for the exit sequence. 0 on success,
// -1 when the table is full (64 entries, a PNR limit), -2 when no guest module is running.
std::int64_t guest_atexit_register(std::uint64_t fn);
// Slice S34: __cxa_atexit / __cxa_thread_atexit_impl bindings. Same returns as above. The C++
// versions pass `arg` to the handler at run time; the thread variant registers on the calling
// thread's list (run LIFO when that thread exits; the main thread's list runs first in the
// process exit sequence, matching glibc's __call_tls_dtors-before-atexit order). The dso
// argument is accepted by the ABI but ignored: PNR does not unload modules individually.
std::int64_t guest_cxa_atexit_register(std::uint64_t fn, std::uint64_t arg);
std::int64_t guest_cxa_thread_atexit_register(std::uint64_t fn, std::uint64_t arg);
// Runs the exit sequence (atexit handlers LIFO, then the module fini list) with the guest FS base,
// then _exit(code). With no active guest module this is a plain _exit(code).
[[noreturn]] void guest_exit_with(int code);
// Calls a guest function from host code on a thread that has a registered guest FS base (for import
// implementations that call back into guest code). Returns fn's rax, or -2 when this thread has no
// registered guest FS base.
std::int64_t call_guest_function(std::uint64_t addr, std::int64_t a, std::int64_t b);
// Used by import thunks: switch to the host FS if the current FS is a registered guest FS.
std::uint64_t guest_to_host();          // returns the guest FS that was active (0 when none)
void host_to_guest(std::uint64_t fs);   // restore after the host call
bool guest_active();

// Guest threads. Handles are opaque.
std::int64_t guest_thread_create(std::int64_t* out_handle, std::int64_t entry, std::int64_t arg);
std::int64_t guest_thread_join(std::int64_t handle, std::int64_t* ret);
std::int64_t guest_mutex_op(int op, std::int64_t* handle);  // 0 init 1 lock 2 unlock 3 destroy
std::int64_t guest_thread_self();

// Kernel sync objects (slice S1). Handles are registry ids, never raw pointers, so a guest passing a
// stale or forged handle gets an error instead of the host dereferencing guest-controlled memory.
// Return 0 on success, a negative error otherwise (-2 bad handle/argument, -3 failed, -4 timed out).
enum : int { kCondInit = 0, kCondWait = 1, kCondSignal = 2, kCondBroadcast = 3, kCondDestroy = 4 };
std::int64_t guest_cond_init(std::int64_t* handle);
// Waits on cond with the guest mutex (the existing guest_mutex_op handle). timeout_us < 0 waits forever.
std::int64_t guest_cond_wait(std::int64_t cond, std::int64_t* mutex_handle, std::int64_t timeout_us);
std::int64_t guest_cond_wake(std::int64_t cond, bool all);
std::int64_t guest_cond_destroy(std::int64_t* handle);
std::int64_t guest_sema_create(std::int64_t* handle, std::int64_t initial, std::int64_t max);
std::int64_t guest_sema_wait(std::int64_t sema, std::int64_t need, std::int64_t timeout_us);  // <0 forever
std::int64_t guest_sema_signal(std::int64_t sema, std::int64_t count);
std::int64_t guest_sema_destroy(std::int64_t* handle);

}  // namespace pnr::loader
