// See ask.hpp.
#include "ask/ask.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <filesystem>

#include "psn/json.hpp"
#include "sandbox/sandbox.hpp"

namespace fs = std::filesystem;
namespace pnr::ask {
namespace {
Status fail(const std::string& m) { return Status::Fail(340, "ask", m); }
bool printable(const std::string& s, size_t max) {
  if (s.empty() || s.size() > max) return false;
  for (unsigned char c : s) if (c < 0x20 || c == 0x7f) return false;
  return true;
}
bool id_chars(const std::string& s) {
  if (s.empty() || s.size() > 64) return false;
  for (unsigned char c : s) if (!(std::isalnum(c) || c == '.' || c == '-' || c == '_')) return false;
  return s[0] != '-' && s[0] != '.';
}
std::string sq(const std::string& s) {  // display quoting only; nothing is ever run through a shell
  bool plain = !s.empty(); for (unsigned char c : s) if (!(std::isalnum(c) || std::strchr("._-/=:@+,", c))) plain = false;
  if (plain) return s;
  std::string o = "'"; for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; } return o + "'";
}
}  // namespace

const std::vector<ToolDef>& tools() {
  static const std::vector<ToolDef> t = {
    {"list_games", "List installed games", {}, {"list"}, false},
    {"show_game", "Show details of one installed game", {{"game_id", "exact id of an installed game"}}, {"show", "{game_id}"}, false},
    {"compat_show", "Show the compatibility state of an installed game", {{"game_id", "exact id of an installed game"}}, {"compat", "show", "{game_id}"}, false},
    {"history", "Show the launch history of an installed game", {{"game_id", "exact id of an installed game"}}, {"history", "{game_id}"}, false},
    {"store_search", "Search the store sources for a package by text", {{"text", "words to search for"}}, {"store", "search", "{text}"}, false},
    {"store_show", "Show one package of a store source", {{"source", "store source id"}, {"package", "package id"}}, {"store", "show", "{source}", "{package}"}, false},
    {"research_inspect", "Inspect a local file read-only", {{"path", "path of an existing file"}}, {"research", "inspect", "{path}"}, false},
    {"sandbox_probe", "Check whether the sandbox works on this machine", {}, {"sandbox", "probe"}, false},
    {"doctor", "Read-only health report of the library", {}, {"doctor"}, false},
    {"suggest_manifest", "Print a draft package manifest for a directory (read-only, writes nothing)", {{"dir", "path of an existing directory"}}, {"manifest", "suggest", "{dir}"}, false},
    {"launch_game", "Launch an installed game by its id", {{"game_id", "exact id of an installed game"}}, {"launch", "{game_id}"}, true},
    {"store_get", "Queue a store package for download", {{"source", "store source id"}, {"package", "package id"}}, {"store", "get", "{source}", "{package}"}, true},
    {"store_install", "Install a downloaded store package", {{"source", "store source id"}, {"package", "package id"}}, {"store", "install", "{source}", "{package}"}, true},
    {"import", "Import a .pnrgame file into the library", {{"path", "path of an existing .pnrgame file"}}, {"import", "{path}"}, true},
    {"remove", "Remove an installed game (without --yes this is a dry run)", {{"game_id", "exact id of an installed game"}}, {"remove", "{game_id}"}, true},
  };
  return t;
}

static std::string jesc(const std::string& s) {
  std::string o; for (unsigned char c : s) { if (c == '"') o += "\\\""; else if (c == '\\') o += "\\\\"; else if (c < 0x20) o += ' '; else o += static_cast<char>(c); } return o;
}
std::string tools_json() {
  std::string o = "[";
  bool first = true;
  for (const auto& t : tools()) {
    if (!first) o += ",";
    first = false;
    o += "{\"name\":\"" + t.name + "\",\"description\":\"" + jesc(t.description) + "\",\"parameters\":{\"type\":\"object\",\"properties\":{";
    for (size_t i = 0; i < t.params.size(); ++i) o += (i ? "," : "") + std::string("\"") + t.params[i].name + "\":{\"type\":\"string\",\"description\":\"" + jesc(t.params[i].description) + "\"}";
    o += "},\"required\":[";
    for (size_t i = 0; i < t.params.size(); ++i) o += (i ? "," : "") + std::string("\"") + t.params[i].name + "\"";
    o += "]}}";
  }
  return o + "]";
}

