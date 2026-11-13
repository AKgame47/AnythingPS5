// `pnr-launcher ask` (slice D1, EXPERIMENTAL). GPL-2.0.
//
// An optional natural-language front door. A small external model (Cactus
// "Needle 2", Apache-2.0, run as a SEPARATE executable the user installs and
// points PNR_NEEDLE at) turns a sentence into one tool call. PNR then treats
// that call as UNTRUSTED TEXT: it re-validates every argument against its own
// state, shows the parsed command, and only runs read-only commands directly.
// Mutating or launching commands need an interactive yes. The model plays no
// part in any security guarantee, and its confidence is not a safety measure
// (it was observed wrong at 0.53 and 0.30). Nothing is vendored or linked.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "core/runtime.hpp"

namespace pnr::ask {

struct ToolParam { std::string name, description; };
struct ToolDef {
  std::string name, description;
  std::vector<ToolParam> params;       // all string, all required
  std::vector<std::string> argv;       // PNR argv template; "{name}" is replaced by the validated argument
  bool mutating = false;               // launches, installs, imports or removes: always confirmed
};
// The ONLY commands the model can reach. psn, set-token, source add/remove/pin,
// compat set, research fetch, and the flags --yes, --force, --allow-flagged,
// --overwrite are deliberately absent.
const std::vector<ToolDef>& tools();
std::string tools_json();              // the schema handed to the model, generated from tools()

enum class Verdict { Accepted, BadOutput, NeedleFailed, Negation, Ungrounded, MultipleCalls, NoCall, UnknownTool, LowConfidence, BadArgs };
const char* verdict_name(Verdict v);

// How PNR checks model-supplied identifiers against its own state.
struct Lookups {
  std::function<bool(const std::string&)> game_exists;
  std::function<bool(const std::string&)> source_exists;
  std::function<bool(const std::string&, const std::string&)> package_exists;
};

struct Parsed {
  Verdict verdict = Verdict::BadOutput;
  std::string reason, tool;
  double confidence = 0;
  bool mutating = false;
  std::vector<std::string> argv;       // valid only when args_ok
  bool args_ok = false;
  std::string command() const;         // "pnr-launcher launch st-game", shell-quoted for display only
};
// Pure function of the model's JSON text: no I/O besides the lookups.
Parsed interpret(const std::string& needle_json, double threshold, const Lookups& lk);

struct NeedleRun {
  int timeout_seconds = 30;
  // D2: run the model program inside PNR's namespace + pivot_root sandbox (no network, no
  // access to the user's files). It sees a private copy of the executable and the tools
  // file only. No silent fallback: if the sandbox cannot be entered the model is not run.
  bool sandbox = false;
};
// Runs `needle --tools <tmp> --prompt <text>` with no shell, a cleared
// environment, stdin from /dev/null, CPU/memory limits and a wall timeout.
Status run_needle(const std::string& needle_path, const std::string& prompt, const NeedleRun& opt, std::string* json_out);

struct AskIO {
  bool sandbox_model = true;                                         // D2: run the model program in PNR's sandbox (default); false only via --no-model-sandbox
  bool interactive = false;                                          // stdin and stdout are a terminal
  std::function<bool(const std::string&)> confirm;                   // shown the command; true = run
  std::function<int(const std::vector<std::string>&, std::string*)> runner;  // runs a PNR argv, returns its exit code, appends output
  std::function<Status(const std::string&, std::string*)> model;     // prompt -> needle JSON (default: run_needle)
};
// Returns the exit code. 0 = understood and (if applicable) run; 1 = refused or not understood; 2 = not configured.
int cmd_ask(const std::string& prompt, const std::string& needle_path, double threshold, const Lookups& lk, AskIO& io, std::string* out);

}  // namespace pnr::ask
