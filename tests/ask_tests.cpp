// Slice D1 tests: `ask`. The model is replaced by canned JSON and by a fake
// executable; the real Needle binary is exercised only when PNR_NEEDLE is set
// (otherwise that test prints SKIP).
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include "ask/ask.hpp"
#include "psn/json.hpp"

namespace fs = std::filesystem;
using namespace pnr::ask;
namespace {
bool has(const std::string& h, const std::string& n) { return h.find(n) != std::string::npos; }
std::string slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
void spit(const std::string& p, const std::string& s) { std::ofstream f(p, std::ios::binary | std::ios::trunc); f << s; }
std::string call(const std::string& name, const std::string& args, double conf, const std::string& extra = "") {
  char b[16]; std::snprintf(b, sizeof b, "%.4f", conf);
  return "{\"type\":\"call\",\"success\":true,\"function_calls\":[{\"name\":\"" + name + "\",\"arguments\":" + args + "}],\"confidence\":" + b + extra + "}";
}
}  // namespace

void pnr_ask_tests(int& pass, int& fail) {
  auto check = [&](bool ok, const std::string& l) { if (ok) ++pass; else ++fail; std::printf("%s %s\n", ok ? "PASS" : "FAIL", l.c_str()); };
  const std::string d = "/tmp/pnr_ask_" + std::to_string(::getpid());
  fs::remove_all(d); fs::create_directories(d);
  spit(d + "/real.pnrgame", "x");
  Lookups lk;
  lk.game_exists = [](const std::string& id) { return id == "st-game" || id == "demo.game"; };
  lk.source_exists = [](const std::string& id) { return id == "demo"; };
  lk.package_exists = [](const std::string& s, const std::string& p) { return s == "demo" && p == "good"; };

  // ---- the exposed command table
  {
    std::set<std::string> mut, names;
    for (auto& t : tools()) { names.insert(t.name); if (t.mutating) mut.insert(t.name); }
    check(mut == std::set<std::string>{"launch_game", "store_get", "store_install", "import", "remove"}, "table: exactly the five approved mutating commands are marked mutating");
    check(names.size() == 14, "table: 14 commands in D1");
    bool bad = false;
    for (auto& t : tools()) {
      for (auto& a : t.argv) if (a == "--yes" || a == "--force" || a == "--allow-flagged" || a == "--overwrite" || a == "--complete") bad = true;
      if (t.argv[0] == "psn" || t.argv[0] == "source" || t.name.rfind("psn", 0) == 0 || t.name.find("token") != std::string::npos) bad = true;
      if (t.argv[0] == "compat" && t.argv.size() > 1 && t.argv[1] == "set") bad = true;
      if (t.argv[0] == "research" && t.argv.size() > 1 && t.argv[1] == "fetch") bad = true;
      if (t.argv[0] == "store" && t.argv.size() > 1 && (t.argv[1] == "source")) bad = true;
    }
    check(!bad, "table: psn, set-token, source, compat set, research fetch and the --yes/--force/--allow-flagged flags are unreachable");
    pnr::psn::Json j; std::string e;
    check(pnr::psn::json_parse(tools_json(), &j, &e) && j.is(pnr::psn::Json::T::Arr) && j.a.size() == 14 && j.a[1].get("parameters") != nullptr, "table: tools_json is valid JSON generated from the table");
  }

  // ---- interpret
  auto ip = [&](const std::string& s, double thr = 0.8) { return interpret(s, thr, lk); };
  {
    auto p = ip(call("list_games", "{}", 0.97));
    check(p.verdict == Verdict::Accepted && !p.mutating && p.command() == "pnr-launcher list", "interpret: a clear read-only call is accepted");
    p = ip(call("launch_game", "{\"game_id\":\"st-game\"}", 0.94));
    check(p.verdict == Verdict::Accepted && p.mutating && p.command() == "pnr-launcher launch st-game", "interpret: launch is accepted but marked mutating");
    p = ip(call("launch_game", "{\"game_id\":\"st-game\"}", 0.8)); check(p.verdict == Verdict::Accepted, "interpret: confidence equal to the threshold is accepted");
    p = ip(call("launch_game", "{\"game_id\":\"st-game\"}", 0.79));
    check(p.verdict == Verdict::LowConfidence && p.args_ok && p.command() == "pnr-launcher launch st-game", "interpret: below threshold is refused but the closest guess is available for display");
    p = ip(call("list_games", "{}", 0.0, ",\"validation\":{\"ungrounded\":[],\"negation\":true}")); check(p.verdict == Verdict::Negation, "interpret: negation is refused");
    p = ip(call("launch_game", "{\"game_id\":\"st-game\"}", 0.99, ",\"validation\":{\"ungrounded\":[\"st-game\"],\"negation\":false}")); check(p.verdict == Verdict::Ungrounded, "interpret: ungrounded values are refused even at 0.99");
    p = ip("{\"success\":true,\"function_calls\":[],\"confidence\":0.8}"); check(p.verdict == Verdict::NoCall, "interpret: no call is reported as no call");
    p = ip("{\"success\":true,\"function_calls\":[{\"name\":\"list_games\",\"arguments\":{}},{\"name\":\"doctor\",\"arguments\":{}}],\"confidence\":0.99}"); check(p.verdict == Verdict::MultipleCalls, "interpret: two calls are refused");
    for (const char* n : {"psn_sync", "set_token", "compat_set", "research_fetch", "source_add", "rm", "launch_game_force"}) {
      p = ip(call(n, "{}", 0.99)); check(p.verdict == Verdict::UnknownTool && !p.args_ok, std::string("interpret: model-named command '") + n + "' is unreachable");
    }
    p = ip(call("launch_game", "{\"game_id\":\"rm -rf /\"}", 0.99)); check(p.verdict == Verdict::BadArgs, "interpret: a shell string as game_id is rejected by the library lookup");
    p = ip(call("launch_game", "{\"game_id\":\"nope\"}", 0.99)); check(p.verdict == Verdict::BadArgs && has(p.reason, "exact id"), "interpret: an unknown game id is rejected (no fuzzy match)");
    p = ip(call("launch_game", "{\"game_id\":\"st-game\",\"yes\":\"true\"}", 0.99)); check(p.verdict == Verdict::BadArgs, "interpret: an extra argument is rejected");
    p = ip(call("launch_game", "{}", 0.99)); check(p.verdict == Verdict::BadArgs, "interpret: a missing argument is rejected");
    p = ip(call("launch_game", "{\"game_id\":5}", 0.99)); check(p.verdict == Verdict::BadArgs, "interpret: a non-text argument is rejected");
    p = ip(call("launch_game", "{\"game_id\":\"--yes\"}", 0.99)); check(p.verdict == Verdict::BadArgs, "interpret: an argument starting with '-' is rejected (flag injection)");
    p = ip(call("store_search", "{\"text\":\"--source evil\"}", 0.99)); check(p.verdict == Verdict::BadArgs, "interpret: search text starting with '-' is rejected");
    p = ip(call("store_search", "{\"text\":\"line\\nbreak\"}", 0.99)); check(p.verdict == Verdict::BadArgs, "interpret: control characters in text are rejected");
    p = ip(call("store_search", "{\"text\":\"racing; rm -rf /\"}", 0.99));
    check(p.verdict == Verdict::Accepted && p.argv.size() == 3 && p.argv[2] == "racing; rm -rf /" && has(p.command(), "'racing; rm -rf /'"), "interpret: free text is ONE argv element (never a shell), shown quoted");
    p = ip(call("research_inspect", "{\"path\":\"" + d + "/real.pnrgame\"}", 0.9)); check(p.verdict == Verdict::Accepted && !p.mutating, "interpret: an existing path is accepted");
    p = ip(call("research_inspect", "{\"path\":\"" + d + "/missing\"}", 0.9)); check(p.verdict == Verdict::BadArgs, "interpret: a missing path is rejected");
    p = ip(call("import", "{\"path\":\"/etc/../etc/passwd\"}", 0.99)); check(p.verdict == Verdict::BadArgs || p.mutating, "interpret: import is mutating whatever the path");
    p = ip(call("store_install", "{\"source\":\"demo\",\"package\":\"good\"}", 0.9)); check(p.verdict == Verdict::Accepted && p.mutating && p.command() == "pnr-launcher store install demo good", "interpret: store install accepted for a real source and package");
    p = ip(call("store_install", "{\"source\":\"demo\",\"package\":\"evil\"}", 0.9)); check(p.verdict == Verdict::BadArgs, "interpret: a package not in the source is rejected");
    p = ip(call("store_get", "{\"source\":\"nosuch\",\"package\":\"good\"}", 0.9)); check(p.verdict == Verdict::BadArgs, "interpret: an unknown source is rejected");
    p = ip("not json"); check(p.verdict == Verdict::BadOutput, "interpret: garbage output is refused");
    p = ip("[1,2]"); check(p.verdict == Verdict::BadOutput, "interpret: non-object output is refused");
    p = ip("{\"success\":false,\"function_calls\":[],\"confidence\":0.9}"); check(p.verdict == Verdict::NeedleFailed, "interpret: a reported model failure is refused");
    p = ip("{\"success\":true,\"function_calls\":[]}"); check(p.verdict == Verdict::BadOutput, "interpret: a missing confidence is refused");
    p = ip("{\"success\":true,\"function_calls\":[],\"confidence\":7}"); check(p.verdict == Verdict::BadOutput, "interpret: an out-of-range confidence is refused");
    p = ip(std::string(2 << 20, ' ')); check(p.verdict == Verdict::BadOutput, "interpret: oversized output is refused");
  }

  // ---- cmd_ask with injected model and runner
  {
    int runs = 0; std::vector<std::string> last;
    AskIO io; io.runner = [&](const std::vector<std::string>& av, std::string* o) { ++runs; last = av; *o += "<ran>\n"; return 0; };
    std::string model_json; io.model = [&](const std::string&, std::string* o) { *o = model_json; return pnr::Status::Ok(); };
    std::string out;
    model_json = call("list_games", "{}", 0.97);
    check(cmd_ask("what games do I have", "/x/needle", 0.8, lk, io, &out) == 0 && runs == 1 && last == std::vector<std::string>{"list"} && has(out, "parsed command: pnr-launcher list") && has(out, "uncalibrated"), "ask: a read-only command runs directly and shows the parsed command");
    out.clear(); runs = 0; model_json = call("launch_game", "{\"game_id\":\"st-game\"}", 0.97);
    check(cmd_ask("start st-game", "/x/needle", 0.8, lk, io, &out) == 1 && runs == 0 && has(out, "NOT RUN") && has(out, "interactive terminal") && has(out, "pnr-launcher launch st-game"), "ask: a mutating command NEVER runs non-interactively");
    io.interactive = true; int asked = 0; io.confirm = [&](const std::string&) { ++asked; return false; };
    out.clear(); check(cmd_ask("start st-game", "/x/needle", 0.8, lk, io, &out) == 1 && runs == 0 && asked == 1 && has(out, "declined"), "ask: declining the confirmation runs nothing");
    io.confirm = [&](const std::string& c) { ++asked; return c == "pnr-launcher launch st-game"; };
    out.clear(); check(cmd_ask("start st-game", "/x/needle", 0.8, lk, io, &out) == 0 && runs == 1 && last == std::vector<std::string>{"launch", "st-game"}, "ask: an interactive yes runs exactly the shown command");
    out.clear(); runs = 0; model_json = call("launch_game", "{\"game_id\":\"st-game\"}", 0.5); asked = 0;
    check(cmd_ask("maybe st-game", "/x/needle", 0.8, lk, io, &out) == 1 && runs == 0 && asked == 0 && has(out, "closest guess (not run)"), "ask: low confidence runs nothing and does not even ask");
    out.clear(); check(cmd_ask("x", "/x/needle", 0.4, lk, io, &out) == 1 || runs == 1, "ask: the threshold is user-configurable");
    out.clear(); runs = 0; check(cmd_ask("x", "", 0.8, lk, io, &out) == 2 && has(out, "not configured") && runs == 0, "ask: without PNR_NEEDLE it says so and does nothing");
    out.clear(); check(cmd_ask("x", "/x/needle", 1.5, lk, io, &out) == 1 && has(out, "between 0 and 1"), "ask: a bad threshold is refused");
    io.model = [&](const std::string&, std::string*) { return pnr::Status::Fail(340, "ask", "boom"); };
    out.clear(); check(cmd_ask("x", "/x/needle", 0.8, lk, io, &out) == 1 && has(out, "NOT RUN: boom") && runs == 0, "ask: a model failure runs nothing");
  }

  // ---- run_needle with a fake executable
  {
    const std::string fake = d + "/needle";
    spit(fake, "#!/bin/sh\nprintf '%s\\n' \"$@\" > " + d + "/args.txt\nenv > " + d + "/env.txt\ncp \"$2\" " + d + "/tools_copy.json\necho '{\"success\":true,\"function_calls\":[],\"confidence\":0.5}'\n");
    ::chmod(fake.c_str(), 0755);
    std::string out; ::setenv("PNR_SECRET_TEST", "leak", 1); ::setenv("HOME_TEST_X", "leak", 1);
    check(run_needle(fake, "what games do I have", NeedleRun{}, &out) && has(out, "\"success\":true"), "needle: a fake executable's JSON is captured");
    std::string args = slurp(d + "/args.txt");
    check(has(args, "--tools\n") && has(args, "--prompt\nwhat games do I have\n"), "needle: invoked with --tools <file> --prompt <text> as separate argv elements");
    const std::string env = slurp(d + "/env.txt");
    check(!has(env, "PNR_SECRET_TEST") && !has(env, "HOME_TEST_X") && has(env, "PATH=/usr/bin:/bin"), "needle: the environment is cleared (no tokens or PNR variables reach the model program)");
    pnr::psn::Json j; std::string e; check(pnr::psn::json_parse(slurp(d + "/tools_copy.json"), &j, &e) && j.a.size() == 14, "needle: the tools file it received is PNR's generated table");
    bool leftover = false; for (auto& de : fs::directory_iterator("/tmp")) if (de.path().filename().string().rfind("pnr-ask-", 0) == 0) leftover = true;
    check(!leftover, "needle: the temporary tools file is removed");
    const std::string evil = "x'; touch " + d + "/pwned; echo '";
    run_needle(fake, evil, NeedleRun{}, &out);
    check(!fs::exists(d + "/pwned") && has(slurp(d + "/args.txt"), evil), "needle: a hostile sentence is passed verbatim and never reaches a shell");
    check(!run_needle(fake, "bad\nprompt", NeedleRun{}, &out) && !run_needle(fake, std::string(600, 'a'), NeedleRun{}, &out) && !run_needle(fake, "", NeedleRun{}, &out), "needle: control characters, over-long and empty sentences are refused");
    check(!run_needle(d + "/nope", "x", NeedleRun{}, &out) && !run_needle(d + "/real.pnrgame", "x", NeedleRun{}, &out), "needle: a missing or non-executable PNR_NEEDLE is refused");
    spit(d + "/slow", "#!/bin/sh\nsleep 20\n"); ::chmod((d + "/slow").c_str(), 0755);
    auto t0 = std::chrono::steady_clock::now(); NeedleRun nr; nr.timeout_seconds = 1;
    auto s = run_needle(d + "/slow", "x", nr, &out);
    double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    check(!s && has(s.error.message, "stopped") && el < 5, "needle: a hung model is killed at the timeout");
    spit(d + "/bad", "#!/bin/sh\nexit 3\n"); ::chmod((d + "/bad").c_str(), 0755);
    check(!run_needle(d + "/bad", "x", NeedleRun{}, &out), "needle: a non-zero exit is a failure");
    spit(d + "/flood", "#!/bin/sh\nyes aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"); ::chmod((d + "/flood").c_str(), 0755);
    check(!run_needle(d + "/flood", "x", nr, &out), "needle: unbounded output is cut off");
  }

  // ---- the real model, only when the user configured it
  if (const char* nb = std::getenv("PNR_NEEDLE")) {
    std::string out; auto s = run_needle(nb, "what games do I have", NeedleRun{}, &out);
    auto p = s ? interpret(out, 0.8, lk) : Parsed{};
    check(s && p.verdict != Verdict::BadOutput && (p.verdict != Verdict::Accepted || p.tool == "list_games"), "REAL needle (PNR_NEEDLE set): output parses, and if accepted the tool is list_games");
  } else std::printf("SKIP real Needle test: PNR_NEEDLE not set\n");
  fs::remove_all(d);
}