const char* verdict_name(Verdict v) {
  switch (v) {
    case Verdict::Accepted: return "accepted"; case Verdict::BadOutput: return "unreadable model output"; case Verdict::NeedleFailed: return "model reported failure";
    case Verdict::Negation: return "negation detected"; case Verdict::Ungrounded: return "ungrounded values"; case Verdict::MultipleCalls: return "more than one call";
    case Verdict::NoCall: return "no call"; case Verdict::UnknownTool: return "unknown tool"; case Verdict::LowConfidence: return "confidence below threshold"; case Verdict::BadArgs: return "arguments rejected";
  }
  return "?";
}

std::string Parsed::command() const {
  std::string c = "pnr-launcher";
  for (const auto& a : argv) c += " " + sq(a);
  return c;
}

Parsed interpret(const std::string& text, double threshold, const Lookups& lk) {
  Parsed p;
  auto done = [&](Verdict v, const std::string& why) { p.verdict = v; p.reason = why; return p; };
  psn::Json j; std::string err;
  if (text.size() > (1u << 20) || !psn::json_parse(text, &j, &err) || !j.is(psn::Json::T::Obj)) return done(Verdict::BadOutput, "the model did not return a JSON object");
  const psn::Json* ok = j.get("success"); const psn::Json* cf = j.get("confidence"); const psn::Json* fc = j.get("function_calls");
  if (!ok || !ok->is(psn::Json::T::Bool)) return done(Verdict::BadOutput, "no success field");
  if (!ok->b) return done(Verdict::NeedleFailed, "the model reported failure");
  if (!cf || !cf->is(psn::Json::T::Num) || !(cf->n >= 0 && cf->n <= 1)) return done(Verdict::BadOutput, "no usable confidence");
  p.confidence = cf->n;
  if (!fc || !fc->is(psn::Json::T::Arr)) return done(Verdict::BadOutput, "no function_calls array");
  if (const psn::Json* v = j.get("validation")) {
    if (const psn::Json* n = v->get("negation")) if (n->is(psn::Json::T::Bool) && n->b) return done(Verdict::Negation, "the sentence contains a negation, so PNR will not guess the opposite");
    if (const psn::Json* u = v->get("ungrounded")) if (u->is(psn::Json::T::Arr) && !u->a.empty()) return done(Verdict::Ungrounded, "the model produced values that are not in your sentence");
  }
  if (fc->a.empty()) return done(Verdict::NoCall, "the model chose no command");
  if (fc->a.size() > 1) return done(Verdict::MultipleCalls, "the model chose several commands; ask for one thing at a time");
  const psn::Json& call = fc->a[0];
  const psn::Json* nm = call.get("name"); const psn::Json* ar = call.get("arguments");
  if (!nm || !nm->is(psn::Json::T::Str) || !ar || !ar->is(psn::Json::T::Obj)) return done(Verdict::BadOutput, "malformed call");
  const ToolDef* def = nullptr; for (const auto& t : tools()) if (t.name == nm->s) def = &t;
  if (!def) return done(Verdict::UnknownTool, "the model named a command PNR does not expose");
  p.tool = def->name; p.mutating = def->mutating;
  // arguments: exactly the declared keys, all strings, validated against PNR's own state
  std::vector<std::pair<std::string, std::string>> vals; std::string bad;
  if (ar->o.size() != def->params.size()) bad = "wrong number of arguments";
  for (const auto& prm : def->params) {
    const psn::Json* v = ar->get(prm.name);
    if (!v || !v->is(psn::Json::T::Str)) { bad = "missing or non-text argument '" + prm.name + "'"; break; }
    const std::string& s = v->s;
    if (!printable(s, 4096)) { bad = "argument '" + prm.name + "' is empty, too long or has control characters"; break; }
    if (s[0] == '-') { bad = "argument '" + prm.name + "' starts with '-' (could be read as a flag)"; break; }
    if (prm.name == "game_id") { if (!id_chars(s) || !lk.game_exists || !lk.game_exists(s)) { bad = "no installed game with that exact id"; break; } }
    else if (prm.name == "source") { if (!id_chars(s) || !lk.source_exists || !lk.source_exists(s)) { bad = "no store source with that exact id"; break; } }
    else if (prm.name == "package") { if (!id_chars(s)) { bad = "not a valid package id"; break; } }
    else if (prm.name == "dir") { std::error_code ec; if (!fs::is_directory(s, ec)) { bad = "no such directory"; break; } }
    else if (prm.name == "path") { std::error_code ec; if (!fs::is_regular_file(s, ec)) { bad = "no such file"; break; } }
    else if (prm.name == "text") { if (s.size() > 120) { bad = "search text too long"; break; } }
    vals.emplace_back(prm.name, s);
  }
  if (bad.empty()) {
    std::string src, pkg; for (auto& v : vals) { if (v.first == "source") src = v.second; if (v.first == "package") pkg = v.second; }
    if (!src.empty() && !pkg.empty() && (!lk.package_exists || !lk.package_exists(src, pkg))) bad = "that source has no package with that exact id";
  }
  if (bad.empty()) {
    for (const auto& a : def->argv) {
      std::string x = a;
      if (a.size() > 2 && a.front() == '{' && a.back() == '}') for (auto& v : vals) if (v.first == a.substr(1, a.size() - 2)) x = v.second;
      p.argv.push_back(x);
    }
    p.args_ok = true;
  }
  if (p.confidence < threshold) return done(Verdict::LowConfidence, "confidence is below your threshold");
  if (!p.args_ok) return done(Verdict::BadArgs, bad);
  return done(Verdict::Accepted, "");
}

