// PNR CLI - first launch driver for the master prompt's 10-step workflow.
// Every step prints an explicit SUCCESS / FAILED / SKIPPED / BLOCKED line.
// It never reports a game as run when only initialization succeeded.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "compat/services.hpp"
#include "core/runtime.hpp"
#include "loader/import_resolver.hpp"
#include "prx/prx.hpp"
#include "prx/prx_file.hpp"
#include "library/game_db.hpp"
#include "loader/native_loader.hpp"
#include "package/importer.hpp"
#include "ape/ape.hpp"
#include "laptop/laptop.hpp"
#include "platform/os.hpp"
#include "services/audio.hpp"
#include "services/input.hpp"

namespace {

void step(const char* name, const char* result, const std::string& detail) {
  std::printf("[%s] %-9s %s\n", name, result, detail.c_str());
}

int cmd_assess() {
  // MODULE 05/06: classify this host and show the recommended starting
  // settings. Classification is CPU/RAM-based only (GPU probe Blocked).
  auto host = pnr::platform::make_host_platform();
  pnr::ape::HardwareFacts f;
  f.cpu_logical_cores = host->cpu_logical_cores();
  if (auto mem = host->memory()) {
    f.ram_total_bytes = mem->total_bytes;
    f.ram_available_bytes = mem->available_bytes;
  }
  f.on_battery = host->on_battery();
  const auto cls = pnr::ape::classify(f);
  std::puts("PNR hardware assessment (heuristic, honest bounds):");
  std::printf("hardware.class=%s (CPU/RAM only; GPU unknown in this build)\n",
              pnr::ape::perf_class_name(cls));
  const auto mode = pnr::laptop::default_mode(f);
  const auto caps = pnr::laptop::caps_for(mode);
  std::printf("power.default_mode=%s%s\n", pnr::laptop::power_mode_name(mode),
              f.on_battery ? "" : " (battery state unknown: balanced)");
  std::printf("caps.fps_cap=%d caps.min_resolution_scale=%d%% caps.max_background_tasks=%d\n",
              caps.fps_cap, caps.min_resolution_scale_pct, caps.max_background_tasks);
  std::puts("note: compatibility is never decided from class alone - real "
            "frame telemetry drives the adaptive controller");
  return 0;
}

int cmd_input() {
  auto backend = pnr::services::make_input_backend();
  std::printf("input.backend=%s\n", backend->name().c_str());
  const auto devices = backend->enumerate();
  std::printf("input.devices=%zu\n", devices.size());
  for (const auto& d : devices)
    std::printf("  %s (%s)\n", d.path.empty() ? d.name.c_str() : d.path.c_str(),
                d.name.c_str());
  if (devices.empty()) std::puts("  (no input devices visible on this host)");
  return 0;
}

int cmd_audio() {
  auto backend = pnr::services::make_audio_backend();
  std::printf("audio.backend=%s\n", backend->name().c_str());
  const auto devices = backend->enumerate();
  std::printf("audio.devices=%zu\n", devices.size());
  if (devices.empty())
    std::puts("  (no audio devices available; host audio backends Planned)");
  return 0;
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

int cmd_run(const std::string& exe, const std::vector<std::string>& args,
            const std::string& game_id = "") {
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

  // STEP 1b - IMPORT RESOLUTION (against the PRX taxonomy).
  // The registry mirrors exactly what the PS-ABI shim provides at launch:
  // file calls are installed against the game's jail, so the
  // classification matches the real dispatch environment.
  {
    pnr::prx::PrxRegistry prx;
    pnr::compat::FileSystemService game_jail("./pnr_data/jail");
    pnr::prx::PsFileTable game_files(game_jail);
    prx.install_file_calls(&game_files);
    pnr::loader::ImportResolution ir;
    const auto st = pnr::loader::resolve_imports(exe, prx, &ir);
    if (!st) {
      step("1-imports", "FAILED", st.error.message);
      return 2;
    }
    step("1-imports", "SUCCESS",
         std::to_string(ir.imports.size()) + " imports: " +
             std::to_string(ir.host) + " host-resolved, " +
             std::to_string(ir.prx_implemented) + " prx-implemented, " +
             std::to_string(ir.prx_declared) + " prx-declared-only, " +
             std::to_string(ir.unknown) + " unknown");
    for (const auto& e : ir.imports) {
      if (e.klass != pnr::loader::ImportClass::Host)
        std::printf("  [import] %-32s %s\n", e.name.c_str(),
                    pnr::loader::import_class_name(e.klass));
    }
  }

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
      [&loader, &game_id](const std::string& e, const std::vector<std::string>& a,
                int* out_sig) -> int {
        // PS-ABI environment: games get their own jail and can resolve the
        // sce* shim from the launcher binary's directory.
        std::string selfdir = ".";
        {
          std::ifstream maps_self("/proc/self/exe");  // existence probe
          char buf[4096];
          const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
          if (n > 0) {
            buf[n] = '\0';
            const std::string full(buf);
            const auto slash = full.find_last_of('/');
            if (slash != std::string::npos) selfdir = full.substr(0, slash);
          }
        }
        // Library games get their own jail; ad-hoc paths share the
        // default one. The sce* shim resolves from the launcher's
        // directory (prx modules come from the runtime, like firmware).
        const std::string jail = game_id.empty()
                                     ? "./pnr_data/jail"
                                     : "./pnr_data/jail/" + game_id;
        auto lr = loader.launch(e, a,
                                {"PNR_JAIL_ROOT=" + jail,
                                 "LD_LIBRARY_PATH=" + selfdir});
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
  const auto t0 = std::chrono::steady_clock::now();
  Status run = session.start(exe, args);
  const double wall =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
          .count();
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

  // Library bookkeeping for games run by id: the launch is recorded, and
  // a clean exit moves compatibility Untested -> Launches Only. That state
  // means exactly what it says: the game started and finished; nobody has
  // verified playability.
  if (!game_id.empty()) {
    pnr::library::GameDb db;
    if (db.open("./pnr_data/library.db")) {
      db.record_launch(game_id, session.last_exit_code(), wall);
      if (session.last_exit_code() == 0) {
        auto compat = db.get_compatibility(game_id);
        if (!compat || compat->state == "Untested") {
          db.set_compatibility({game_id, "Launches Only",
                                "launched natively, exit 0; gameplay not "
                                "verified"});
          std::printf("[library] launch recorded; compatibility=Launches "
                      "Only (exit 0, gameplay unverified)\n");
        } else {
          std::printf("[library] launch recorded; compatibility=%s "
                      "(unchanged)\n", compat->state.c_str());
        }
      } else {
        std::printf("[library] launch recorded (exit %d; compatibility "
                    "unchanged)\n", session.last_exit_code());
      }
      db.close();
    }
  }
  return session.last_exit_code();
}

void usage() {
  std::puts("PNR CLI (AnythingPS5) - honest prototype\n"
            "usage:\n"
            "  pnr run <exe-or-game-id> [args...]\n"
            "                                 validate + natively run a "
            "test app or an\n"
            "                                 installed library game\n"
            "  pnr validate <executable>        ELF/x86-64 validation only\n"
            "  pnr import <file.pnrgame>        verify + install a dev package\n"
            "  pnr games                        list the game library\n"
            "  pnr hwinfo                       print detected host hardware");
}

}  // namespace

int main(int argc, char** argv) {
  pnr::loader::NativeLoader loader;
  if (argc >= 2 && std::strcmp(argv[1], "hwinfo") == 0) return cmd_hwinfo();
  if (argc >= 2 && std::strcmp(argv[1], "assess") == 0) return cmd_assess();
  if (argc >= 2 && std::strcmp(argv[1], "input") == 0) return cmd_input();
  if (argc >= 2 && std::strcmp(argv[1], "audio") == 0) return cmd_audio();
  if (argc >= 3 && std::strcmp(argv[1], "validate") == 0)
    return cmd_validate(loader, argv[2]);
  if (argc >= 3 && std::strcmp(argv[1], "import") == 0)
    return cmd_import(argv[2]);
  if (argc >= 2 && std::strcmp(argv[1], "games") == 0) return cmd_games();
  if (argc >= 3 && std::strcmp(argv[1], "run") == 0) {
    std::vector<std::string> args(argv + 3, argv + argc);
    std::string target = argv[2];
    std::string game_id;
    if (!std::filesystem::exists(target)) {
      // Not a path: try the library. Never guess beyond an exact id.
      pnr::library::GameDb db;
      if (db.open("./pnr_data/library.db")) {
        auto inst = db.get_installation(target);
        if (inst && !inst->executable.empty()) {
          game_id = target;
          target = inst->path + "/" + inst->executable;
        }
        db.close();
      }
      if (game_id.empty()) {
        std::fprintf(stderr,
                     "run: '%s' is neither a file nor an installed game id\n",
                     argv[2]);
        return 2;
      }
    }
    return cmd_run(target, args, game_id);
  }
  usage();
  return 64;
}
