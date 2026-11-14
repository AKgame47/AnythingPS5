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
//   {"call":"library_list"}                        needs library_read; returns the user's games (id, title) as a JSON string
//   {"call":"store_add_source","id":"..","url":"https://..","rights":".."}   needs store_add_source; adds a source with a rights
//                                                  statement marked as declared by the plugin; fetches nothing
//   {"call":"download_request","source":"..","pkg":".."}   needs download_request; QUEUES a package from an existing store
//                                                  source (unsupported kinds refused); it never downloads or installs
//   {"call":"result","value":"..."}                ends the session; the value is returned to the caller
// Any other call, or a call whose permission was not granted, is answered {"ok":false,"error":"..."} and counted.
// Limits: wall-clock timeout, total output cap, request cap. Implemented, Tested on synthetic plugins (Linux x86-64).
// Library, store and download calls are Implemented through BrokerHost (P4). Windows: Blocked.
#pragma once
#include <string>
#include <vector>

#include "plugin/manager.hpp"

namespace pnr::plugin {

// What the broker can reach on the plugin's behalf. Every method is called ONLY after the matching permission check passed.
// No host means those calls are refused with "not available". Implementations must not trust any argument.
struct BrokerHost {
  virtual ~BrokerHost() = default;
  virtual bool library_list(std::string* json, std::string* err) = 0;                 // permission library_read
  virtual bool store_add_source(const std::string& id, const std::string& url, const std::string& rights, std::string* err) = 0;  // store_add_source
  virtual bool download_request(const std::string& source, const std::string& pkg, std::string* err) = 0;  // download_request: queue only
};

struct RunOptions {
  BrokerHost* host = nullptr;
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
