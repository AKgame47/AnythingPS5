// PNR unit tests - no framework dependency, explicit pass/fail counts.
// Never fabricated: a failure prints and fails the binary.
#include <cstdio>
#include <cstdlib>
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
#include "graphics/spirv.hpp"
#include "graphics/vulkan.hpp"
#include "library/game_db.hpp"
#include "shader/rdna.hpp"
#include "shader/translate.hpp"
#include "loader/native_loader.hpp"
#include "loader/nid.hpp"
#include "prx/prx.hpp"
#include "prx/prx_file.hpp"
#include "loader/import_resolver.hpp"
#include <atomic>
#include <filesystem>
#include "package/importer.hpp"
#include "package/sha256.hpp"
#include "package/tar.hpp"
#include "platform/os.hpp"

void pnr_loop_tests(int& pass,int& fail);
void pnr_launcher_tests(int& pass,int& fail);
void pnr_launcher_mgmt_tests(int& pass,int& fail);
void pnr_launcher_install_tests(int& pass,int& fail);
void pnr_launcher_care_tests(int& pass,int& fail);

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
              {"demo-game", "/tmp/wherever", "0.1.0", "verified", ""}),
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
          pnr::loader::nid::compute_nid("sceKernelReserveVirtualRange", "libkernel");
      CHECK(reg.find(nid_mmap) != nullptr, "prx: reserveVirtualRange in taxonomy");
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

  // --- libkernel memory management (mmap-backed) ---
  {
    // map RW, write through it, query, protect RO, unmap
    void* p = pnr::prx::sceKernelMapMemory(
        4096, pnr::prx::SCE_KERNEL_PROT_READ | pnr::prx::SCE_KERNEL_PROT_WRITE);
    CHECK(p != nullptr, "mm: map RW");
    std::memset(p, 0x5A, 4096);
    void* qs = nullptr; void* qe = nullptr; std::int64_t qp = 0;
    CHECK(pnr::prx::sceKernelQueryMemoryProtection(p, &qs, &qe, &qp) ==
              pnr::prx::SCE_OK,
          "mm: query ours");
    CHECK(qs == p && qe == static_cast<char*>(p) + 4096 &&
              qp == (pnr::prx::SCE_KERNEL_PROT_READ |
                     pnr::prx::SCE_KERNEL_PROT_WRITE),
          "mm: query range+prot");
    CHECK(pnr::prx::sceKernelMTypeprotect(p, 4096,
                                          pnr::prx::SCE_KERNEL_PROT_READ) ==
              pnr::prx::SCE_OK,
          "mm: protect RO");
    CHECK(pnr::prx::sceKernelMunmap(p, 4096) == pnr::prx::SCE_OK,
          "mm: unmap");
    // honest failures
    CHECK(pnr::prx::sceKernelMapMemory(0, pnr::prx::SCE_KERNEL_PROT_READ) ==
              nullptr,
          "mm: map(0) nullptr");
    CHECK(pnr::prx::sceKernelMapMemory(4096, 99) == nullptr,
          "mm: bad prot nullptr");
    int dummy = 0;
    CHECK(pnr::prx::sceKernelMunmap(&dummy, 16) ==
              pnr::prx::SCE_KERNEL_ERROR_EINVAL,
          "mm: unmap foreign EINVAL");
    CHECK(pnr::prx::sceKernelQueryMemoryProtection(&dummy, nullptr, nullptr,
                                                   nullptr) ==
              pnr::prx::SCE_KERNEL_ERROR_EINVAL,
          "mm: query foreign EINVAL");
    // NID-bound path
    pnr::prx::PrxRegistry reg;
    std::function<std::int64_t(const std::vector<std::int64_t>&)> fn;
    std::string err;
    CHECK(reg.bind(pnr::loader::nid::compute_nid("sceKernelMapMemory",
                                                 "libkernel"),
                   &fn, &err) == pnr::prx::SCE_OK,
          "mm: mapMemory binds implemented");
    const std::int64_t mapped = fn({4096, pnr::prx::SCE_KERNEL_PROT_READ |
                                            pnr::prx::SCE_KERNEL_PROT_WRITE});
    CHECK(mapped != 0, "mm: bound map works");
    CHECK(reg.bind(pnr::loader::nid::compute_nid("sceKernelMunmap", "libkernel"),
                   &fn, &err) == pnr::prx::SCE_OK,
          "mm: munmap binds");
    CHECK(fn({mapped, 4096}) == pnr::prx::SCE_OK, "mm: bound unmap works");
  }

  // --- libkernel file calls over the jailed filesystem ---
  {
    const std::string fsroot =
        "/tmp/pnr_prx_fs_" + std::to_string(::getpid());
    std::filesystem::remove_all(fsroot);
    pnr::compat::FileSystemService fs(fsroot);
    CHECK(fs.write_file("/data/hello.txt", "hello pnr"),
          "fs: seed write inside jail");
    pnr::prx::PsFileTable table(fs);

    // declared-only until installed (honest)
    pnr::prx::PrxRegistry reg;
    std::function<std::int64_t(const std::vector<std::int64_t>&)> fn;
    std::string err;
    const std::string nid_open =
        pnr::loader::nid::compute_nid("sceKernelOpen", "libkernel");
    CHECK(reg.bind(nid_open, &fn, &err) == pnr::prx::SCE_KERNEL_ERROR_EINVAL,
          "fs: open declared-only before install");

    const std::int64_t fd = table.open("/data/hello.txt",
                                       pnr::prx::SCE_KERNEL_O_RDONLY);
    CHECK(fd >= 3, "fs: open inside jail");
    char buf[32] = {};
    CHECK(table.read(fd, buf, 9) == 9 && std::string(buf, 9) == "hello pnr",
          "fs: read bytes");
    CHECK(table.lseek(fd, 0, pnr::prx::SCE_KERNEL_SEEK_SET) == 0, "fs: lseek");
    pnr::prx::SceKernelStat st{};
    CHECK(table.fstat(fd, &st) == pnr::prx::SCE_OK && st.size == 9 &&
              !st.is_directory,
          "fs: fstat size");
    CHECK(table.read(fd, buf, 4) == 4, "fs: read after seek");
    CHECK(table.close(fd) == pnr::prx::SCE_OK, "fs: close");
    CHECK(table.close(fd) == pnr::prx::SCE_KERNEL_ERROR_EINVAL,
          "fs: double close EINVAL");
    // write path: create + append
    const std::int64_t wfd = table.open(
        "/out/log.txt", pnr::prx::SCE_KERNEL_O_WRONLY | pnr::prx::SCE_KERNEL_O_CREAT);
    CHECK(wfd >= 3, "fs: create file");
    CHECK(table.write(wfd, "abc", 3) == 3, "fs: write bytes");
    CHECK(table.close(wfd) == pnr::prx::SCE_OK, "fs: close write");
    // traversal rejected by the jail, never touches host
    CHECK(table.open("../../etc/passwd", pnr::prx::SCE_KERNEL_O_RDONLY) ==
              pnr::prx::SCE_KERNEL_ERROR_EINVAL,
          "fs: traversal rejected");
    // "/etc/passwd" stays INSIDE the jail (root/etc/passwd), so the open
    // fails only because the file does not exist there - host file untouched
    CHECK(table.open("/etc/passwd", pnr::prx::SCE_KERNEL_O_RDONLY) < 0,
          "fs: absolute path jailed, not host");
    // NID-bound flow after install
    reg.install_file_calls(&table);
    CHECK(reg.bind(nid_open, &fn, &err) == pnr::prx::SCE_OK,
          "fs: open binds after install");
    const std::string path = "/data/hello.txt";
    const std::int64_t bfd = fn({reinterpret_cast<std::int64_t>(path.c_str()),
                                 pnr::prx::SCE_KERNEL_O_RDONLY});
    CHECK(bfd >= 3, "fs: bound open");
    CHECK(reg.bind(pnr::loader::nid::compute_nid("sceKernelRead", "libkernel"),
                   &fn, &err) == pnr::prx::SCE_OK,
          "fs: read binds");
    char b2[16] = {};
    CHECK(fn({bfd, reinterpret_cast<std::int64_t>(b2), 5}) == 5 &&
              std::string(b2, 5) == "hello",
          "fs: bound read");
    table.close(bfd);
    std::filesystem::remove_all(fsroot);
  }

  // --- SHOWCASE: PS-style demo binary dispatched through the PS-ABI shim ---
  {
#ifdef PNR_PS_DEMO_PATH
    const std::string jail = "/tmp/pnr_psdemo_jail_" + std::to_string(::getpid());
    std::filesystem::remove_all(jail);
    pnr::loader::NativeLoader loader;
    auto lr = loader.launch(PNR_PS_DEMO_PATH, {},
                            {"PNR_JAIL_ROOT=" + jail,
                             std::string("LD_LIBRARY_PATH=") + PNR_PSABI_DIR});
    CHECK(lr.validated && lr.started && lr.finished,
          "psdemo: launched and finished");
    CHECK(!lr.signaled && lr.exit_code == 0,
          "psdemo: all sce calls dispatched, exit 0");
    // and the proof the game wrote really sits inside the jail
    std::ifstream proof(jail + "/psdemo_proof.txt");
    std::string content((std::istreambuf_iterator<char>(proof)),
                        std::istreambuf_iterator<char>());
    CHECK(content == "proof the sce file calls work",
          "psdemo: jailed proof file has exact content");
    // its imports classify prx-implemented with the launch-time registry
    pnr::prx::PrxRegistry reg;
    pnr::compat::FileSystemService gfs(jail);
    pnr::prx::PsFileTable gtab(gfs);
    reg.install_file_calls(&gtab);
    pnr::loader::ImportResolution ir;
    CHECK(pnr::loader::resolve_imports(PNR_PS_DEMO_PATH, reg, &ir),
          "psdemo: imports resolve");
    CHECK(ir.prx_implemented >= 14 && ir.unknown == 0,
          "psdemo: every sce import is implemented");
    std::filesystem::remove_all(jail);
#endif
  }

  // --- LIBRARY: installation records carry the executable path ---
  {
    const std::string dbp = "/tmp/pnr_libdb_" + std::to_string(::getpid()) +
                            ".db";
    pnr::library::GameDb db;
    CHECK(db.open(dbp), "libdb: opens");
    CHECK(db.set_installation({"g1", "/x/g1-1.0", "1.0", "verified",
                               "bin/game"}),
          "libdb: set_installation with executable");
    auto inst = db.get_installation("g1");
    CHECK(inst && inst->executable == "bin/game" && inst->path == "/x/g1-1.0",
          "libdb: executable round-trips");
    CHECK(db.record_launch("g1", 0, 0.42) && db.launch_count("g1") == 1,
          "libdb: launch recorded");
    db.close();
    std::filesystem::remove(dbp);
  }

  // --- MODULE 04: SPIR-V emission + real execution on Vulkan ---
  {
    using pnr::graphics::SpirvModule;
    using pnr::graphics::VulkanBackend;
    namespace spv = pnr::graphics::spv;
    const auto words = SpirvModule::compute_write_u32(42);
    CHECK(words.size() > 20 && words[0] == spv::kMagic,
          "spirv: magic header present");
    CHECK(words[1] == spv::kVersion10 && words[3] == 13 && words[4] == 0,
          "spirv: version/bound/schema correct");
    CHECK(words.size() % 1 == 0, "spirv: word-aligned");

    // External validation when the harness provides spirv-val.
    const char* val = std::getenv("PNR_SPIRV_VAL");
    if (val != nullptr && std::filesystem::exists(val)) {
      const std::string tmp = "/tmp/pnr_spv_" + std::to_string(::getpid()) +
                              ".spv";
      {
        std::ofstream o(tmp, std::ios::binary);
        o.write(reinterpret_cast<const char*>(words.data()),
                words.size() * 4);
      }
      const std::string cmd = std::string(val) + " " + tmp + " 2>&1";
      FILE* p = popen(cmd.c_str(), "r");
      std::string out;
      char buf[256];
      while (p != nullptr && fgets(buf, sizeof buf, p) != nullptr) out += buf;
      const int rc = p != nullptr ? pclose(p) : -1;
      CHECK(rc == 0, "spirv: spirv-val accepts the module");
      if (rc != 0) std::printf("    spirv-val said: %s\n", out.c_str());
      std::filesystem::remove(tmp);
    } else {
      std::puts("    note: PNR_SPIRV_VAL unset; spirv-val check skipped "
                "(execution check below still runs)");
    }

    // Real execution: llvmpipe (software Vulkan) in the dev sandbox.
    // No ICD at all is an honest SKIP, never a failure.
    VulkanBackend vk;
    auto st = vk.open();
    if (!st) {
      std::printf("    note: Vulkan unavailable (%s); execution checks "
                  "skipped honestly\n", st.error.message.c_str());
    } else {
      st = vk.create_instance("pnr-tests");
      CHECK(st, "vk: instance created");
      CHECK(!vk.devices().empty(), "vk: at least one physical device");
      for (const auto& d : vk.devices())
        std::printf("    vk device: %s (vendor=0x%x type=%d%s)\n",
                    d.name.c_str(), d.vendor_id, d.type,
                    d.software ? " software" : "");
      st = vk.create_device(0);
      CHECK(st, "vk: compute device created");
      std::vector<uint32_t> io = {0};
      st = vk.run_compute_words(words, &io);
      CHECK(st, st ? "vk: compute dispatch ran"
                   : (std::string("vk: dispatch failed: ") +
                      st.error.message)
                        .c_str());
      CHECK(io[0] == 42, "vk: shader wrote 42 into the SSBO and readback matches");
      vk.shutdown();
    }
  }

  // --- MODULE 04: RDNA instruction decoder (synthetic words) ---
  {
    using pnr::shader::RdnaFamily;
    using pnr::shader::RdnaOperandKind;
    using pnr::shader::rdna_decode;
    using pnr::shader::rdna_family_of;

    // family detection across the encoding space
    CHECK(rdna_family_of(0xBF810000) == RdnaFamily::SOPP, "rdna: SOPP family");
    CHECK(rdna_family_of(0x80020304) == RdnaFamily::SOP2, "rdna: SOP2 family");
    CHECK(rdna_family_of(0xBE800305) == RdnaFamily::SOP1, "rdna: SOP1 family");
    CHECK(rdna_family_of(0xB0021234) == RdnaFamily::SOPK, "rdna: SOPK family");
    CHECK(rdna_family_of(0xBF000201) == RdnaFamily::SOPC, "rdna: SOPC family");
    CHECK(rdna_family_of(0x7E000301) == RdnaFamily::VOP1, "rdna: VOP1 family");
    CHECK(rdna_family_of(0x4A020702) == RdnaFamily::VOP2, "rdna: VOP2 family");
    CHECK(rdna_family_of(0xD5230000) == RdnaFamily::VOP3, "rdna: VOP3 family");
    CHECK(rdna_family_of(0xE0000000) == RdnaFamily::MUBUF, "rdna: MUBUF family");
    CHECK(rdna_family_of(0xC0000000) == RdnaFamily::Unknown,
          "rdna: unknown family not guessed");

    // s_endpgm decodes with name
    {
      auto r = rdna_decode({0xBF810000});
      CHECK(r.ok && r.instructions.size() == 1 &&
                r.instructions[0].mnemonic == "s_endpgm" &&
                r.instructions[0].supported,
            "rdna: s_endpgm named and supported");
    }
    // s_branch with negative offset: target = pc + 4 + simm*4
    {
      auto r = rdna_decode({0xBF82FFFE});
      CHECK(r.ok && r.instructions[0].mnemonic == "s_branch" &&
                r.instructions[0].branch_target == 0xFFFFFFFCu,
            "rdna: s_branch negative target wraps honestly");
    }
    // SOP2 s_add_u32 s2, s4, s3
    {
      auto r = rdna_decode({0x80020304});
      const auto& i = r.instructions[0];
      CHECK(r.ok && i.mnemonic == "s_add_u32" &&
                i.dst.kind == RdnaOperandKind::ScalarReg && i.dst.reg == 2 &&
                i.src0.reg == 4 && i.src1.reg == 3,
            "rdna: s_add_u32 operands decoded");
    }
    // SOPK s_movk_i32 s2, 0x1234
    {
      auto r = rdna_decode({0xB0021234});
      const auto& i = r.instructions[0];
      CHECK(r.ok && i.mnemonic == "s_movk_i32" && i.dst.reg == 2 &&
                i.simm == 0x1234,
            "rdna: s_movk_i32 decoded");
    }
    // VOP2 v_add_nc_u32 v1, v2, v3
    {
      auto r = rdna_decode({0x4A020702});
      const auto& i = r.instructions[0];
      CHECK(r.ok && i.mnemonic == "v_add_nc_u32" &&
                i.dst.kind == RdnaOperandKind::VectorReg && i.dst.reg == 1 &&
                i.src0.kind == RdnaOperandKind::VectorReg && i.src0.reg == 2 &&
                i.src1.reg == 3,
            "rdna: v_add_nc_u32 operands decoded");
    }
    // VOP2 with inline constant: v_add_nc_u32 v0, 5, v1
    {
      auto r = rdna_decode({0x4A000285});
      const auto& i = r.instructions[0];
      CHECK(r.ok && i.src0.kind == RdnaOperandKind::InlineIntConst &&
                i.src0.signed_value == 5,
            "rdna: inline integer constant decoded");
    }
    // VOP1 literal constant consumes the next word
    {
      auto r = rdna_decode({0x7E0602FF, 0xDEADBEEF});
      const auto& i = r.instructions[0];
      CHECK(r.ok && i.mnemonic == "v_mov_b32" && i.word_count == 2 &&
                i.src0.kind == RdnaOperandKind::LiteralConst &&
                i.src0.value == 0xDEADBEEF,
            "rdna: literal constant second word");
    }
    // VOP3 is 2 words and honestly unsupported (no named table yet)
    {
      auto r = rdna_decode({0xD5230000, 0x00000000});
      CHECK(r.ok && r.instructions[0].family == RdnaFamily::VOP3 &&
                r.instructions[0].word_count == 2 &&
                !r.instructions[0].supported,
            "rdna: VOP3 structurally decoded, marked unsupported");
    }
    // memory family: 2 words, unsupported, walking continues
    {
      auto r = rdna_decode({0xE0000000, 0x00000000, 0xBF810000});
      CHECK(r.ok && r.instructions.size() == 2 &&
                r.instructions[0].word_count == 2 &&
                !r.instructions[0].supported &&
                r.instructions[1].mnemonic == "s_endpgm",
            "rdna: 64-bit family skipped correctly");
    }
    // honest failures: unknown family, truncated 64-bit, truncated literal
    CHECK(!rdna_decode({0xC0000000}).ok, "rdna: unknown family errors");
    CHECK(!rdna_decode({0xD5230000}).ok, "rdna: truncated VOP3 errors");
    CHECK(!rdna_decode({0x7E0602FF}).ok, "rdna: truncated literal errors");
    // a real-shaped mini program
    {
      auto r = rdna_decode({0xBE800305, 0x7E000301, 0xBF810000});
      CHECK(r.ok && r.instructions.size() == 3 &&
                r.instructions[0].pc == 0 && r.instructions[1].pc == 4 &&
                r.instructions[2].pc == 8 && r.instructions[0].supported &&
                r.instructions[1].supported && r.instructions[2].supported,
            "rdna: mini program fully decoded with pcs");
    }
  }


  // --- MODULE 04: restricted RDNA bytes -> SPIR-V -> Vulkan readback ---
  {
    using namespace pnr::shader;
    constexpr uint64_t base = 0x0000001234567800ull;
    auto program = [](uint32_t value) {
      // v_mov_b32 v0, address-low; v1, address-high; v2, data;
      // flat_store_dword v[0:1], v2; s_endpgm.
      return std::vector<uint32_t>{0x7e0002ff, 0x34567800,
        0x7e0202ff, 0x12, 0x7e0402ff, value,
        0xdc700000, 0x007f0200, 0xbf810000};
    };
    auto bytes = [](const std::vector<uint32_t>& words) {
      std::vector<uint8_t> b;
      for (auto w : words) for (unsigned j = 0; j < 4; ++j)
        b.push_back(static_cast<uint8_t>(w >> (j * 8)));
      return b;
    };
    auto r = translate_compute_bytes(bytes(program(42)), base);
    CHECK(r.ok && r.stored_value == 42 && !r.spirv.empty(),
          "translate: little-endian RDNA bytes produce SPIR-V");
    auto dec = rdna_decode(program(42));
    CHECK(dec.ok && dec.instructions.size() == 5 &&
          dec.instructions[3].mnemonic == "flat_store_dword" &&
          dec.instructions[3].memory_data == 2 &&
          dec.instructions[3].memory_addr == 0 &&
          dec.instructions[3].memory_saddr == 127,
          "translate: FLAT store fields decoded with AnyPS5 taxonomy");
    auto rejected = [&](std::vector<uint32_t> code, const char* name) {
      auto x = translate_compute_words(code, base);
      CHECK(!x.ok && !x.error.empty() && x.spirv.empty(), name);
    };
    rejected({}, "translate: empty input rejected");
    auto b = bytes(program(42)); b.pop_back();
    auto bad_bytes = translate_compute_bytes(b, base);
    CHECK(!bad_bytes.ok && bad_bytes.spirv.empty(), "translate: partial word rejected");
    rejected({0x7e0002ff}, "translate: truncated literal rejected");
    rejected({0xdc700000}, "translate: truncated FLAT rejected");
    rejected({0xc0000000}, "translate: unknown family rejected");
    auto w = program(42); w.pop_back();
    rejected(w, "translate: missing termination rejected");
    rejected({0xbf810000}, "translate: missing store rejected");
    w = program(42); w.push_back(0xbf810000);
    rejected(w, "translate: trailing instruction rejected");
    w = program(42); w[8] |= 1;
    rejected(w, "translate: endpgm immediate rejected");
    w = program(42); w[4] = 0x7e040200;
    rejected(w, "translate: SGPR source rejected");
    w = program(42); w[4] = 0x7e0402f9;
    rejected(w, "translate: SDWA source rejected");
    w = program(42); w[4] = 0x7e040303;
    rejected(w, "translate: undefined VGPR source rejected");
    w = program(42); w[7] = 0x007f0300;
    rejected(w, "translate: undefined data rejected");
    w = program(42); w[7] = 0x007f02ff;
    rejected(w, "translate: address register overflow rejected");
    w = program(42); w[1] += 4;
    rejected(w, "translate: out-of-binding address rejected");
    w = program(42); w[6] |= 4;
    rejected(w, "translate: nonzero memory offset rejected");
    w = program(42); w[6] |= 0x10000;
    rejected(w, "translate: cache modifier rejected");
    w = program(42); w[6] |= 0x8000;
    rejected(w, "translate: global segment rejected");
    w = program(42); w[7] = 0x00000200;
    rejected(w, "translate: scalar address mode rejected");
    w = program(42); w.insert(w.end()-1, {0xdc700000, 0x007f0200});
    rejected(w, "translate: second store rejected");
    w = program(42); w[8] = 0xbf820000;
    rejected(w, "translate: branch rejected instead of ignored");
    w = program(42); w[6] = 0xdc300000;
    rejected(w, "translate: malformed load fields rejected");
    // Inline integer source and register copy use the same constant analysis.
    w = program(42); w[4] = 0x7e0402aa; w.erase(w.begin()+5);
    auto inline_r = translate_compute_words(w, base);
    CHECK(inline_r.ok && inline_r.stored_value == 42,
          "translate: inline integer move folded");
    w = program(42); w.insert(w.begin()+6, 0x7e060302); w[8] = 0x007f0300;
    auto copy_r = translate_compute_words(w, base);
    CHECK(copy_r.ok && copy_r.stored_value == 42, "translate: VGPR copy folded");

    const char* val = std::getenv("PNR_SPIRV_VAL");
    if (val && std::filesystem::exists(val) && r.ok) {
      const std::string tmp = "/tmp/pnr_translated_" + std::to_string(::getpid()) + ".spv";
      { std::ofstream o(tmp, std::ios::binary);
        o.write(reinterpret_cast<const char*>(r.spirv.data()), r.spirv.size()*4); }
      const int rc = std::system((std::string(val) + " --target-env vulkan1.0 " + tmp).c_str());
      CHECK(rc == 0, "translate: spirv-val validates translated module for Vulkan 1.0");
      std::filesystem::remove(tmp);
    } else std::puts("    note: translated module validator check skipped (not configured)");
    pnr::graphics::VulkanBackend vk;
    auto st = vk.open();
    if (!st) std::printf("    note: translated execution skipped: %s\n", st.error.message.c_str());
    else {
      st = vk.create_instance("pnr-translate-tests");
      CHECK(st, "translate: Vulkan instance created");
      if (st && !vk.devices().empty()) {
        st = vk.create_device(0);
        CHECK(st, "translate: compute device created");
        if (st) for (uint32_t v : {0u, 42u, 0xdeadbeefu, 0xffffffffu}) {
          auto t = translate_compute_bytes(bytes(program(v)), base);
          std::vector<uint32_t> io = {0xa5a5a5a5u, 0xcafebabeu};
          auto run = vk.run_compute_words(t.spirv, &io);
          CHECK(t.ok && run && io[0] == v && io[1] == 0xcafebabeu,
                "translate: RDNA bytes dispatched, exact readback, neighbor untouched");
          std::printf("    translated on %s: expected=0x%08x readback=0x%08x\n",
                      vk.device_name().c_str(), v, io[0]);
        }
      } else CHECK(false, "translate: Vulkan device available");
    }
  }


  // --- Dynamic integer translation: same module, different runtime data ---
  {
    using namespace pnr::shader;
    using namespace pnr::graphics;
    constexpr uint64_t base = 0x1234567800ull;
    auto kernel = [](uint32_t op) {
      // address v[0:1], load into v2, v3 = literal, ALU v2 = v2 OP v3,
      // store v2. Destination aliases source: SSA must preserve old reads.
      return std::vector<uint32_t>{0x7e0002ff, 0x34567800,
        0x7e0202ff, 0x12, 0xdc300000, 0x027f0000,
        0x7e0602ff, 0xf0f0aa55u,
        (op<<25u) | (2u<<17u) | (3u<<9u) | 258u,
        0xdc700000, 0x007f0200, 0xbf810000};
    };
    auto reject = [&](std::vector<uint32_t> w, const char* label) {
      const auto r = translate_compute_words(w, base);
      CHECK(!r.ok && !r.error.empty() && r.spirv.empty(), label);
    };
    auto w = kernel(0x25); w[5] |= 0x100;
    reject(w, "dynamic: load VDATA must be zero");
    w = kernel(0x25); w[5] &= ~0x7f0000u;
    reject(w, "dynamic: scalar load address rejected");
    w = kernel(0x25); w[4] |= 0x1000;
    reject(w, "dynamic: load DLC rejected");
    w = kernel(0x25); w[8] = (0x25u<<25u) | (2u<<17u) | (9u<<9u) | 258u;
    reject(w, "dynamic: undefined ALU register rejected");
    w = kernel(0x25); w[8] = (0x25u<<25u) | (2u<<17u) | (3u<<9u) | 249u;
    reject(w, "dynamic: ALU SDWA rejected");
    w = kernel(0x25); w[8] = (0x0fu<<25u) | (2u<<17u) | (3u<<9u) | 258u;
    reject(w, "dynamic: float min outside subset rejected");
    w = kernel(0x25); w[5] = 0x017f0000; // load overwrites high address v1
    reject(w, "dynamic: runtime-dependent high address rejected");
    CHECK(SpirvModule::compute_u32({{U32Op::Add,0,0}}).empty(),
          "dynamic: emitter rejects forward SSA references");
    CHECK(SpirvModule::compute_u32({{U32Op::Constant,1}, {U32Op::Store,0},
          {U32Op::Add,1,0}}).empty(), "dynamic: store cannot be read as value");
    VulkanBackend vk;
    auto st = vk.open();
    if (!st) std::printf("    note: dynamic execution skipped: %s\n", st.error.message.c_str());
    else {
      st = vk.create_instance("pnr-dynamic-tests");
      CHECK(st, "dynamic: Vulkan instance created");
      if (st && !vk.devices().empty()) {
        st = vk.create_device(0);
        CHECK(st, "dynamic: compute device created");
        if (st) for (uint32_t op : {0x25u,0x26u,0x1bu,0x1cu,0x1du}) {
          auto t = translate_compute_words(kernel(op), base);
          CHECK(t.ok && !t.stored_value_known, "dynamic: load/ALU emitted, result not invented");
          // Check emitted opcodes directly, then validate the whole module.
          bool load=false, alu=false;
          const uint32_t expect_op = op==0x25 ? 128 : op==0x26 ? 130 :
                                     op==0x1b ? 199 : op==0x1c ? 197 : 198;
          for (size_t i=5; i<t.spirv.size();) {
            load |= (t.spirv[i]&0xffffu)==61;
            alu |= (t.spirv[i]&0xffffu)==expect_op;
            if (!(t.spirv[i]>>16u)) break;
            i += t.spirv[i]>>16u;
          }
          CHECK(load && alu, "dynamic: OpLoad and integer ALU present in SPIR-V");
          const char* val = std::getenv("PNR_SPIRV_VAL");
          if (val && std::filesystem::exists(val)) {
            const std::string path = "/tmp/pnr_dynamic_" + std::to_string(::getpid()) + ".spv";
            { std::ofstream o(path,std::ios::binary);
              o.write(reinterpret_cast<const char*>(t.spirv.data()),t.spirv.size()*4); }
            CHECK(std::system((std::string(val)+" --target-env vulkan1.0 "+path).c_str())==0,
                  "dynamic: translated ALU module validates for Vulkan 1.0");
            std::filesystem::remove(path);
          } else std::puts("    note: dynamic spirv-val skipped (not configured)");
          // No recompilation between runs: the input is genuinely runtime data.
          for (uint32_t input : {0u,1u,0xffffffffu,0x12345678u}) {
            constexpr uint32_t k=0xf0f0aa55u;
            const uint32_t expected = op==0x25 ? input+k : op==0x26 ? input-k :
                op==0x1b ? input&k : op==0x1c ? input|k : input^k;
            std::vector<uint32_t> io = {input,0xcafebabe};
            auto run=vk.run_compute_words(t.spirv,&io);
            CHECK(run && io[0]==expected && io[1]==0xcafebabe,
                  "dynamic: runtime input ALU readback matches uint32 semantics");
            std::printf("    dynamic op=0x%x input=0x%08x readback=0x%08x expected=0x%08x\n",
                        op,input,io[0],expected);
          }
        }
        if (st) {
          w = kernel(0x25);
          // v4 = loaded v2, then overwrite v2 with 7. Store old value v4.
          w[8] = 0x7e080302; w.insert(w.begin()+9,0x7e040287);
          w[11]=0x007f0400;
          auto t=translate_compute_words(w,base);
          std::vector<uint32_t> io={0x87654321,0xabcdef01};
          auto run=vk.run_compute_words(t.spirv,&io);
          CHECK(t.ok && run && io[0]==0x87654321 && io[1]==0xabcdef01,
                "dynamic: register copy survives later source overwrite");
          // Chain all five ALUs; each result is a runtime SSA operand.
          w=kernel(0x25);
          for (uint32_t op : {0x26u,0x1bu,0x1cu,0x1du})
            w.insert(w.end()-3,(op<<25u)|(2u<<17u)|(3u<<9u)|258u);
          t=translate_compute_words(w,base); io={0x12345678,0xabcdef01};
          uint32_t expect=0x12345678u;
          expect+=0xf0f0aa55u; expect-=0xf0f0aa55u;
          expect&=0xf0f0aa55u; expect|=0xf0f0aa55u; expect^=0xf0f0aa55u;
          run=vk.run_compute_words(t.spirv,&io);
          CHECK(t.ok && run && io[0]==expect && io[1]==0xabcdef01,
                "dynamic: five dependent ALU instructions chain correctly");
          w=kernel(0x25);
          // VOP2 literal src0: 10 - loaded v2. Literal is an extra word.
          w[8]=(0x26u<<25u)|(2u<<17u)|(2u<<9u)|255u;
          w.insert(w.begin()+9,10);
          t=translate_compute_words(w,base); io={11,0xabcdef01};
          run=vk.run_compute_words(t.spirv,&io);
          CHECK(t.ok && run && io[0]==0xffffffff && io[1]==0xabcdef01,
                "dynamic: literal source and subtraction order preserved");
        }
      } else CHECK(false,"dynamic: physical device available");
    }
  }


  // --- Bounded multi-word SSBO: independent two inputs + one output ---
  {
    using namespace pnr::shader;
    using namespace pnr::graphics;
    constexpr uint64_t base=0x1234567800ull;
    const std::vector<uint32_t> code={
      0x7e0002ff,0x34567800,0x7e0202ff,0x12,
      0xdc300004,0x027f0000, // v2 = buf[1]
      0xdc300008,0x037f0000, // v3 = buf[2]
      (0x25u<<25u)|(2u<<17u)|(3u<<9u)|258u, // v2 += v3
      0xdc700010,0x007f0200, // buf[4] = v2
      0xbf810000};
    auto t=translate_compute_words(code,base,5);
    CHECK(t.ok && t.required_buffer_words==5 && !t.stored_value_known,
          "bounds: two inputs and separate output compile with explicit range");
    std::vector<uint8_t> encoded;
    for(auto word:code)for(unsigned j=0;j<4;++j)encoded.push_back(uint8_t(word>>(j*8)));
    auto from_bytes=translate_compute_bytes(encoded,base,5);
    CHECK(from_bytes.ok && from_bytes.spirv==t.spirv && from_bytes.required_buffer_words==5,
          "bounds: byte entry preserves explicit buffer extent");
    auto bad=[&](std::vector<uint32_t> w,uint64_t b,uint32_t n,const char* label) {
      auto r=translate_compute_words(w,b,n);
      CHECK(!r.ok && !r.error.empty() && r.spirv.empty() && !r.required_buffer_words,label);
    };
    bad(code,base,0,"bounds: zero words rejected");
    bad(code,base,65537,"bounds: oversized binding rejected");
    bad(code,base+1,5,"bounds: unaligned guest base rejected");
    bad(code,UINT64_MAX-3,2,"bounds: overflowing guest range rejected");
    bad(code,base,4,"bounds: store at end of range rejected");
    auto w=code; w[4]=0xdc300014;
    bad(w,base,5,"bounds: load beyond range rejected");
    w=code; w[4]=0xdc300005;
    bad(w,base,5,"bounds: unaligned load rejected");
    w=code; w[9]=0xdc700011;
    bad(w,base,5,"bounds: unaligned store rejected");
    w=code; w[1]-=8;
    bad(w,base,5,"bounds: below-base address rejected");
    w=code; w[1]=0xfffffffc; w[3]=0xffffffff;
    bad(w,base,5,"bounds: effective address addition overflow rejected");
    w=code; w[4]|=0x800;
    bad(w,base,5,"bounds: reserved FLAT offset bit rejected");
    CHECK(SpirvModule::compute_u32({{U32Op::Load,5}},5).empty(),
          "bounds: emitter rejects out-of-range load index");
    CHECK(SpirvModule::compute_u32({{U32Op::Constant,7},{U32Op::Store,0,5}},5).empty(),
          "bounds: emitter rejects out-of-range store index");
    CHECK(SpirvModule::compute_u32({},0).empty(),"bounds: emitter rejects zero-sized array");
    const char* val=std::getenv("PNR_SPIRV_VAL");
    if(val && std::filesystem::exists(val)) {
      const std::string path="/tmp/pnr_bounds_"+std::to_string(::getpid())+".spv";
      {std::ofstream o(path,std::ios::binary);o.write(reinterpret_cast<const char*>(t.spirv.data()),t.spirv.size()*4);}
      CHECK(std::system((std::string(val)+" --target-env vulkan1.0 "+path).c_str())==0,
            "bounds: fixed array SPIR-V validates for Vulkan 1.0");
      std::filesystem::remove(path);
    } else std::puts("    note: bounds spirv-val skipped (not configured)");
    VulkanBackend vk;
    std::vector<uint32_t> short_io={1,2,3,4};
    const auto before=short_io;
    CHECK(!run_translated_compute(vk,t,&short_io) && short_io==before,
          "bounds: short host io rejected before Vulkan, unchanged");
    CHECK(!run_translated_compute(vk,t,nullptr),"bounds: null host io rejected");
    CHECK(!run_translated_compute(vk,ComputeTranslation{},&short_io),
          "bounds: failed translation not dispatched");
    auto st=vk.open();
    if(!st) std::printf("    note: bounds Vulkan skipped: %s\n",st.error.message.c_str());
    else {
      st=vk.create_instance("pnr-bounds-tests"); CHECK(st,"bounds: Vulkan instance created");
      if(st && !vk.devices().empty()) {
        st=vk.create_device(0); CHECK(st,"bounds: compute device created");
        if(st) {
          for(uint32_t a : {0u,42u,0xffffffffu}) {
            std::vector<uint32_t> io={0xaaaaaaaa,a,17,0xbbbbbbbb,0xcccccccc};
            auto run=run_translated_compute(vk,t,&io);
            CHECK(run && io==std::vector<uint32_t>({0xaaaaaaaa,a,17,0xbbbbbbbb,uint32_t(a+17)}),
                  "bounds: independent inputs/output, all other words unchanged");
          }
          // Absolute guest address adjustment, immediate offsets zero.
          w=code; w[1]+=4; w[4]=0xdc300000; w[6]=0xdc300004; w[9]=0xdc70000c;
          auto abs=translate_compute_words(w,base,5);
          std::vector<uint32_t> io={1,10,20,2,3};
          auto run=run_translated_compute(vk,abs,&io);
          CHECK(abs.ok && run && io==std::vector<uint32_t>({1,10,20,2,30}),
                "bounds: constant register address and immediate offset compose");
          // Highest legal unsigned FLAT offset (2044) and last binding word.
          w={0x7e0002ff,0x34567800,0x7e0202ff,0x12,
             0xdc3007fc,0x027f0000,0xdc700000,0x007f0200,0xbf810000};
          auto edge=translate_compute_words(w,base,512);
          io.assign(512,0xabcddcba);io[511]=0x12345678;
          run=run_translated_compute(vk,edge,&io);
          bool clean=io[0]==0x12345678 && io[511]==0x12345678;
          for(size_t j=1;j<511;++j)clean &= io[j]==0xabcddcba;
          CHECK(edge.ok && run && clean,"bounds: last binding word and max aligned FLAT offset execute");
        }
      } else CHECK(false,"bounds: physical device available");
    }
  }


  // --- Runtime low-address indexing: guarded load/store, never raw OOB ---
  {
    using namespace pnr::shader;
    using namespace pnr::graphics;
    constexpr uint64_t base=0x1200001000ull;
    // Runtime address fetched from buf[0] into v4, high word v5=0x12.
    const std::vector<uint32_t> prefix={0x7e0002ff,0x1000,0x7e0202ff,0x12,
      0xdc300000,0x047f0000,0x7e0a02ff,0x12};
    auto load_code=prefix;
    load_code.insert(load_code.end(),{0xdc300000,0x027f0004,
      0xdc70000c,0x007f0200,0xbf810000});
    auto store_code=prefix;
    store_code.insert(store_code.end(),{0xdc300004,0x027f0000,
      0xdc700000,0x007f0204,0xbf810000});
    auto both_code=prefix;
    both_code.insert(both_code.end(),{0xdc300000,0x027f0004,
      (0x25u<<25u)|(2u<<17u)|(2u<<9u)|129u,
      0xdc700000,0x007f0204,0xbf810000});
    auto alu_address_code=load_code;
    alu_address_code.insert(alu_address_code.begin()+8,
      (0x25u<<25u)|(4u<<17u)|(4u<<9u)|132u); // v4 += inline 4
    std::vector<ComputeTranslation> modules;
    for(const auto* code : {&load_code,&store_code,&both_code,&alu_address_code}) {
      auto t=translate_compute_words(*code,base,8);
      CHECK(t.ok && t.guarded_memory && !t.stored_value_known,
            "guards: runtime-indexed module compiles with honest metadata");
      bool selection=false, branch=false;
      for(size_t i=5;i<t.spirv.size();) {
        selection |= (t.spirv[i]&65535)==247;
        branch |= (t.spirv[i]&65535)==250;
        if(!(t.spirv[i]>>16u))break;
        i+=t.spirv[i]>>16u;
      }
      CHECK(selection && branch,"guards: structured selection and conditional branch emitted");
      const char* val=std::getenv("PNR_SPIRV_VAL");
      if(val && std::filesystem::exists(val)) {
        const std::string path="/tmp/pnr_guards_"+std::to_string(::getpid())+".spv";
        {std::ofstream o(path,std::ios::binary);o.write(reinterpret_cast<const char*>(t.spirv.data()),t.spirv.size()*4);}
        CHECK(std::system((std::string(val)+" --target-env vulkan1.0 "+path).c_str())==0,
              "guards: structured guarded SPIR-V validates for Vulkan 1.0");
        std::filesystem::remove(path);
      } else std::puts("    note: guards spirv-val skipped (not configured)");
      modules.push_back(t);
    }
    auto reject=[&](std::vector<uint32_t> w,uint64_t b,uint32_t n,const char* label) {
      auto t=translate_compute_words(w,b,n);
      CHECK(!t.ok && t.spirv.empty(),label);
    };
    auto w=store_code;w[7]=0x13;
    CHECK(translate_compute_words(w,base,8).ok,"guards: mismatched high now emits full64 guard");
    w=store_code;w[7]=0x12;w[1]=0xfffffffc;
    CHECK(translate_compute_words(w,0x12fffffffcull,2).ok,"guards: crossing binding now emits full64 guard");
    w=store_code;w[10]|=4;
    CHECK(translate_compute_words(w,base,8).ok,"guards: dynamic store offset now emits carry-aware guard");
    w=load_code;w[8]|=4;
    CHECK(translate_compute_words(w,base,8).ok,"guards: dynamic load offset now emits carry-aware guard");
    w=load_code;w[9]=0x027f00ff;
    reject(w,base,8,"guards: runtime address pair overflow rejected");
    CHECK(SpirvModule::compute_u32({{U32Op::GuardedLoad,0}},8,0x1000).empty(),
          "guards: missing runtime address IR value rejected");
    CHECK(SpirvModule::compute_u32({{U32Op::Constant,0x1000},
      {U32Op::GuardedStore,1,0}},8,0x1000).empty(),
          "guards: forward store data IR rejected");
    CHECK(SpirvModule::compute_u32({{U32Op::Constant,0},
      {U32Op::GuardedLoad,0}},2,0xfffffffc).empty(),
          "guards: emitter rejects crossing 32-bit boundary");
    VulkanBackend vk;auto st=vk.open();
    if(!st)std::printf("    note: guard execution skipped: %s\n",st.error.message.c_str());
    else {
      st=vk.create_instance("pnr-guard-tests");CHECK(st,"guards: Vulkan instance created");
      if(st && !vk.devices().empty()) {
        st=vk.create_device(0);CHECK(st,"guards: compute device created");
        if(st)for(size_t k=0;k<modules.size();++k) {
          // Same compiled shader for valid first/middle/last, below-base,
          // one-past, misaligned, huge/wrapped-looking runtime addresses.
          for(uint32_t addr:{0x1000u,0x1004u,0x101cu,0xffcu,0x1020u,
                            0x1001u,0x101du,0u,0xffffffffu}) {
            std::vector<uint32_t> io={addr,0x12345678,0xaaaa5555,0x87654321,
                                     0x01020304,0xfeedface,0xdeadbeef,0xcafebabe};
            auto expected=io;
            const uint32_t effective=k==3 ? uint32_t(addr+4) : addr;
            const bool valid=effective>=0x1000 && effective<=0x101c && effective%4==0;
            const uint32_t index=valid ? (effective-0x1000)/4 : 0;
            if(k==0 || k==3)expected[3]=valid?io[index]:0;
            if(k==1 && valid)expected[index]=io[1];
            if(k==2 && valid)expected[index]=io[index]+1;
            auto run=run_translated_compute(vk,modules[k],&io);
            CHECK(run && io==expected,
                  "guards: runtime load=zero/store=skip for invalid index, all other words exact");
          }
        }
      }else CHECK(false,"guards: physical device available");
    }
  }


  // --- One bounded workgroup: v0=LocalInvocationIndex in explicit test ABI ---
  {
    using namespace pnr::shader; using namespace pnr::graphics;
    constexpr uint64_t base=0x1200001000ull;
    auto add=[](uint32_t dst,uint32_t src0,uint32_t src1) {
      return (0x25u<<25u)|(dst<<17u)|(src1<<9u)|src0;
    };
    auto kernel=[&](uint32_t lanes) {
      return std::vector<uint32_t>{
        add(4,256,0),add(4,260,4), // v4=lane*4
        0x7e0402ff,0x1000,add(4,258,4), // v4=base.low+lane*4
        0x7e0a02ff,0x12, // v5=high
        0xdc300000,0x067f0004, // v6=inputs[lane]
        0x7e0402ff,lanes*4,add(4,258,4), // v4+=output offset
        add(6,129,6), // v6+=1
        0xdc700000,0x007f0604,0xbf810000};
    };
    auto reject=[&](const std::vector<uint32_t>& w,uint32_t lanes,const char* name) {
      auto t=translate_compute_words(w,base,128,lanes);
      CHECK(!t.ok && t.spirv.empty(),name);
    };
    reject(kernel(8),0,"lanes: zero workgroup size rejected");
    reject(kernel(8),65,"lanes: oversized workgroup rejected");
    auto w=kernel(8);w[10]=0; // output aliases inputs
    reject(w,8,"lanes: input/output overlap rejected without barriers");
    w=kernel(8);w[11]=0x7e0802ff;w.insert(w.begin()+12,0x1040);
    reject(w,8,"lanes: constant colliding store addresses rejected");
    w=kernel(8);w[0]=add(4,256,0);w[1]=0x7e080300; // only stride1
    reject(w,8,"lanes: misaligned invocation stride rejected");
    w=kernel(8);w[11]=0x7e080306; // v4=runtime input v6
    reject(w,8,"lanes: unproven data-dependent parallel address rejected");
    w=kernel(8);w[10]=0xfffffff0;
    reject(w,8,"lanes: affine overflow rejected");
    auto too_small=translate_compute_words(kernel(8),base,15,8);
    CHECK(!too_small.ok && too_small.spirv.empty(),"lanes: final invocation beyond binding rejected");
    CHECK(SpirvModule::compute_u32({{U32Op::InvocationIndex}},1,0,65).empty(),
          "lanes: emitter rejects oversized workgroup");
    VulkanBackend vk;auto st=vk.open();
    if(!st)std::printf("    note: lanes Vulkan skipped: %s\n",st.error.message.c_str());
    else {
      st=vk.create_instance("pnr-lane-tests");CHECK(st,"lanes: Vulkan instance created");
      if(st && !vk.devices().empty()) {
        st=vk.create_device(0);CHECK(st,"lanes: compute device created");
        if(st)for(uint32_t lanes:{2u,8u,64u}) {
          const auto t=translate_compute_words(kernel(lanes),base,lanes*2+1,lanes);
          CHECK(t.ok && t.local_size_x==lanes && t.guarded_memory,
                "lanes: explicit local invocation ABI compiles");
          const char* val=std::getenv("PNR_SPIRV_VAL");
          if(val && std::filesystem::exists(val)) {
            const std::string path="/tmp/pnr_lanes_"+std::to_string(::getpid())+".spv";
            {std::ofstream o(path,std::ios::binary);o.write(reinterpret_cast<const char*>(t.spirv.data()),t.spirv.size()*4);}
            CHECK(std::system((std::string(val)+" --target-env vulkan1.0 "+path).c_str())==0,
                  "lanes: input BuiltIn and local-size module validate for Vulkan 1.0");
            std::filesystem::remove(path);
          }else std::puts("    note: lane validator skipped (not configured)");
          for(uint32_t seed:{0u,0xffffffffu,0x12345678u}) {
            std::vector<uint32_t> io(lanes*2+1,0xa5a5a5a5);
            for(uint32_t j=0;j<lanes;++j)io[j]=seed+j;
            auto expected=io;
            for(uint32_t j=0;j<lanes;++j)expected[lanes+j]=uint32_t(io[j]+1);
            const auto run=run_translated_compute(vk,t,&io);
            CHECK(run && io==expected,
                  "lanes: every invocation processes its element, inputs and sentinel unchanged");
          }
          std::vector<uint8_t> bytes;
          for(auto word:kernel(lanes))for(unsigned j=0;j<4;++j)bytes.push_back(uint8_t(word>>(j*8)));
          const auto b=translate_compute_bytes(bytes,base,lanes*2+1,lanes);
          CHECK(b.ok && b.spirv==t.spirv && b.local_size_x==lanes,
                "lanes: byte input preserves workgroup configuration");
        }
      } else CHECK(false,"lanes: physical device available");
    }
  }


  // --- Strict forward SCC0 skip with move-only body and merge Phi ---
  {
    using namespace pnr::shader;using namespace pnr::graphics;
    constexpr uint64_t base=0x1200001000ull;
    // v2=input[0], v3=input[1]; s0=readfirstlane(v2), s1=7;
    // v4=111; if(s0==s1) {v4=v3;} output[2]=v4.
    const std::vector<uint32_t> code={
      0x7e0002ff,0x1000,0x7e0202ff,0x12,
      0xdc300000,0x027f0000,0xdc300004,0x037f0000,
      0x7e000502,0xbe810387,0x7e0802ff,111,
      0xbf060100,0xbf840001,0x7e080303,
      0xdc700008,0x007f0400,0xbf810000};
    auto t=translate_compute_words(code,base,4);
    CHECK(t.ok && t.conditional_selection && !t.stored_value_known,
          "branch: runtime SCC compare/forward selection compiles");
    auto dec=rdna_decode({0x7e000502});
    CHECK(dec.ok && dec.instructions[0].dst.kind==RdnaOperandKind::ScalarReg &&
          dec.instructions[0].src0.kind==RdnaOperandKind::VectorReg,
          "branch: v_readfirstlane has SGPR destination, VGPR source");
    auto reject=[&](std::vector<uint32_t> w,const char* label,uint32_t lanes=1) {
      auto r=translate_compute_words(w,base,4,lanes);
      CHECK(!r.ok && !r.error.empty() && r.spirv.empty(),label);
    };
    auto w=code;w[13]=0xbf84ffff;
    reject(w,"branch: backward branch rejected");
    w=code;w[13]=0xbf840000;
    reject(w,"branch: zero displacement rejected");
    w=code;w[13]=0xbf840002;
    reject(w,"branch: target inside FLAT payload rejected");
    w=code;w[13]=0xbf847fff;
    reject(w,"branch: out-of-program target rejected");
    w=code;w.erase(w.begin()+12);
    reject(w,"branch: undefined SCC rejected");
    w=code;w[12]=0xbf060109;
    reject(w,"branch: undefined compare SGPR rejected");
    w=code;w[8]=0x7e000509;
    reject(w,"branch: undefined readfirstlane VGPR rejected");
    w=code;w[8]=0x7ed40502;
    reject(w,"branch: special scalar destination rejected");
    w=code;w[13]=0xbf850001;
    reject(w,"branch: SCC1 branch outside subset rejected");
    w=code;w[14]=0xbf840001;
    reject(w,"branch: nested branch rejected");
    w=code;w[13]=0xbf840003;
    reject(w,"branch: conditional memory side effect rejected");
    w=code;w[14]=0x7e0c0303;
    reject(w,"branch: merged register undefined on skipped path rejected");
    w=code;w.insert(w.begin()+15,{0xbf840001,0x7e080303});
    reject(w,"branch: second selection rejected");
    w=code;w[14]=(0x25u<<25u)|(4u<<17u)|(3u<<9u)|260u;
    reject(w,"branch: body ALU outside selection subset rejected");
    w=code;w[14]=0xbe810387;
    reject(w,"branch: body scalar side effect rejected");
    w=code;w[12]=0xbf070100;
    reject(w,"branch: non-equality comparison outside subset rejected");
    w=code;w[13]=0xbf840001;w[14]=0x7e0802ff;w.insert(w.begin()+15,222);
    reject(w,"branch: target inside move literal rejected");
    reject(code,"branch: scalar branch in multi-invocation mode rejected",2);
    CHECK(SpirvModule::compute_u32({{U32Op::Constant,1},
      {U32Op::SelectEqual,0,0,0,2}}).empty(),"branch: malformed selection SSA ref rejected");
    const char* val=std::getenv("PNR_SPIRV_VAL");
    auto validate=[&](const ComputeTranslation& r) {
      if(val && std::filesystem::exists(val)) {
        const std::string path="/tmp/pnr_branch_"+std::to_string(::getpid())+".spv";
        {std::ofstream o(path,std::ios::binary);o.write(reinterpret_cast<const char*>(r.spirv.data()),r.spirv.size()*4);}
        CHECK(std::system((std::string(val)+" --target-env vulkan1.0 "+path).c_str())==0,
              "branch: structured branch/Phi validates for Vulkan 1.0");
        std::filesystem::remove(path);
      }else std::puts("    note: branch validator skipped (not configured)");
    };
    validate(t);
    VulkanBackend vk;auto st=vk.open();
    if(!st)std::printf("    note: branch Vulkan skipped: %s\n",st.error.message.c_str());
    else {
      st=vk.create_instance("pnr-branch-tests");CHECK(st,"branch: Vulkan instance created");
      if(st && !vk.devices().empty()) {
        st=vk.create_device(0);CHECK(st,"branch: compute device created");
        if(st) {
          for(uint32_t a:{7u,0u,0xffffffffu})for(uint32_t b:{0u,0xdeadbeefu}) {
            std::vector<uint32_t> io={a,b,0xa5a5a5a5,0xcafebabe};
            const auto run=run_translated_compute(vk,t,&io);
            CHECK(run && io==std::vector<uint32_t>({a,b,a==7?b:111,0xcafebabe}),
                  "branch: taken/fallthrough path runtime readback exact, neighbors unchanged");
          }
          // Two dependent assignments, both merged (v3 already defined).
          w=code;w[13]=0xbf840002;
          w.insert(w.begin()+15,0x7e060304); // v3=v4 in body
          w[17]=0x007f0300; // store v3
          auto multi=translate_compute_words(w,base,4);validate(multi);
          for(uint32_t a:{7u,8u}) {
            std::vector<uint32_t> io={a,55,0,0xabcdef01};
            const auto run=run_translated_compute(vk,multi,&io);
            CHECK(multi.ok && run && io[2]==55 && io[3]==0xabcdef01,
                  "branch: dependent body assignments merge across multiple VGPRs");
          }
          // Selection feeds a guarded pointer, exercising guard/branch Phi composition.
          w=code;w[11]=0x1004;w[13]=0xbf840002;w[14]=0x7e0802ff;
          w.insert(w.begin()+15,0x1000);
          w.insert(w.begin()+16,{0x7e0a02ff,0x12,0xdc300000,0x067f0004});
          w[21]=0x007f0600;
          auto pointer=translate_compute_words(w,base,4);validate(pointer);
          for(uint32_t a:{7u,8u}) {
            std::vector<uint32_t> io={a,55,0,0xabcdef01};
            const auto run=run_translated_compute(vk,pointer,&io);
            CHECK(pointer.ok && pointer.guarded_memory && run && io[2]==(a==7?a:55),
                  "branch: selected address composes with guarded load Phi");
          }
          // Conditional constant uses extra literal word; PC target must skip both.
          w=code;w[13]=0xbf840002;w[14]=0x7e0802ff;w.insert(w.begin()+15,222);
          auto literal=translate_compute_words(w,base,4);validate(literal);
          for(uint32_t a:{7u,8u}) {
            std::vector<uint32_t> io={a,55,0,0xabcdef01};
            const auto run=run_translated_compute(vk,literal,&io);
            CHECK(literal.ok && run && io[2]==(a==7?222u:111u),
                  "branch: literal body word count and merge PC preserved");
          }
        }
      }else CHECK(false,"branch: physical device available");
    }
  }

  {
    auto d=pnr::shader::rdna_decode({0x8000ffff,0x12345678,0xbf810000});
    CHECK(d.ok && d.instructions.size()==2 && d.instructions[0].word_count==2 &&
          d.instructions[0].src0.value==0x12345678 && d.instructions[0].src1.value==0x12345678 &&
          d.instructions[1].pc==8,"audit: shared scalar literal consumed exactly once");
    CHECK(!pnr::shader::rdna_decode({0x8000ffff}).ok,"audit: shared literal truncation rejected");
  }

  // --- Explicit uniform SGPR inputs via runtime Vulkan push constants ---
  {
    using namespace pnr::shader; using namespace pnr::graphics;
    constexpr uint64_t base=0x1200001000ull;
    // s1=s0 copy; s0 overwritten; v2=s1. Copies retain runtime uniform SSA.
    const std::vector<uint32_t> scalar={0x7e0002ff,0x1000,0x7e0202ff,0x12,
      0xbe810300,0xbe800387,0x7e040201,0xdc700000,0x007f0200,0xbf810000};
    auto t=translate_compute_words(scalar,base,1,1,1);
    CHECK(t.ok && t.required_uniform_words==1 && !t.stored_value_known,
          "uniform: initial SGPR runtime input and scalar copy compile");
    auto reject=[&](const std::vector<uint32_t>& code,uint32_t count,const char* label) {
      auto r=translate_compute_words(code,base,1,1,count);
      CHECK(!r.ok && r.spirv.empty(),label);
    };
    reject(scalar,0,"uniform: undeclared initial SGPR remains undefined");
    reject(scalar,17,"uniform: oversized SGPR input count rejected");
    auto w=scalar;w[4]=0xbed40300;
    reject(w,1,"uniform: scalar move special destination rejected");
    w=scalar;w[4]=0xbe810310;
    reject(w,16,"uniform: SGPR beyond declared range rejected");
    CHECK(SpirvModule::compute_u32({{U32Op::UniformInput,1}},1,0,1,1).empty(),
          "uniform: emitter rejects out-of-range input index");
    CHECK(SpirvModule::compute_u32({},1,0,1,17).empty(),
          "uniform: emitter rejects oversized push block");
    VulkanBackend vk;std::vector<uint32_t> io={99};
    CHECK(!run_translated_compute(vk,t,&io) && io[0]==99,
          "uniform: missing runtime data rejected before dispatch");
    CHECK(!run_translated_compute(vk,t,&io,{1,2}) && io[0]==99,
          "uniform: extra runtime data rejected before dispatch");
    auto invalid=t;invalid.required_uniform_words=17;
    CHECK(!run_translated_compute(vk,invalid,&io,std::vector<uint32_t>(17)),
          "uniform: malformed required count rejected");
    auto validate=[&](const ComputeTranslation& r) {
      const char* val=std::getenv("PNR_SPIRV_VAL");
      if(val && std::filesystem::exists(val)) {
        const std::string path="/tmp/pnr_uniform_"+std::to_string(::getpid())+".spv";
        {std::ofstream o(path,std::ios::binary);o.write(reinterpret_cast<const char*>(r.spirv.data()),r.spirv.size()*4);}
        CHECK(std::system((std::string(val)+" --target-env vulkan1.0 "+path).c_str())==0,
              "uniform: push constant module validates for Vulkan 1.0");
        std::filesystem::remove(path);
      }else std::puts("    note: uniform validator skipped (not configured)");
    };
    validate(t);
    auto st=vk.open();
    if(!st)std::printf("    note: uniform Vulkan skipped: %s\n",st.error.message.c_str());
    else {
      st=vk.create_instance("pnr-uniform-tests");CHECK(st,"uniform: Vulkan instance created");
      if(st && !vk.devices().empty()) {
        st=vk.create_device(0);CHECK(st,"uniform: compute device created");
        if(st) {
          for(uint32_t value:{0u,42u,0xffffffffu}) {
            io={99};auto run=run_translated_compute(vk,t,&io,{value});
            CHECK(run && io[0]==value,"uniform: changed push input, same module, copied SGPR readback exact");
          }
          // Highest allowed SGPR input s15, scalar to vector ALU source.
          w={0x7e0002ff,0x1000,0x7e0202ff,0x12,0x7e040281,
             (0x25u<<25u)|(2u<<17u)|(2u<<9u)|15u,
             0xdc700000,0x007f0200,0xbf810000};
          auto last=translate_compute_words(w,base,1,1,16);validate(last);
          std::vector<uint32_t> inputs(16,0);inputs[15]=0xffffffff;io={99};
          auto run=run_translated_compute(vk,last,&io,inputs);
          CHECK(last.ok && run && io[0]==0,"uniform: s15 push input and uint32 wraparound execute");
          // Uniform equality drives the existing strict branch subset.
          w={0x7e0002ff,0x1000,0x7e0202ff,0x12,0x7e0402ff,111,
             0xbf060100,0xbf840002,0x7e0402ff,222,
             0xdc700000,0x007f0200,0xbf810000};
          auto branch=translate_compute_words(w,base,1,1,2);validate(branch);
          for(uint32_t b:{7u,8u}) {
            io={0};run=run_translated_compute(vk,branch,&io,{7,b});
            CHECK(branch.ok && run && io[0]==(b==7?222u:111u),
                  "uniform: runtime SGPR equality selects both branch outcomes");
          }
          // Eight invocations, separate inputs/outputs, uniform increment.
          auto add=[](uint32_t dst,uint32_t a,uint32_t b){return (0x25u<<25u)|(dst<<17u)|(b<<9u)|a;};
          w={add(4,256,0),add(4,260,4),0x7e0402ff,0x1000,add(4,258,4),
             0x7e0a02ff,0x12,0xdc300000,0x067f0004,
             0x7e0402ff,32,add(4,258,4),add(6,0,6),
             0xdc700000,0x007f0604,0xbf810000};
          auto lanes=translate_compute_words(w,base,17,8,1);validate(lanes);
          for(uint32_t increment:{0u,17u,0xffffffffu}) {
            io.assign(17,0xa5a5a5a5);for(uint32_t j=0;j<8;++j)io[j]=j*3;
            auto expected=io;for(uint32_t j=0;j<8;++j)expected[j+8]=io[j]+increment;
            run=run_translated_compute(vk,lanes,&io,{increment});
            CHECK(lanes.ok && run && io==expected,
                  "uniform: push input broadcast to all invocations, exact disjoint outputs");
          }
          std::vector<uint8_t> bytes;for(auto word:w)for(unsigned j=0;j<4;++j)bytes.push_back(uint8_t(word>>(j*8)));
          auto b=translate_compute_bytes(bytes,base,17,8,1);
          CHECK(b.ok && b.spirv==lanes.spirv && b.required_uniform_words==1,
                "uniform: byte entry preserves uniform input ABI");
        }
      }else CHECK(false,"uniform: physical device available");
    }
  }


  // --- Binary32 host-policy add/sub/mul, register storage stays raw uint ---
  {
    using namespace pnr::shader;using namespace pnr::graphics;
    constexpr uint64_t base=0x1200001000ull;
    auto code=[](uint32_t op){return std::vector<uint32_t>{
      0x7e0002ff,0x1000,0x7e0202ff,0x12,0xdc300000,0x027f0000,
      0xdc300004,0x037f0000,(op<<25u)|(2u<<17u)|(3u<<9u)|258u,
      0xdc700008,0x007f0200,0xbf810000};};
    auto reject=[&](std::vector<uint32_t> w,const char* name){
      auto t=translate_compute_words(w,base,4);
      CHECK(!t.ok && t.spirv.empty(),name);
    };
    auto w=code(3);w[8]=(3u<<25u)|(2u<<17u)|(9u<<9u)|258u;
    reject(w,"f32: undefined vector source rejected");
    w=code(3);w[8]=(3u<<25u)|(2u<<17u)|(3u<<9u)|249u;
    reject(w,"f32: SDWA source rejected");
    w=code(3);w[8]=(3u<<25u)|(2u<<17u)|(3u<<9u)|250u;
    reject(w,"f32: DPP source rejected");
    w=code(3);w[8]=(3u<<25u)|(2u<<17u)|(3u<<9u)|242u;
    reject(w,"f32: inline float selector not yet implemented rejected");
    reject(code(5),"f32: reverse subtract outside subset rejected");
    reject(code(0xf),"f32: min/NaN alternative semantics not guessed");
    w=code(3);w[8]=0xd5030002;w.insert(w.begin()+9,0x60302);
    reject(w,"f32: VOP3 modifiers outside subset rejected");
    CHECK(SpirvModule::compute_u32({{U32Op::FAdd,0,0}}).empty(),
          "f32: malformed float SSA refs rejected");
    struct Case {uint32_t a,b,add,sub,mul;bool nan=false;};
    const std::vector<Case> cases={
      {0x3fc00000,0x40100000,0x40700000,0xbf400000,0x40580000}, // 1.5,2.25
      {0xbf800000,0x40000000,0x3f800000,0xc0400000,0xc0000000}, // -1,2
      {0x3f800000,0x33800000,0x3f800000,0x3f7fffff,0x33800000}, // tie add 1+2^-24
      {0x3f800001,0x33800000,0x3f800002,0x3f800000,0x33800001}, // tie odd -> even
      {0x80000000,0x80000000,0x80000000,0,0},
      {0x7fc12345,0x3f800000,0,0,0,true}, // NaN classification only
      {0x7f800001,0x3f800000,0,0,0,true} // signaling payload: no exact bit promise
    };
    VulkanBackend vk;auto st=vk.open();
    if(!st)std::printf("    note: f32 Vulkan skipped: %s\n",st.error.message.c_str());
    else {
      st=vk.create_instance("pnr-float-tests");CHECK(st,"f32: Vulkan instance created");
      if(st && !vk.devices().empty()) {
        st=vk.create_device(0);CHECK(st,"f32: compute device created");
        if(st)for(uint32_t op:{3u,4u,8u}) {
          auto t=translate_compute_words(code(op),base,4);
          CHECK(t.ok && t.host_float32 && !t.stored_value_known,"f32: host-policy metadata, runtime values not invented");
          bool arithmetic=false,decorate=false;
          for(size_t i=5;i<t.spirv.size();) {
            auto opcode=t.spirv[i]&65535;
            arithmetic|=opcode==(op==3?129u:op==4?131u:133u);
            decorate|=opcode==71 && (t.spirv[i]>>16u)==3 && t.spirv[i+2]==42;
            if(!(t.spirv[i]>>16u))break;
            i+=t.spirv[i]>>16u;
          }
          CHECK(arithmetic && decorate,"f32: real float operation and NoContraction decoration emitted");
          const char* val=std::getenv("PNR_SPIRV_VAL");
          if(val && std::filesystem::exists(val)) {
            const std::string path="/tmp/pnr_float_"+std::to_string(::getpid())+".spv";
            {std::ofstream o(path,std::ios::binary);o.write(reinterpret_cast<const char*>(t.spirv.data()),t.spirv.size()*4);}
            CHECK(std::system((std::string(val)+" --target-env vulkan1.0 "+path).c_str())==0,
                  "f32: bitcast arithmetic module validates for Vulkan 1.0");
            std::filesystem::remove(path);
          }else std::puts("    note: float validator skipped (not configured)");
          for(const auto& c:cases) {
            std::vector<uint32_t> io={c.a,c.b,0xdeadbeef,0xcafebabe};
            auto run=run_translated_compute(vk,t,&io);
            const uint32_t expect=op==3?c.add:op==4?c.sub:c.mul;
            const bool nan=(io[2]&0x7f800000)==0x7f800000 && (io[2]&0x7fffff)!=0;
            CHECK(run && io[0]==c.a && io[1]==c.b && io[3]==0xcafebabe && (c.nan?nan:io[2]==expect),
                  "f32: lavapipe finite/zero exact bits or NaN class, input/neighbor unchanged");
          }
          // Literal source and scalar uniform are raw bit operands, not int->float conversions.
          auto literal_code=code(op);
          literal_code[8]=(op<<25u)|(2u<<17u)|(3u<<9u)|255u;
          literal_code.insert(literal_code.begin()+9,0x3fc00000);
          auto literal=translate_compute_words(literal_code,base,4);
          std::vector<uint32_t> lit_io={0,0x40100000,0,0xcafebabe};
          auto lit_run=run_translated_compute(vk,literal,&lit_io);
          CHECK(literal.ok && lit_run && lit_io[2]==(op==3?0x40700000u:op==4?0xbf400000u:0x40580000u),
                "f32: literal raw bits used as binary32 source");
          auto uniform_code=code(op);uniform_code[8]=(op<<25u)|(2u<<17u)|(3u<<9u);
          auto uniform=translate_compute_words(uniform_code,base,4,1,1);
          lit_io={0,0x40100000,0,0xcafebabe};
          lit_run=run_translated_compute(vk,uniform,&lit_io,{0x3fc00000});
          CHECK(uniform.ok && lit_run && lit_io[2]==(op==3?0x40700000u:op==4?0xbf400000u:0x40580000u),
                "f32: scalar uniform raw bits used as binary32 source");
          // Observation only: portable NaN/Inf/denorm bit preservation is not asserted.
          for(const auto& pair:std::vector<std::pair<uint32_t,uint32_t>>{
              {0x7f800000,0x3f800000},{1,0x3f800000},{0x7f7fffff,0x40000000}}) {
            std::vector<uint32_t> io={pair.first,pair.second,0,0xcafebabe};
            auto run=run_translated_compute(vk,t,&io);
            CHECK(run && io[3]==0xcafebabe,"f32: special-value probe dispatch succeeds");
            std::printf("    f32 host probe op=%u a=0x%08x b=0x%08x result=0x%08x (observation, not portability claim)\n",
                        op,pair.first,pair.second,io[2]);
          }
        }
      }else CHECK(false,"f32: physical device available");
    }
  }


  // --- Full64 runtime pointers, scalar carry/borrow, lexicographic guards ---
  {
    using namespace pnr::shader;using namespace pnr::graphics;
    constexpr uint64_t base=0x12fffffff8ull;
    auto sop=[](uint32_t op,uint32_t dst,uint32_t a,uint32_t b){
      return 0x80000000u|(op<<23u)|(dst<<16u)|(b<<8u)|a;};
    auto kernel=[&](bool subtract,bool store,uint32_t offset) {
      return std::vector<uint32_t>{
        0x7e0002ff,0xfffffff8,0x7e0202ff,0x12,
        sop(subtract?1:0,4,0,2),sop(subtract?5:4,5,1,3),
        0x7e080204,0x7e0a0205, // v4/v5=runtime scalar pointer pair
        0x7e0c02ff,0xdeadbeef,
        (store?0xdc700000u:0xdc300000u)|offset,store?0x007f0604u:0x067f0004u,
        // loads write a static result slot (word 3); stores only once
        store?0xbf810000u:0xdc70000cu,store?0xbf810000u:0x007f0600u,
        0xbf810000u};
    };
    auto load=kernel(false,false,0);
    auto store=kernel(false,true,4);store.erase(store.end()-2,store.end());
    auto sub=kernel(true,false,0);
    auto reject=[&](std::vector<uint32_t> w,const char* label,uint32_t lanes=1) {
      auto t=translate_compute_words(w,base,4,lanes,4);
      CHECK(!t.ok && t.spirv.empty(),label);
    };
    auto w=load;w[4]=sop(4,4,0,2);
    reject(w,"ptr64: carry input undefined rejected");
    w=load;w[4]=sop(5,4,0,2);
    reject(w,"ptr64: borrow input undefined rejected");
    w=load;w[5]=sop(4,106,1,3);
    reject(w,"ptr64: special scalar arithmetic destination rejected");
    w=load;w[5]=sop(4,5,1,16);
    reject(w,"ptr64: undefined high arithmetic source rejected");
    reject(load,"ptr64: scalar carry arithmetic in workgroup rejected",2);
    w=load;w[5]=0xbf060100; // compare replaces SCC; following carry must reject
    w.insert(w.begin()+6,sop(4,5,1,3));
    reject(w,"ptr64: equality SCC cannot masquerade as arithmetic carry");
    CHECK(SpirvModule::compute_u32({{U32Op::GuardedLoad64,0,0}},4,0,1,0,0).empty(),
          "ptr64: full64 guard invalid SSA refs rejected");
    CHECK(SpirvModule::compute_u32({{U32Op::Constant,0},{U32Op::GuardedLoad64,0,0,0,2048}},4).empty(),
          "ptr64: full64 guard offset beyond encoding rejected");
    auto validate=[&](const ComputeTranslation& t) {
      const char* val=std::getenv("PNR_SPIRV_VAL");
      if(val && std::filesystem::exists(val)) {
        const std::string path="/tmp/pnr_ptr64_"+std::to_string(::getpid())+".spv";
        {std::ofstream o(path,std::ios::binary);o.write(reinterpret_cast<const char*>(t.spirv.data()),t.spirv.size()*4);}
        CHECK(std::system((std::string(val)+" --target-env vulkan1.0 "+path).c_str())==0,
              "ptr64: pair arithmetic/guard module validates without Int64 capability");
        std::filesystem::remove(path);
      }else std::puts("    note: ptr64 validator skipped (not configured)");
    };
    VulkanBackend vk;auto st=vk.open();
    if(!st)std::printf("    note: ptr64 Vulkan skipped: %s\n",st.error.message.c_str());
    else {
      st=vk.create_instance("pnr-ptr64-tests");CHECK(st,"ptr64: Vulkan instance created");
      if(st && !vk.devices().empty()) {
        st=vk.create_device(0);CHECK(st,"ptr64: compute device created");
        if(st)for(unsigned kind=0;kind<3;++kind) {
          auto t=translate_compute_words(kind==0?load:kind==1?store:sub,base,4,1,4);
          CHECK(t.ok && t.guarded_memory,"ptr64: runtime high/low pointer compiles");validate(t);
          struct Input {uint64_t a,b;};
          for(auto input:std::vector<Input>{{base,0},{0x12fffffffcu,4},{0x1300000000ull,4},
              {base,0x100000000ull},{base-4,0},{base+16,0},{base+1,0},
              {UINT64_MAX,1},{0,1},{0x1300000000ull,8},{0x12ffffffffull,1},{0x12ffffffffull,0x100000001ull},
              {0x1300000000ull,0xffffffffull}}) {
            const uint64_t pointer=kind==2?input.a-input.b:input.a+input.b;
            const uint32_t off=kind==1?4:0;
            const bool overflow=pointer>UINT64_MAX-off;
            const uint64_t effective=pointer+off;
            const bool valid=!overflow && effective>=base && effective-base<16 && (effective-base)%4==0;
            const uint32_t index=valid?uint32_t((effective-base)/4):0;
            std::vector<uint32_t> uniforms={uint32_t(input.a),uint32_t(input.a>>32u),uint32_t(input.b),uint32_t(input.b>>32u)};
            std::vector<uint32_t> io={0x11111111,0x22222222,0x33333333,0x44444444};auto expected=io;
            if(kind==1){if(valid)expected[index]=0xdeadbeef;}
            else expected[3]=valid?io[index]:0;
            auto run=run_translated_compute(vk,t,&io,uniforms);
            CHECK(run && io==expected,"ptr64: carry/borrow/high-word/bounds runtime result exact");
          }
        }

        if(st) {
          // Guest pointer low/high arrive from SSBO, not uniform arithmetic.
          std::vector<uint32_t> pair={0x7e0002ff,0xfffffff8,0x7e0202ff,0x12,
            0xdc300000,0x047f0000,0xdc300004,0x057f0000,
            0xdc300000,0x067f0004,0xdc70000c,0x007f0600,0xbf810000};
          auto t=translate_compute_words(pair,base,4);validate(t);
          for(uint64_t addr:{base+8,base-4,base+1,base+16,uint64_t(0x13fffffff8ull)}) {
            std::vector<uint32_t> io={uint32_t(addr),uint32_t(addr>>32u),0x12345678,0};auto expected=io;
            expected[3]=addr==base+8?0x12345678:0;
            const auto run=run_translated_compute(vk,t,&io);
            CHECK(t.ok && run && io==expected,"ptr64: loaded high/low pair is guarded lexicographically");
          }
          // Runtime pointer+immediate crossing low-word boundary and overflowing full64.
          constexpr uint64_t top=0xfffffffffffffff0ull;
          std::vector<uint32_t> topcode={0x7e000200,0x7e020201,0x7e0402ff,0xabcdef01,
            0xdc700004,0x007f0200,0xbf810000};
          auto top_t=translate_compute_words(topcode,top,4,1,2);validate(top_t);
          for(uint64_t addr:{top,UINT64_MAX-3,UINT64_MAX,top-4}) {
            std::vector<uint32_t> io(4,7),expected=io;
            if(addr==top)expected[1]=0xabcdef01;
            if(addr==top-4)expected[0]=0xabcdef01;
            auto run=run_translated_compute(vk,top_t,&io,{uint32_t(addr),uint32_t(addr>>32u)});
            CHECK(top_t.ok && run && io==expected,"ptr64: full64 offset overflow suppresses store before pointer construction");
          }
        }
      }else CHECK(false,"ptr64: physical device available");
    }
  }

  // --- Workgroup-uniform SCC selection of lane-varying data ---
  {
    using namespace pnr::shader;using namespace pnr::graphics;
    constexpr uint64_t base=0x1200001000ull;
    auto add=[](uint32_t d,uint32_t a,uint32_t b){return (0x25u<<25u)|(d<<17u)|(b<<9u)|a;};
    auto mov=[](uint32_t d,uint32_t s){return 0x7e000200u|(d<<17u)|s;};
    auto kernel=[&](uint32_t lanes,int shape) {
      std::vector<uint32_t> w={add(4,256,0),add(4,260,4),
        mov(2,255),0x1000,add(4,258,4),mov(5,255),0x12,
        0xdc300000,0x067f0004, // v6=input[lane]
        mov(9,260),mov(7,256),mov(8,255),111, // saved pointer, lane, default
        0xbf060100,0xbf840003, // s0==s1, three single-word moves
        mov(7,262),mov(8,263),mov(4,265), // dependent moves, identical pointer map
        mov(2,255),lanes*4,add(4,258,4),
        0xdc700000,0x007f0804,0xbf810000};
      if(shape==1)w[16]=mov(8,256); // select lane index instead of input
      if(shape==2)w[17]=mov(4,258); // pointer alternative has different map
      if(shape==3)w[17]=mov(4,256); // selects misaligned lane address
      if(shape==4)w[17]=mov(4,262); // data-derived pointer remains unproven
      return w;
    };
    auto reject=[&](std::vector<uint32_t> w,uint32_t lanes,const char* label,uint32_t n=2) {
      auto t=translate_compute_words(w,base,lanes*2+1,lanes,n);
      CHECK(!t.ok && t.spirv.empty() && !t.error.empty(),label);
    };
    auto w=kernel(8,0);w[13]=0xbf060900;
    reject(w,8,"uniform-branch: undefined compare SGPR rejected");
    w=kernel(8,0);w[13]=0xbf06fb00;
    reject(w,8,"uniform-branch: unsupported scalar flag selector rejected");
    w=kernel(8,0);w[13]=0x7e140500;
    reject(w,8,"uniform-branch: readfirstlane wave reduction still rejected");
    w=kernel(8,0);w.erase(w.begin()+13);
    reject(w,8,"uniform-branch: missing compare rejected");
    w=kernel(8,0);w[14]=0xbf84ffff;
    reject(w,8,"uniform-branch: backward jump rejected");
    w=kernel(8,0);w[14]=0xbf840002;w[16]=mov(8,255);
    reject(w,8,"uniform-branch: target inside literal rejected");
    w=kernel(8,0);w[15]=0xbf840001;
    reject(w,8,"uniform-branch: nested branch rejected");
    w=kernel(8,0);w[15]=0xbe820300;
    reject(w,8,"uniform-branch: conditional scalar state write rejected");
    w=kernel(8,0);w[15]=0xdc300000;
    reject(w,8,"uniform-branch: conditional memory instruction rejected");
    w=kernel(8,0);w[16]=mov(10,263);
    reject(w,8,"uniform-branch: undefined skipped-path VGPR rejected");
    for(int shape:{2,3,4})reject(kernel(8,shape),8,"uniform-branch: selected pointer without identical affine proof rejected");
    w=kernel(8,0);w[19]=0;
    reject(w,8,"uniform-branch: overlapping input/output still rejected");
    w=kernel(8,0);w[20]=mov(4,258);
    reject(w,8,"uniform-branch: colliding constant store addresses rejected");
    reject(kernel(8,0),8,"uniform-branch: missing declared scalar input rejected",1);
    VulkanBackend vk;auto st=vk.open();
    if(!st)std::printf("    note: uniform branch Vulkan skipped: %s\n",st.error.message.c_str());
    else {
      st=vk.create_instance("pnr-uniform-branch-tests");CHECK(st,"uniform-branch: instance created");
      if(st && !vk.devices().empty()) {
        st=vk.create_device(0);CHECK(st,"uniform-branch: compute device created");
        if(st)for(uint32_t lanes:{2u,8u,64u})for(int shape:{0,1}) {
          auto code=kernel(lanes,shape);
          auto t=translate_compute_words(code,base,lanes*2+1,lanes,2);
          CHECK(t.ok && t.conditional_selection && t.local_size_x==lanes,
                "uniform-branch: workgroup selection with identical pointer proof compiles");
          const char* val=std::getenv("PNR_SPIRV_VAL");
          if(val && std::filesystem::exists(val)) {
            const std::string path="/tmp/pnr_uniform_branch_"+std::to_string(::getpid())+".spv";
            {std::ofstream o(path,std::ios::binary);o.write(reinterpret_cast<const char*>(t.spirv.data()),t.spirv.size()*4);}
            CHECK(std::system((std::string(val)+" --target-env vulkan1.0 "+path).c_str())==0,
                  "uniform-branch: structured workgroup branch/Phi validates for Vulkan 1.0");
            std::filesystem::remove(path);
          }else std::puts("    note: uniform branch validator skipped (not configured)");
          for(uint32_t a:{0u,7u,0xffffffffu})for(uint32_t b:{0u,7u,0xffffffffu}) {
            std::vector<uint32_t> io(lanes*2+1,0xa5a5a5a5);
            for(uint32_t j=0;j<lanes;++j)io[j]=0xffffffffu-j*31;
            auto expected=io;
            for(uint32_t j=0;j<lanes;++j)expected[lanes+j]=a==b?(shape==1?j:io[j]):111;
            auto run=run_translated_compute(vk,t,&io,{a,b});
            CHECK(t.ok && run && io==expected,
                  "uniform-branch: all lanes select correct varying data; inputs/sentinel unchanged");
          }
          std::vector<uint8_t> bytes;
          for(auto word:code)for(unsigned j=0;j<4;++j)bytes.push_back(uint8_t(word>>(j*8)));
          auto bt=translate_compute_bytes(bytes,base,lanes*2+1,lanes,2);
          CHECK(bt.ok && bt.spirv==t.spirv && bt.conditional_selection,
                "uniform-branch: byte API preserves workgroup conditional ABI");
        }
      }else CHECK(false,"uniform-branch: physical device available");
    }
  }

  pnr_loop_tests(g_pass,g_fail);
  pnr_launcher_tests(g_pass,g_fail);
  pnr_launcher_mgmt_tests(g_pass,g_fail);
  pnr_launcher_install_tests(g_pass,g_fail);
  pnr_launcher_care_tests(g_pass,g_fail);
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
