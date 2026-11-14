// PNR plugin system - isolated execution + permission broker (slice P2), GPL-2.0.
// Runs an ENABLED plugin whose entry type is "process" inside the PNR Linux sandbox (PARTIAL isolation: user, net,
// pid, ipc, uts namespaces, seccomp denylist, pivot_root filesystem; see sandbox/sandbox.hpp for what is NOT
// covered). Not escape-proof; do not run code you do not trust on the strength of this alone.
// There is no silent fallback: if the sandbox is unusable the plugin is NOT started.
//
// Broker protocol: the plugin writes one JSON object per line on stdout, the host answers one JSON line on stdin.
//   {"call":"log","msg":"..."}                     always allowed, message is stored (capped)
//   {"call":"data_put","name":"n","value":"..."}   needs granted fs_write_plugin_data
//   {"call":"data_get","name":"n"}                 needs granted fs_write_plugin_data
//   {"call":"result","value":"..."}                ends the session; the value is returned to the caller
// Any other call, or a call whose permission was not granted, is answered {"ok":false,"error":"..."} and counted.
// Limits: wall-clock timeout, total output cap, request cap. Implemented, Tested on synthetic plugins (Linux x86-64).
// Library, store and download calls are Planned (P4). Windows: Blocked.
#pragma once
#include <string>
#include <vector>

#include "plugin/manager.hpp"

namespace pnr::plugin {

struct RunOptions {
  int timeout_ms = 10000;
  std::size_t max_output_bytes = 1u << 20;
  int max_requests = 500;
};

struct RunResult {
  bool started = false;            // the plugin process was launched in the sandbox
  bool completed = false;          // it sent a "result" call
  std::string result;              // value of that call
  std::string error;               // plain words when not started / not completed
  std::vector<std::string> logs;
  int denied = 0;                  // requests the broker refused
  int served = 0;
  bool timed_out = false;
  int exit_status = -1;
};

// home is the same root the PluginManager uses. Plugin data lives in <home>/plugins/data/<id>/.
RunResult run_plugin(const InstalledPlugin& p, const std::string& home, const RunOptions& opt = {});

}  // namespace pnr::plugin
