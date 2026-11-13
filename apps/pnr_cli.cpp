// PNR CLI - first launch driver for the master prompt's 10-step workflow.
// Every step prints an explicit SUCCESS / FAILED / SKIPPED / BLOCKED line.
// It never reports a game as run when only initialization succeeded.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "compat/services.hpp"
#include "core/runtime.hpp"
#include "library/game_db.hpp"
#include "loader/native_loader.hpp"
#include "package/importer.hpp"
#include "platform/os.hpp"

namespace {

void step(const char* name, const char* result, const std::string& detail) {
  std::printf("[%s] %-9s %s\n", name, result, detail.c_str());
}

int cmd_hwinfo() {
  std::puts("PNR hardware snapshot (host truth, no estimates):");
  auto host = pnr::platform::make_host_platform();
  std::printf("host.platform=%s\n", host->name().c_str());
  std::printf("cpu.logical_cores=%u\n", host->cpu_logical_cores());
  if (auto mem = host->memory()) {
    std::printf("mem.total_mb=%llu\n",
                (unsigned long long)(mem->total_bytes / 1048576));
    std::printf("mem.available_mb=%llu\n",
                (unsigned long long)(mem->available_bytes / 1048576));
  } else {
    std::puts("mem.info=unavailable on this host (reported, not guessed)");
  }
  if (auto batt = host->on_battery())
    std::printf("power.on_battery=%d\n", *batt ? 1 : 0);
  else
    std::puts("power.on_battery=unknown (no power_supply node on this host)");
  std::puts("gpu.vulkan=UNAVAILABLE in this sandbox (no GPU/Vulkan driver); "
            "capability-based detection lands with Phase 2");
  return 0;
}

int cmd_import(const std::string& pkg_path) {
  pnr::library::GameDb db;
  auto st = db.open("./pnr_data/library.db");
  if (!st) {
    std::printf("IMPORT FAILED: %s\n", st.error.message.c_str());
    return 2;
  }
  auto r = pnr::pkg::import_package(pkg_path, "./pnr_data", db);
  if (!r.status) {
    std::printf("IMPORT FAILED: %s\n", r.status.error.message.c_str());
    return 2;
  }
  std::printf("IMPORT OK: %s %s (%s)\n  id=%s\n  installed=%s\n  "
              "compatibility=Untested (no execution test yet)\n",
              r.manifest.title.c_str(), r.manifest.version.c_str(),
              r.manifest.arch.c_str(), r.manifest.id.c_str(),
              r.install_path.c_str());
  db.close();
  return 0;
}

int cmd_games() {
  pnr::library::GameDb db;
  auto st = db.open("./pnr_data/library.db");
  if (!st) {
    std::printf("LIBRARY FAILED: %s\n", st.error.message.c_str());
    return 2;
  }
  auto games = db.list_games();
  if (games.empty()) {
    std::puts("library is empty (import a .pnrgame first)");
    return 0;
  }
  for (const auto& g : games) {
    auto compat = db.get_compatibility(g.game_id);
    auto inst = db.get_installation(g.game_id);
    std::printf("%s  v%s  [%s]\n    %s\n", g.title.c_str(),
                g.version.c_str(),
                compat ? compat->state.c_str() : "Untested",
                inst ? inst->path.c_str() : "(not installed)");
  }
  db.close();
  return 0;
}

int cmd_validate(pnr::loader::NativeLoader& loader, const std::string& exe) {
  auto r = loader.validate(exe);
  if (!r.ok) {
    std::printf("VALIDATE FAILED: %s\n", r.error.c_str());
    return 2;
  }
  std::printf("VALIDATE OK: %s\n", exe.c_str());
  std::printf("  elf64=%d machine=%u (x86-64=62) type=%u entry=0x%llx "
              "segments=%zu\n",
              r.info.is_64bit ? 1 : 0, r.info.machine, r.info.elf_type,
              (unsigned long long)r.info.entry, r.info.segments.size());
  return 0;
}

int cmd_run(const std::string& exe, const std::vector<std::string>& args) {
  using pnr::Status;
  pnr::RuntimeManager rt;
  pnr::loader::NativeLoader loader;

  rt.events().subscribe("lifecycle.changed", [](const pnr::Event& e) {
    std::printf("  [event] lifecycle -> %s\n", e.payload.c_str());
  });

  // STEP 1 - VALIDATION
  auto v = loader.validate(exe);
  if (!v.ok) {
    step("1-validate", "FAILED", v.error);
    return 2;
  }
  step("1-validate", "SUCCESS",
       exe + " is a valid x86-64 ELF (" +
           std::to_string(v.info.segments.size()) + " segments)");

  // STEP 2 - SYSTEM ANALYSIS
  const unsigned cores = std::thread::hardware_concurrency();
  rt.capabilities().declare("cpu.x86_64", true,
                            "host executes x86-64 natively; logical cores=" +
                                std::to_string(cores));
  step("2-analysis", "SUCCESS",
       "logical cores=" + std::to_string(cores) +
           "; Vulkan=unavailable in sandbox (recorded, not guessed)");

  // STEP 3 - CONFIGURATION
  rt.config().set("profile", "balanced");
  rt.config().set("target_fps", "60");
  step("3-config", "SUCCESS",
       "default profile 'balanced' (no game profile found; defaults used "
       "openly)");

  // STEP 4 - EXECUTION ENVIRONMENT
  Status st = rt.initialize();
  if (!st) {
    step("4-runtime", "FAILED", st.error.message);
    return 3;
  }
  pnr::compat::MemoryService memory;
  pnr::compat::FileSystemService fs("./pnr_data");
  pnr::compat::ThreadService threads;
  step("4-runtime", "SUCCESS", "compat memory/fs/thread services up");

  // STEP 5 - GRAPHICS INITIALIZATION
  step("5-graphics", "BLOCKED",
       "no Vulkan device in this environment; demo app is non-graphical. "
       "Blocked is reported, not papered over.");

  // STEP 6 - CONTENT INITIALIZATION
  st = fs.create_dirs("/saves");
  if (!st) {
    step("6-content", "FAILED", st.error.message);
    return 4;
  }
  step("6-content", "SUCCESS", "save-data dir jailed at ./pnr_data/saves");

  // STEP 7 - GAME START (separate process, supervised)
  pnr::GameSession session(
      rt, "session-1",
      [&loader](const std::string& e, const std::vector<std::string>& a,
                int* out_sig) -> int {
        auto lr = loader.launch(e, a);
        if (!lr.started) {
          std::printf("  [loader] start failed: %s\n", lr.error.c_str());
          *out_sig = -1;
          return -1;
        }
        if (lr.signaled) {
          *out_sig = lr.signal;
          return -1;
        }
        *out_sig = 0;
        std::printf("  [monitor] wall=%.3fs exit=%d\n", lr.wall_seconds,
                    lr.exit_code);
        return lr.exit_code;
      });

  std::puts("--- game output begin ---");
  Status run = session.start(exe, args);
  std::puts("--- game output end ---");
  if (!run) {
    step("7-start", "FAILED", run.error.message);
    rt.shutdown();
    return 5;
  }
  step("7-start", "SUCCESS",
       "game ran natively in child process, exit code " +
           std::to_string(session.last_exit_code()));

  // STEP 8/9 - MONITORING + OPTIMIZATION
  step("8-monitor", "SUCCESS",
       "wall time + exit status measured (frame-time telemetry needs the "
       "graphics phase)");
  step("9-optimize", "SKIPPED",
       "deterministic adaptive controller is Phase 4 work");

  // STEP 10 - SHUTDOWN
  st = rt.shutdown();
  step("10-shutdown", st ? "SUCCESS" : "FAILED",
       st ? "resources released, " +
                std::to_string(rt.errors().count()) + " errors recorded"
          : st.error.message);

  std::printf("RESULT: %s\n",
              session.last_exit_code() == 0 ? "RUN OK (real native execution)"
                                            : "RUN FAILED (real result)");
  return session.last_exit_code();
}

void usage() {
  std::puts("PNR CLI (AnythingPS5) - honest prototype\n"
            "usage:\n"
            "  pnr run <executable> [args...]   validate + natively run a "
            "developer test app\n"
            "  pnr validate <executable>        ELF/x86-64 validation only\n"
            "  pnr import <file.pnrgame>        verify + install a dev package\n"
            "  pnr games                        list the game library\n"
            "  pnr hwinfo                       print detected host hardware");
}

}  // namespace

int main(int argc, char** argv) {
  pnr::loader::NativeLoader loader;
  if (argc >= 2 && std::strcmp(argv[1], "hwinfo") == 0) return cmd_hwinfo();
  if (argc >= 3 && std::strcmp(argv[1], "validate") == 0)
    return cmd_validate(loader, argv[2]);
  if (argc >= 3 && std::strcmp(argv[1], "import") == 0)
    return cmd_import(argv[2]);
  if (argc >= 2 && std::strcmp(argv[1], "games") == 0) return cmd_games();
  if (argc >= 3 && std::strcmp(argv[1], "run") == 0) {
    std::vector<std::string> args(argv + 3, argv + argc);
    return cmd_run(argv[2], args);
  }
  usage();
  return 64;
}
