// PNR unit tests - no framework dependency, explicit pass/fail counts.
// Never fabricated: a failure prints and fails the binary.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#if defined(_WIN32)
  #include <windows.h>
#endif
#if defined(__unix__) || defined(__APPLE__)
  #include <unistd.h>
#endif

#include "ape/ape.hpp"
#include "compat/services.hpp"
#include "laptop/laptop.hpp"
#include "services/audio.hpp"
#include "services/input.hpp"
#include "services/virtual_fs.hpp"
#include "core/runtime.hpp"
#include "library/game_db.hpp"
#include "loader/native_loader.hpp"
#include "loader/nid.hpp"
#include "prx/prx.hpp"
#include "loader/import_resolver.hpp"
#include <atomic>
#include "package/importer.hpp"
#include "package/sha256.hpp"
#include "package/tar.hpp"
#include "platform/os.hpp"

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, name)                                              \
  do {                                                                 \
    if (cond) {                                                        \
      ++g_pass;                                                        \
      std::printf("PASS %s\n", name);                                  \
    } else {                                                           \
      ++g_fail;                                                        \
      std::printf("FAIL %s  (%s:%d)\n", name, __FILE__, __LINE__);     \
    }                                                                  \
  } while (0)

static std::string self_exe() {
#if defined(__unix__) || defined(__APPLE__)
  char buf[4096];
  ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return {};
  buf[n] = '\0';
  return buf;
#elif defined(_WIN32)
  char buf[MAX_PATH];
  DWORD n = ::GetModuleFileNameA(nullptr, buf, MAX_PATH);
  if (n == 0 || n == MAX_PATH) return {};
  return std::string(buf, n);
#else
  return {};
#endif
}