Status run_needle(const std::string& needle_path, const std::string& prompt, const NeedleRun& opt, std::string* out) {
  out->clear();
  if (!printable(prompt, 500)) return fail("the question must be 1-500 printable characters");
  std::error_code ec;
  if (!fs::is_regular_file(needle_path, ec) || ::access(needle_path.c_str(), X_OK) != 0) return fail("PNR_NEEDLE does not point to an executable file");
  char tmpl[] = "/tmp/pnr-ask-XXXXXX";   // a private directory holding the tools file (and, in sandbox mode, a copy of the executable)
  if (!::mkdtemp(tmpl)) return fail("cannot create a temporary directory");
  ::chmod(tmpl, 0700);
  struct Rm { std::string p; ~Rm() { std::error_code e; fs::remove_all(p, e); } } rm{tmpl};
  const std::string dir = tmpl, tools_path = dir + "/tools.json";
  {
    const std::string tj = tools_json();
    int tfd = ::open(tools_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    bool wrote = tfd >= 0 && ::write(tfd, tj.data(), tj.size()) == static_cast<ssize_t>(tj.size());
    if (tfd >= 0) ::close(tfd);
    if (!wrote) return fail("cannot write the tools file");
  }
  std::string exe = needle_path;
  if (opt.sandbox) {
    auto sp = pnr::sandbox::probe();
    if (!sp.usable()) return fail("the sandbox for the model program is not usable here (" + sp.notes + "); the model was NOT run");
    exe = dir + "/needle";
    std::error_code cec; fs::copy_file(needle_path, exe, fs::copy_options::overwrite_existing, cec);
    if (cec) return fail("cannot copy the model program into its private directory");
    ::chmod(exe.c_str(), 0500);
  }
  int pipefd[2]; if (::pipe(pipefd) != 0) return fail("pipe failed");
  pid_t pid = ::fork();
  if (pid < 0) { ::close(pipefd[0]); ::close(pipefd[1]); return fail("fork failed"); }
  if (pid == 0) {
    ::dup2(pipefd[1], 1); ::close(pipefd[0]); ::close(pipefd[1]);
    int dn = ::open("/dev/null", O_RDWR); if (dn >= 0) { ::dup2(dn, 0); ::dup2(dn, 2); }
    struct rlimit cpu{static_cast<rlim_t>(opt.timeout_seconds), static_cast<rlim_t>(opt.timeout_seconds)}; ::setrlimit(RLIMIT_CPU, &cpu);
    struct rlimit as{1ull << 30, 1ull << 30}; ::setrlimit(RLIMIT_AS, &as);
    ::setsid();
    if (opt.sandbox) {
      pnr::sandbox::Policy pol; pol.game_dir = dir;
      std::string se;
      if (!pnr::sandbox::enter(pol, &se)) _exit(126);
    }
    char* envp[] = {const_cast<char*>("PATH=/usr/bin:/bin"), const_cast<char*>("LANG=C"), nullptr};
    char* av[] = {const_cast<char*>(exe.c_str()), const_cast<char*>("--tools"), const_cast<char*>(tools_path.c_str()), const_cast<char*>("--prompt"), const_cast<char*>(prompt.c_str()), nullptr};
    ::execve(exe.c_str(), av, envp);
    _exit(127);
  }
  ::close(pipefd[1]);
  const auto t0 = std::chrono::steady_clock::now();
  bool timed_out = false, too_big = false;
  char buf[4096];
  while (true) {
    const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (el > opt.timeout_seconds) { timed_out = true; break; }
    struct pollfd pf{pipefd[0], POLLIN, 0};
    int pr = ::poll(&pf, 1, 200);
    if (pr < 0 && errno != EINTR) break;
    if (pr > 0) { ssize_t n = ::read(pipefd[0], buf, sizeof buf); if (n <= 0) break; out->append(buf, static_cast<size_t>(n)); if (out->size() > (1u << 20)) { too_big = true; break; } }
  }
  ::close(pipefd[0]);
  if (timed_out || too_big) ::kill(-pid, SIGKILL);
  int st = 0; while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
  if ((timed_out || too_big) && opt.sandbox) {  // the killed sandbox parent could not remove its empty mountpoint dir: remove ours (empty, ours, created during this run), best effort
    const auto cutoff = std::chrono::system_clock::now() - std::chrono::seconds(static_cast<long>(opt.timeout_seconds) + 60);
    std::error_code lec;
    for (fs::directory_iterator it("/tmp", lec), end; !lec && it != end; it.increment(lec)) {
      const std::string nm = it->path().filename().string();
      if (nm.rfind("pnr-sb-", 0) != 0) continue;
      struct stat sb; if (::lstat(it->path().c_str(), &sb) != 0 || sb.st_uid != ::getuid() || !S_ISDIR(sb.st_mode)) continue;
      if (std::chrono::system_clock::from_time_t(sb.st_mtime) < cutoff) continue;
      ::rmdir(it->path().c_str());   // fails harmlessly if it is not empty
    }
  }
  if (timed_out) return fail("the model did not answer within " + std::to_string(opt.timeout_seconds) + " s and was stopped");
  if (too_big) return fail("the model produced too much output and was stopped");
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) return fail("the model program failed to run (exit " + std::to_string(WIFEXITED(st) ? WEXITSTATUS(st) : -1) + ")");
  while (!out->empty() && (out->back() == '\n' || out->back() == ' ')) out->pop_back();
  return Status::Ok();
}

