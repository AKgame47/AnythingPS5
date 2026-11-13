// PNR unit tests - no framework dependency, explicit pass/fail counts.
// Never fabricated: a failure prints and fails the binary.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>

#include "compat/services.hpp"
#include "core/runtime.hpp"
#include "library/game_db.hpp"
#include "loader/native_loader.hpp"
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
  char buf[4096];
  ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return {};
  buf[n] = '\0';
  return buf;
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
    const std::string root = "/tmp/pnr_import_root";
    std::remove((root + "/library.db").c_str());
    pnr::library::GameDb db;
    CHECK(db.open(root + "/library.db"), "import: db open");

    pnr::pkg::Manifest m;
    m.id = "selftest-game";
    m.title = "Selftest Game";
    m.version = "1.0.0";
    m.executable = "bin/game";
    auto st = pnr::pkg::build_package(
        "/tmp/pnr_selftest.pnrgame", m,
        {{"bin/game", self_exe()}, {"assets/note.txt", "/etc/hostname"}});
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

  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