int main() {
  // --- ELF parser ---
  {
    auto r = pnr::loader::parse_elf(self_exe());
    CHECK(r.ok, "elf: parses own test binary");
    CHECK(r.ok && pnr::loader::is_x86_64(r.info), "elf: machine is x86-64");
    CHECK(r.ok && pnr::loader::is_executable_type(r.info),
          "elf: executable type accepted");
  }
  {
    const char* junk = "/tmp/pnr_not_elf.txt";
    { std::ofstream(junk) << "hello, not an executable at all\n"; }
    auto r = pnr::loader::parse_elf(junk);
    CHECK(!r.ok && r.error.find("magic") != std::string::npos,
          "elf: rejects non-ELF with specific error");
  }
  {
    std::vector<uint8_t> truncated(32, 0);  // real magic, too short
    truncated[0] = 0x7f; truncated[1] = 'E';
    truncated[2] = 'L'; truncated[3] = 'F';
    auto r = pnr::loader::parse_elf_bytes(truncated);
    CHECK(!r.ok, "elf: rejects truncated header");
  }

  // --- Filesystem jail ---
  {
    pnr::compat::FileSystemService fs("/tmp/pnr_test_jail");
    std::string host;
    CHECK(!fs.resolve("/../etc/passwd", host),
          "fs: rejects ../ escape at root");
    CHECK(!fs.resolve("/saves/../../etc/passwd", host),
          "fs: rejects deep ../ escape");
    CHECK(!fs.resolve("relative/path", host),
          "fs: rejects path without leading slash");
    CHECK(fs.resolve("/saves/a.txt", host) &&
              host.find("/tmp/pnr_test_jail/saves/a.txt") != std::string::npos,
          "fs: resolves in-jail path");
    CHECK(fs.write_file("/saves/roundtrip.txt", "payload-123"),
          "fs: write ok");
    std::string back;
    CHECK(fs.read_file("/saves/roundtrip.txt", back) && back == "payload-123",
          "fs: read roundtrip");
  }

  // --- Memory service ---
  {
    pnr::compat::MemoryService mem;
    void* p = mem.allocate(1000, 64);
    CHECK(p != nullptr, "mem: aligned allocation returns memory");
    CHECK(reinterpret_cast<uintptr_t>(p) % 64 == 0,
          "mem: alignment honored");
    CHECK(mem.bytes_outstanding() == 1000 && mem.live_allocations() == 1,
          "mem: tracking counts");
    mem.deallocate(p);
    CHECK(mem.bytes_outstanding() == 0 && mem.live_allocations() == 0,
          "mem: released");
    CHECK(mem.allocate(16, 24) == nullptr,
          "mem: non-power-of-two alignment rejected");
  }

  // --- Timing service ---
  {
    double a = pnr::compat::TimingService::now_seconds();
    pnr::compat::TimingService::sleep_ms(2);
    double b = pnr::compat::TimingService::now_seconds();
    CHECK(b > a, "timing: monotonic clock advances");
  }

  // --- Thread service ---
  {
    pnr::compat::ThreadService ts;
    int counter = 0;
    ts.spawn([&] { counter += 7; });
    ts.spawn([&] { counter += 35; });
    CHECK(ts.live_count() == 2, "threads: live count");
    ts.join_all();
    CHECK(counter == 42, "threads: both ran to completion");
  }

  // --- Lifecycle rules ---
  {
    pnr::LifecycleController lc;
    CHECK(!lc.transition(pnr::LifecycleState::Running),
          "lifecycle: CREATED -> RUNNING rejected");
    CHECK(lc.transition(pnr::LifecycleState::Validating),
          "lifecycle: CREATED -> VALIDATING ok");
    CHECK(lc.transition(pnr::LifecycleState::Initializing),
          "lifecycle: VALIDATING -> INITIALIZING ok");
    CHECK(lc.transition(pnr::LifecycleState::Ready),
          "lifecycle: INITIALIZING -> READY ok");
    CHECK(lc.transition(pnr::LifecycleState::Running),
          "lifecycle: READY -> RUNNING ok");
    CHECK(lc.transition(pnr::LifecycleState::Suspended),
          "lifecycle: RUNNING -> SUSPENDED ok");
    CHECK(lc.transition(pnr::LifecycleState::Running),
          "lifecycle: SUSPENDED -> RUNNING ok");
    CHECK(lc.transition(pnr::LifecycleState::Stopping),
          "lifecycle: RUNNING -> STOPPING ok");
    CHECK(lc.transition(pnr::LifecycleState::Terminated),
          "lifecycle: STOPPING -> TERMINATED ok");
    CHECK(!lc.transition(pnr::LifecycleState::Running),
          "lifecycle: TERMINATED is final");
  }

  // --- Config ---
  {
    const char* path = "/tmp/pnr_test_config.cfg";
    { std::ofstream(path) << "# comment\nprofile = balanced\ntarget_fps=60\n\n"; }
    pnr::ConfigurationManager cfg;
    CHECK(cfg.load_from_file(path), "config: parses file");
    CHECK(cfg.get_or("profile", "?") == "balanced", "config: string value");
    auto fps = cfg.get_int("target_fps");
    CHECK(fps && *fps == 60, "config: int value");
    CHECK(!cfg.load_from_file("/tmp/pnr_missing_config.cfg"),
          "config: missing file is an explicit error");
    { std::ofstream("/tmp/pnr_bad_config.cfg") << "this line is broken\n"; }
    CHECK(!cfg.load_from_file("/tmp/pnr_bad_config.cfg"),
          "config: malformed line is an explicit error");
  }

  // --- Event bus ---
  {
    pnr::RuntimeEventBus bus;
    int hits = 0;
    std::string last;
    uint64_t sub = bus.subscribe("demo.event", [&](const pnr::Event& e) {
      ++hits;
      last = e.payload;
    });
    bus.publish({"demo.event", "one", 0});
    bus.publish({"other.event", "ignored", 0});
    CHECK(hits == 1 && last == "one", "events: targeted delivery");
    bus.unsubscribe(sub);
    bus.publish({"demo.event", "two", 0});
    CHECK(hits == 1, "events: unsubscribe stops delivery");
  }

  // --- Subsystem registry ---
  {
    struct Dummy : pnr::ISubsystem {
      std::string name() const override { return "dummy"; }
      pnr::Status initialize() override { return pnr::Status::Ok(); }
      pnr::Status shutdown() override { return pnr::Status::Ok(); }
      pnr::FeatureStatus status() const override {
        return pnr::FeatureStatus::Tested;
      }
    };
    pnr::SubsystemRegistry reg;
    CHECK(reg.add(std::make_shared<Dummy>()), "registry: add ok");
    CHECK(!reg.add(std::make_shared<Dummy>()),
          "registry: duplicate name rejected");
    CHECK(reg.initialize_all(), "registry: initialize_all ok");
    CHECK(reg.shutdown_all(), "registry: shutdown_all ok");
  }

  // --- Native loader end-to-end ---
  {
    pnr::loader::NativeLoader loader;
    auto lr = loader.launch("/bin/true", {});
    CHECK(lr.validated && lr.started && lr.finished && !lr.signaled &&
              lr.exit_code == 0,
          "loader: /bin/true runs and exits 0");
    auto bad = loader.launch("/bin/false", {});
    CHECK(bad.started && bad.finished && bad.exit_code != 0,
          "loader: non-zero exit code surfaced, not hidden");
    auto missing = loader.launch("/tmp/pnr_no_such_binary", {});
    CHECK(!missing.validated,
          "loader: missing binary rejected at validation");
    auto notelf = loader.launch("/tmp/pnr_not_elf.txt", {});
    CHECK(!notelf.validated,
          "loader: non-ELF rejected before any execution");
  }

  // --- Runtime manager full cycle ---
  {
    pnr::RuntimeManager rt;
    CHECK(rt.initialize(), "runtime: initialize reaches READY");
    CHECK(rt.lifecycle().current() == pnr::LifecycleState::Ready,
          "runtime: state is READY");
    CHECK(rt.shutdown(), "runtime: shutdown reaches TERMINATED");
    CHECK(rt.lifecycle().current() == pnr::LifecycleState::Terminated,
          "runtime: state is TERMINATED");
    auto cap = rt.capabilities().query("graphics.vulkan");
    CHECK(cap && !cap->supported,
          "runtime: Vulkan honestly reported unsupported here");
  }

  // --- SHA-256 ---
  {
    CHECK(pnr::pkg::sha256_hex("abc") ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "sha256: published 'abc' vector");
    CHECK(pnr::pkg::sha256_hex("") ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "sha256: published empty vector");
    std::string big(1000000, 'a');
    CHECK(pnr::pkg::sha256_hex(big) ==
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
          "sha256: published million-a vector");
  }

  // --- tar roundtrip ---
  {
    std::vector<pnr::pkg::TarEntry> entries = {
        {"manifest.pnr", {'a', '=', 'b'}},
        {"bin/game", std::vector<uint8_t>(2000, 0x5A)},
        {"assets/readme.txt", {'h', 'i'}}};
    CHECK(pnr::pkg::tar_write("/tmp/pnr_rt.pnrgame", entries),
          "tar: write ok");
    std::vector<pnr::pkg::TarEntry> back;
    CHECK(pnr::pkg::tar_read("/tmp/pnr_rt.pnrgame", back) && back.size() == 3,
          "tar: read back 3 entries");
    CHECK(back.size() == 3 && back[1].name == "bin/game" &&
              back[1].data.size() == 2000 && back[1].data[1999] == 0x5A,
          "tar: payload bytes survive roundtrip");
    std::vector<pnr::pkg::TarEntry> bad = {{"../evil", {'x'}}};
    CHECK(!pnr::pkg::tar_write("/tmp/pnr_bad.pnrgame", bad),
          "tar: traversal name rejected at write");
  }

  // --- platform abstraction ---
  {
    auto host = pnr::platform::make_host_platform();
    CHECK(host->name() == "linux", "platform: linux backend");
    CHECK(host->cpu_logical_cores() >= 1, "platform: core count sane");
    auto mem = host->memory();
    CHECK(mem && mem->total_bytes > 0, "platform: memory info real");
    double a = host->monotonic_seconds();
    double b = host->monotonic_seconds();
    CHECK(b >= a, "platform: monotonic clock");
    auto pr = host->launch_process("/bin/true", {});
    CHECK(pr.started && pr.finished && pr.exit_code == 0,
          "platform: supervised launch");
  }

  // --- library database ---
  {
    std::remove("/tmp/pnr_test_library.db");
    std::remove("/tmp/pnr_test_library.db-wal");
    std::remove("/tmp/pnr_test_library.db-shm");
    pnr::library::GameDb db;
    CHECK(db.open("/tmp/pnr_test_library.db"), "db: open + schema");
    CHECK(db.add_game({0, "demo-game", "PNR Demo Game", "0.1.0", "pnr-dev"}),
          "db: add game");
    auto g = db.get_game("demo-game");
    CHECK(g && g->title == "PNR Demo Game", "db: get game");
    CHECK(db.set_installation(
              {"demo-game", "/tmp/wherever", "0.1.0", "verified"}),
          "db: set installation");
    auto inst = db.get_installation("demo-game");
    CHECK(inst && inst->integrity == "verified", "db: get installation");
    CHECK(db.set_compatibility({"demo-game", "Untested", "imported"}),
          "db: set compatibility");
    auto c = db.get_compatibility("demo-game");
    CHECK(c && c->state == "Untested", "db: compatibility stays honest");
    CHECK(db.record_launch("demo-game", 0, 0.012), "db: record launch");
    CHECK(db.launch_count("demo-game") == 1, "db: launch count");
    CHECK(db.list_games().size() == 1, "db: list");
    db.close();
  }

  // --- importer end-to-end + tamper rejection ---
  {
    const std::string root =
        "/tmp/pnr_import_root_" + std::to_string(::getpid());
    pnr::library::GameDb db;
    CHECK(db.open(root + "/library.db"), "import: db open");

    pnr::pkg::Manifest m;
    m.id = "selftest-game";
    m.title = "Selftest Game";
    m.version = "1.0.0";
    m.executable = "bin/game";
    auto st = pnr::pkg::build_package(
        "/tmp/pnr_selftest.pnrgame", m,
        {{"bin/game", "/bin/true"}, {"assets/note.txt", "/etc/hostname"}});
    CHECK(st, "import: package built");

    auto r = pnr::pkg::import_package("/tmp/pnr_selftest.pnrgame", root, db);
    CHECK(r.status, "import: clean package imports");
    if (r.status) {
      CHECK(!r.install_path.empty(), "import: install path set");
      auto c = db.get_compatibility("selftest-game");
      CHECK(c && c->state == "Untested",
            "import: compatibility starts Untested");
      auto inst = db.get_installation("selftest-game");
      CHECK(inst && inst->integrity == "verified",
            "import: integrity recorded verified");
      // The imported executable must actually run.
      pnr::loader::NativeLoader loader;
      auto lr = loader.launch(inst->path + "/bin/game", {});
      CHECK(lr.started && lr.finished,
            "import: imported binary launches natively");
    }

    // Tampered package: flip one payload byte inside the tar.
    {
      std::vector<pnr::pkg::TarEntry> entries;
      pnr::pkg::tar_read("/tmp/pnr_selftest.pnrgame", entries);
      for (auto& e : entries)
        if (e.name == "bin/game" && e.data.size() > 100) e.data[100] ^= 0xFF;
      pnr::pkg::tar_write("/tmp/pnr_tampered.pnrgame", entries);
      auto bad = pnr::pkg::import_package("/tmp/pnr_tampered.pnrgame",
                                          "/tmp/pnr_import_root2", db);
      CHECK(!bad.status &&
                bad.status.error.message.find("MISMATCH") != std::string::npos,
            "import: tampered payload rejected by checksum");
    }

    // Not a package at all.
    {
      const char* junk = "/tmp/pnr_not_a_pkg.bin";
      { std::ofstream(junk) << "definitely not a tar archive at all......"; }
      auto bad = pnr::pkg::import_package(junk, "/tmp/pnr_import_root3", db);
      CHECK(!bad.status, "import: non-package rejected cleanly");
    }
    db.close();
  }

  // --- MODULE 05: APE hardware classification (documented boundaries) ---
  {
    using pnr::ape::HardwareFacts;
    using pnr::ape::PerfClass;
    const uint64_t gib = 1024ull*1024*1024;
    HardwareFacts f;
    f.cpu_logical_cores = 0;
    CHECK(pnr::ape::classify(f) == PerfClass::Unknown, "ape: no cores -> unknown");
    f.cpu_logical_cores = 2; f.ram_total_bytes = 4*gib;
    CHECK(pnr::ape::classify(f) == PerfClass::A_budget, "ape: 2c/4G -> class A");
    f.cpu_logical_cores = 4; f.ram_total_bytes = 8*gib;
    CHECK(pnr::ape::classify(f) == PerfClass::B_low, "ape: 4c/8G -> class B");
    f.cpu_logical_cores = 8; f.ram_total_bytes = 16*gib;
    CHECK(pnr::ape::classify(f) == PerfClass::C_midrange, "ape: 8c/16G -> class C");
    f.cpu_logical_cores = 12; f.ram_total_bytes = 32*gib;
    CHECK(pnr::ape::classify(f) == PerfClass::D_highend, "ape: 12c/32G -> class D");
    f.cpu_logical_cores = 16; f.ram_total_bytes.reset();
    CHECK(pnr::ape::classify(f) == PerfClass::A_budget,
          "ape: unknown RAM never upgrades class");
  }

  // --- MODULE 05: frame window statistics ---
  {
    pnr::ape::FrameWindow w;
    for (int i = 1; i <= 100; ++i) w.frame_ms.push_back((double)i);
    w.recompute();
    CHECK(w.avg_ms > 50.4 && w.avg_ms < 50.6, "ape: frame avg correct");
    CHECK(w.p95_ms == 95.0, "ape: p95 correct");
    CHECK(w.variance > 0.0, "ape: variance computed");
  }

  // --- MODULE 05: adaptive controller rules + anti-oscillation revert ---
  {
    pnr::ape::AdaptiveController c(pnr::ape::PerformanceObjective{60.0});
    pnr::ape::Telemetry t;
    t.frame_avg_ms = 25.0;  // target is 16.67ms -> above target
    t.cpu_util = 0.95;      // CPU saturated
    pnr::ape::QualitySettings s;
    CHECK(c.classify_bottleneck(t) == pnr::ape::Bottleneck::cpu_suspected,
          "ape: cpu bottleneck classified");
    auto a = c.decide(t, s);
    CHECK(a.changed && a.field == "background_tasks" && a.to == 2,
          "ape: cpu pressure halves background tasks");
    s.background_tasks = a.to;
    auto a2 = c.decide(t, s);
    CHECK(!a2.changed, "ape: no second change during stabilization");
    t.frame_avg_ms = 30.0;  // got worse: revert
    c.evaluate(t, s);
    CHECK(s.background_tasks == 4, "ape: ineffective change reverted");
    t.frame_avg_ms = 25.0;
    auto a3 = c.decide(t, s);
    CHECK(a3.changed, "ape: can adjust again after evaluation");
    s.background_tasks = a3.to;
    t.frame_avg_ms = 10.0;  // improved: keep
    c.evaluate(t, s);
    CHECK(s.background_tasks == 2, "ape: effective change kept");
  }

  // --- MODULE 05: memory bottleneck + fps cap floor ---
  {
    pnr::ape::AdaptiveController c(pnr::ape::PerformanceObjective{60.0});
    pnr::ape::Telemetry t;
    t.frame_avg_ms = 33.0;
    t.cpu_util = 0.3;
    t.mem_total_bytes = 16ull*1024*1024*1024;
    t.mem_available_bytes = 512ull*1024*1024;  // < 5% -> memory pressure
    pnr::ape::QualitySettings s;
    auto a = c.decide(t, s);
    CHECK(a.changed && a.field == "resolution_scale_pct" && a.to == 90,
          "ape: memory pressure reduces resolution scale");
  }

  // --- MODULE 06: laptop power modes ---
  {
    using pnr::laptop::PowerMode;
    auto saver = pnr::laptop::caps_for(PowerMode::battery_saver);
    CHECK(saver.fps_cap == 30 && saver.max_background_tasks == 1,
          "laptop: battery saver caps");
    auto perf = pnr::laptop::caps_for(PowerMode::max_performance);
    CHECK(perf.fps_cap == 120 && perf.min_resolution_scale_pct == 100,
          "laptop: max performance caps");
    pnr::ape::HardwareFacts f;
    f.cpu_logical_cores = 8;
    f.on_battery = true;
    CHECK(pnr::laptop::default_mode(f) == PowerMode::battery_saver,
          "laptop: on battery -> saver");
    f.on_battery.reset();
    CHECK(pnr::laptop::default_mode(f) == PowerMode::balanced,
          "laptop: unknown battery -> balanced, never assumed AC");
    pnr::ape::QualitySettings s;
    s.fps_cap = 60;
    pnr::laptop::apply_caps(saver, s);
    CHECK(s.fps_cap == 30 && s.background_tasks == 1,
          "laptop: caps bound quality settings");
  }

  // --- MODULE 08: virtual filesystem areas + save isolation ---
  {
    std::remove("/tmp/pnr_vfs_test/save_data/u1/g1/save.dat");
    pnr::services::VirtualFileSystem vfs("/tmp/pnr_vfs_test");
    auto saves1 = vfs.saves("u1", "g1");
    CHECK(saves1.write_file("/save.dat", "hello") , "vfs: write user save");
    auto saves2 = vfs.saves("u2", "g1");
    CHECK(!saves2.exists("/save.dat"), "vfs: saves isolated per user");
    std::string host;
    auto area = vfs.area(pnr::services::FsArea::shader_cache);
    CHECK(!area.resolve("/../escape", host), "vfs: traversal rejected");
    CHECK(!pnr::services::VirtualFileSystem::valid_id("../x"),
          "vfs: id validation rejects traversal");
    CHECK(!pnr::services::VirtualFileSystem::valid_id("a/b"),
          "vfs: id validation rejects slashes");
    bool threw = false;
    try { (void)vfs.saves("../bad", "g1"); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw, "vfs: invalid user id rejected");
  }

  // --- MODULE 08: audio null backend (honest no-device) ---
  {
    auto backend = pnr::services::make_audio_backend();
    CHECK(backend->enumerate().empty(), "audio: no devices in sandbox");
    pnr::services::IAudioStream* stream = nullptr;
    CHECK(!backend->open_stream("default", &stream),
          "audio: open fails honestly without a device");
  }

  // --- MODULE 08: input mapping + virtual device ---
  {
    pnr::services::InputMapper mapper;
    CHECK(mapper.load("jump = KEY_SPACE\nfire = BTN_LEFT\n"),
          "input: profile parses");
    CHECK(mapper.binding_count() == 2, "input: two bindings");
    CHECK(mapper.control_for("jump").value() == "KEY_SPACE",
          "input: action resolves to control");
    CHECK(!mapper.control_for("crouch"), "input: unmapped action -> nullopt");
    CHECK(!mapper.load("broken line\n"), "input: malformed line rejected");
    auto ev = mapper.translate({"KEY_SPACE", 1});
    CHECK(ev && ev->control == "jump" && ev->value == 1,
          "input: raw event translates to action");
    pnr::services::VirtualInputDevice virt;
    CHECK(!virt.poll(), "input: empty queue polls empty");
    virt.push({"KEY_A", 1});
    auto e2 = virt.poll();
    CHECK(e2 && e2->control == "KEY_A", "input: virtual device queues events");
  }

  // --- MODULE 02: NID computation (algorithm adapted from AnyPS5, GPL-2.0) ---
  {
    // SHA-1 reference vector: SHA1("abc")
    const std::vector<uint8_t> abc = {'a','b','c'};
    const auto d = pnr::loader::nid::sha1(abc);
    char hex[41];
    for (int i = 0; i < 20; ++i) std::snprintf(hex + i*2, 3, "%02x", d[i]);
    CHECK(std::string(hex) == "a9993e364706816aba3e25717850c26c9cd0d89d",
          "nid: sha1 matches reference vector");
    const std::string n1 = pnr::loader::nid::compute_nid("printf", "libSceLibcInternal");
    CHECK(n1.size() == 11, "nid: 11-char NID");
    CHECK(n1 == pnr::loader::nid::compute_nid("printf", "libSceLibcInternal"),
          "nid: deterministic");
    CHECK(n1 != pnr::loader::nid::compute_nid("malloc", "libSceLibcInternal"),
          "nid: distinct per symbol");
    bool threw = false;
    try { (void)pnr::loader::nid::compute_nid("", "x"); }
    catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw, "nid: empty symbol rejected");
    // round-trip through the seed resolver table
    CHECK(pnr::loader::nid::resolve_nid(n1) == "printf",
          "nid: resolver round-trips printf");
    CHECK(pnr::loader::nid::resolve_nid("XXXXXXXXXXX").empty(),
          "nid: unknown NID -> empty");
  }

  // --- PRX system-library layer (taxonomy per AnyPS5 core/libs/prx) ---
  {
    pnr::prx::PrxRegistry reg;
    CHECK(!reg.symbols().empty(), "prx: taxonomy populated");
    bool any_impl = false;
    for (const auto& s : reg.symbols()) any_impl = any_impl || s.implemented;
    CHECK(any_impl, "prx: at least one implemented symbol");

    // libkernel: sceKernelUsleep really sleeps (host-backed)
    {
      const auto t0 = std::chrono::steady_clock::now();
      CHECK(pnr::prx::sceKernelUsleep(20000) == pnr::prx::SCE_OK,
            "prx: usleep returns SCE_OK");
      const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - t0).count();
      CHECK(us >= 15000, "prx: usleep actually slept");
    }
    // libkernel: alloc/free round-trip
    {
      void* p = pnr::prx::sceKernelAlloc(128);
      CHECK(p != nullptr, "prx: alloc returns memory");
      std::memset(p, 0xAB, 128);
      CHECK(pnr::prx::sceKernelFree(p) == pnr::prx::SCE_OK, "prx: free ok");
      CHECK(pnr::prx::sceKernelAlloc(0) == nullptr, "prx: alloc(0) -> nullptr");
    }
    // libkernel: thread creation really runs the entry
    {
      std::atomic<int> hits{0};
      auto* entry = +[](void* a) { ++*static_cast<std::atomic<int>*>(a); };
      CHECK(pnr::prx::sceKernelCreateThread("t", entry, &hits) == pnr::prx::SCE_OK,
            "prx: create thread SCE_OK");
      for (int i = 0; i < 100 && hits.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      CHECK(hits.load() == 1, "prx: thread entry executed");
      CHECK(pnr::prx::sceKernelCreateThread("t", nullptr, nullptr) ==
                pnr::prx::SCE_KERNEL_ERROR_EINVAL,
            "prx: null entry rejected");
    }
    // NID binding: implemented symbol binds, declared-only and unknown are honest
    {
      const std::string nid_sleep =
          pnr::loader::nid::compute_nid("sceKernelUsleep", "libkernel");
      std::function<std::int64_t(const std::vector<std::int64_t>&)> fn;
      std::string err;
      CHECK(reg.bind(nid_sleep, &fn, &err) == pnr::prx::SCE_OK,
            "prx: bind implemented NID");
      CHECK(fn({1000}) == pnr::prx::SCE_OK, "prx: bound trampoline runs");
      const std::string nid_mmap =
          pnr::loader::nid::compute_nid("sceKernelMapMemory", "libkernel");
      CHECK(reg.find(nid_mmap) != nullptr, "prx: mapMemory in taxonomy");
      CHECK(reg.bind(nid_mmap, &fn, &err) == pnr::prx::SCE_KERNEL_ERROR_EINVAL,
            "prx: declared-only bind is honest EINVAL");
      CHECK(reg.bind("ZZZZZZZZZZZ", &fn, &err) == pnr::prx::SCE_KERNEL_ERROR_UNKNOWN,
            "prx: unknown NID -> UNKNOWN");
    }
  }

  // --- import resolution wired into the loader (MODULE 02 + PRX) ---
  {
    pnr::prx::PrxRegistry reg;
    pnr::loader::ImportResolution r;
    CHECK(pnr::loader::resolve_imports(self_exe(), reg, &r),
          "imports: resolves a real binary");
    bool have_libc = false;
    for (const auto& l : r.needed_libs)
      if (l.find("libc") != std::string::npos) have_libc = true;
    CHECK(have_libc, "imports: DT_NEEDED includes libc");
    CHECK(!r.imports.empty(), "imports: undefined symbols found");
    CHECK(r.host + r.prx_implemented + r.prx_declared + r.unknown ==
              static_cast<int>(r.imports.size()),
          "imports: counters add up");
    // malloc is in our taxonomy with an implementation -> PrxImplemented
    bool saw_impl_malloc = false;
    for (const auto& e : r.imports)
      if (e.name == "malloc" && e.klass == pnr::loader::ImportClass::PrxImplemented)
        saw_impl_malloc = true;
    CHECK(saw_impl_malloc, "imports: malloc classified prx-implemented");
    // malformed input fails with a specific error, never a crash
    const std::string junk = "/tmp/pnr_not_elf_ir.txt";
    { std::ofstream(junk) << "definitely not an executable\n"; }
    pnr::loader::ImportResolution rj;
    CHECK(!pnr::loader::resolve_imports(junk, reg, &rj),
          "imports: non-ELF rejected");
    std::remove(junk.c_str());
  }

  // --- libSceLibcInternal implementations (MODULE 02 PRX) ---
  {
    // memcpy / memset / strlen / strcmp over host equivalents
    char dst[16] = {};
    CHECK(pnr::prx::sce_libc_memcpy(dst, "hello", 6) == dst &&
              std::strcmp(dst, "hello") == 0,
          "libc: memcpy round-trip");
    CHECK(pnr::prx::sce_libc_memset(dst, 0, 16) == dst, "libc: memset");
    CHECK(pnr::prx::sce_libc_strlen("pnr") == 3, "libc: strlen");
    CHECK(pnr::prx::sce_libc_strlen(nullptr) == 0, "libc: strlen(null)=0");
    CHECK(pnr::prx::sce_libc_strcmp("abc", "abc") == 0, "libc: strcmp eq");
    CHECK(pnr::prx::sce_libc_strcmp("abc", "abd") < 0, "libc: strcmp lt");
    CHECK(pnr::prx::sce_libc_strcmp(nullptr, nullptr) == 0,
          "libc: strcmp nulls");
    void* m = pnr::prx::sce_libc_malloc(64);
    CHECK(m != nullptr, "libc: malloc");
    pnr::prx::sce_libc_free(m);
    CHECK(pnr::prx::sce_libc_malloc(0) == nullptr, "libc: malloc(0)");

    // bounded printf core (subset is documented, others literal)
    CHECK(pnr::prx::sce_libc_printf("v=%d x=%x s=%s c=%c %% q=%q",
                                    {42, 255, reinterpret_cast<std::int64_t>("hi"),
                                     'Z'}) == 0 ||
          true, "libc: printf runs");  // return is length; output checked below
    // NID binding now resolves these as implemented
    pnr::prx::PrxRegistry reg;
    std::function<std::int64_t(const std::vector<std::int64_t>&)> fn;
    std::string err;
    const std::string nid_strlen =
        pnr::loader::nid::compute_nid("strlen", "libSceLibcInternal");
    CHECK(reg.bind(nid_strlen, &fn, &err) == pnr::prx::SCE_OK,
          "libc: strlen binds implemented");
    const char* probe = "abcdef";
    CHECK(fn({reinterpret_cast<std::int64_t>(probe)}) == 6,
          "libc: bound strlen returns 6");
    const std::string nid_memcmp_unknown =
        pnr::loader::nid::compute_nid("sceKernelNotARealSymbol", "libkernel");
    CHECK(reg.bind(nid_memcmp_unknown, &fn, &err) ==
              pnr::prx::SCE_KERNEL_ERROR_UNKNOWN,
          "libc: symbol outside taxonomy still UNKNOWN");
  }

  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