int cmd_ask(const std::string& prompt, const std::string& needle_path, double thr, const Lookups& lk, AskIO& io, std::string* out) {
  char tb[16]; std::snprintf(tb, sizeof tb, "%.2f", thr);
  if (needle_path.empty()) {
    *out += "ask is not configured. It needs a Needle 2 executable you installed yourself: set PNR_NEEDLE to its path.\nPNR downloads and bundles nothing. See docs/ask.md.\n";
    return 2;
  }
  if (!(thr >= 0 && thr <= 1)) { *out += "ask: --threshold must be between 0 and 1\n"; return 1; }
  std::string raw; Status ms = io.model ? io.model(prompt, &raw) : run_needle(needle_path, prompt, NeedleRun{30, io.sandbox_model}, &raw);
  if (!ms) { *out += "NOT RUN: " + ms.error.message + "\n"; return 1; }
  Parsed p = interpret(raw, thr, lk);
  char cb[16]; std::snprintf(cb, sizeof cb, "%.2f", p.confidence);
  *out += "model (experimental, uncalibrated, " + std::string(io.sandbox_model ? "sandboxed" : "NOT sandboxed") + "): " + (p.tool.empty() ? std::string("no command") : p.tool) + ", confidence " + cb + ", threshold " + tb + "\n";
  if (p.verdict != Verdict::Accepted) {
    *out += std::string("NOT RUN: ") + verdict_name(p.verdict) + ": " + p.reason + "\n";
    if (p.args_ok && p.verdict == Verdict::LowConfidence) *out += "closest guess (not run): " + p.command() + "\n";
    return 1;
  }
  *out += "parsed command: " + p.command() + "\n";
  if (p.mutating) {
    if (!io.interactive || !io.confirm) { *out += "NOT RUN: this command changes things or starts a game, and confirmation needs an interactive terminal. Run it yourself if it is what you meant.\n"; return 1; }
    if (!io.confirm(p.command())) { *out += "not run (you declined)\n"; return 1; }
  }
  std::string res; int rc = io.runner ? io.runner(p.argv, &res) : 1;
  *out += res;
  return rc;
}

}  // namespace pnr::ask
