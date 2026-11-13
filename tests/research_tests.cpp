// Slice C1 tests: research log, generic fetch (against an in-process fake that
// speaks Range/If-Range), and format inspection. No real host is contacted.
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "package/importer.hpp"
#include "package/sha256.hpp"
#include "psn/http.hpp"
#include "research/research.hpp"

namespace fs = std::filesystem;
using namespace pnr::research;
namespace {
std::string slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
void spit(const std::string& p, const std::string& s) { std::ofstream f(p, std::ios::binary | std::ios::trunc); f << s; }
bool has(const std::string& h, const std::string& n) { return h.find(n) != std::string::npos; }
std::string content_of(size_t n, char seed) { std::string s(n, 0); for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>((i * 31 + seed + (i >> 8)) & 0xff); return s; }

struct Fake : pnr::psn::HttpClient {
  std::string content, etag = "\"e1\"";
  size_t drop_after = 0; int drops_left = 0;
  int fail_status = 0, fail_times = 0, calls = 0;
  bool declare_no_length = false, oversend = false;
  std::vector<std::string> ranges, urls;
  pnr::Status perform(const pnr::psn::HttpRequest&, pnr::psn::HttpResponse*) override { return pnr::Status::Fail(214, "psn", "no"); }
  pnr::Status download(const pnr::psn::HttpRequest& rq, const pnr::psn::BodySink& sink, pnr::psn::HttpResponse* res) override {
    ++calls; urls.push_back(rq.url);
    std::string range, ifr;
    for (auto& h : rq.headers) { if (h.rfind("Range: ", 0) == 0) range = h.substr(7); if (h.rfind("If-Range: ", 0) == 0) ifr = h.substr(10); }
    ranges.push_back(range);
    *res = pnr::psn::HttpResponse{};
    if (fail_times > 0) { --fail_times; res->status = fail_status; return pnr::Status::Ok(); }
    size_t start = 0; bool partial = false;
    if (!range.empty()) { start = static_cast<size_t>(std::atoll(range.c_str() + 6)); if (start >= content.size()) { res->status = 416; return pnr::Status::Ok(); } partial = ifr.empty() || ifr == etag; if (!partial) start = 0; }
    res->etag = etag;
    std::string body = content.substr(start); if (oversend) body += "EXTRA";
    res->status = partial ? 206 : 200;
    if (partial) res->content_range = "bytes " + std::to_string(start) + "-" + std::to_string(content.size() - 1) + "/" + std::to_string(content.size());
    res->content_length = declare_no_length ? -1 : static_cast<long long>(oversend ? body.size() - 5 : body.size());
    size_t limit = body.size(); bool drop = false;
    if (drops_left > 0 && drop_after < body.size()) { limit = drop_after; drop = true; --drops_left; }
    for (size_t off = 0; off < limit; off += 8192) { size_t n = std::min<size_t>(8192, limit - off); if (!sink(*res, body.data() + off, n)) return pnr::Status::Fail(214, "psn", "sink refused"); }
    if (drop) return pnr::Status::Fail(213, "psn", "connection dropped");
    return pnr::Status::Ok();
  }
};
}  // namespace

void pnr_research_tests(int& pass, int& fail) {
  auto check = [&](bool ok, const std::string& l) { if (ok) ++pass; else ++fail; std::printf("%s %s\n", ok ? "PASS" : "FAIL", l.c_str()); };
  const std::string d = "/tmp/pnr_research_" + std::to_string(::getpid());
  fs::remove_all(d); fs::create_directories(d);

  // ---- log
  {
    Log log(d + "/l1"); int tick = 0;
    log.clock = [&] { return "2026-01-01T00:00:0" + std::to_string(++tick) + "Z"; };
    LogEntry e;
    check(log.append("note", "", "first", "", &e) && e.seq == 1 && e.time == "2026-01-01T00:00:01Z", "log: first entry is seq 1 with the injected clock");
    log.append("note", "", "second \"quoted\" \\ text", ""); log.append("inspect", "/x/y", "third", std::string(64, 'a'));
    std::vector<LogEntry> v; check(log.read(&v) && v.size() == 3 && v[1].text == "second \"quoted\" \\ text" && v[2].prev == v[1].hash && v[0].prev == std::string(64, '0'), "log: entries round-trip (quotes, backslashes) and chain");
    size_t n = 0; std::string det; check(log.verify(&n, &det) && n == 3 && has(det, "cannot detect removal of the newest"), "log: verify passes and states its own limits");
    check(!log.append("note", "", "bad\nnewline", "") && !log.append("note", "", "", "") && !log.append("note", "", "x", "nothex") && !log.append("bogus", "", "x", ""), "log: newlines, empty notes, bad hashes and unknown kinds are refused");
    const std::string orig = slurp(log.path());
    std::string t = orig; t.replace(t.find("first"), 5, "FIRST"); spit(log.path(), t);
    check(!log.verify(&n, &det) && has(det, "line 1"), "log: an edited entry is detected");
    check(!log.append("note", "", "after tamper", ""), "log: appending to a broken chain is refused");
    { std::string lines[3]; size_t a = 0, k = 0; while (k < 3) { size_t b = orig.find('\n', a); lines[k++] = orig.substr(a, b - a + 1); a = b + 1; }
      spit(log.path(), lines[0] + lines[2]); check(!log.verify(&n, &det), "log: a deleted middle entry is detected");
      spit(log.path(), lines[1] + lines[0] + lines[2]); check(!log.verify(&n, &det), "log: reordered entries are detected");
      spit(log.path(), lines[0] + lines[1]); check(log.verify(&n, &det) && n == 2, "log: HONEST: dropping the newest entries is NOT detectable (stated in verify)"); }
    spit(log.path(), orig + "garbage line\n"); check(!log.verify(&n, &det), "log: a garbage line is detected");
  }
  {  // concurrent appenders
    Log log(d + "/l2");
    for (int c = 0; c < 4; ++c) { if (::fork() == 0) { Log l(d + "/l2"); for (int k = 0; k < 5; ++k) l.append("note", "", "child " + std::to_string(c) + " #" + std::to_string(k), ""); _exit(0); } }
    int st; while (::wait(&st) > 0) {}
    size_t n = 0; std::string det; std::vector<LogEntry> v;
    check(log.verify(&n, &det) && n == 20 && log.read(&v) && v.back().seq == 20, "log: 4 concurrent processes appending 20 entries keep one intact chain");
  }

  // ---- fetch
  fs::create_directories(d + "/f");
  const std::string content = content_of(300000, 7), sha = pnr::pkg::sha256_hex(content);
  const std::string url = "https://files.example.invalid/a/b.bin";
  FetchOptions fo; fo.rights = "my own build artifact"; fo.backoff_ms = 0; fo.free_margin = 1;
  {
    Log log(d + "/f/log"); Fake fk; fk.content = content; FetchResult r; fo.expect_sha256 = sha;
    pnr::Status s = fetch(fk, log, url, d + "/f/ok.bin", fo, &r);
    check(s && r.hash_verified && r.size_declared && r.bytes == 300000 && slurp(d + "/f/ok.bin") == content && !fs::exists(d + "/f/ok.bin.part"), "fetch: verified download lands at the output path, no .part left");
    std::vector<LogEntry> v; log.read(&v);
    check(v.size() == 1 && v[0].kind == "fetch" && v[0].sha256 == sha && has(v[0].text, "my own build artifact") && has(v[0].text, "unverified") && has(v[0].text, "matched"), "fetch: logged with url, hash and the declared rights (marked unverified)");
    fo.expect_sha256 = "";
    s = fetch(fk, log, url, d + "/f/nohash.bin", fo, &r);
    check(s && !r.hash_verified && r.sha256 == sha, "fetch: without --sha256 the result says integrity is UNVERIFIED");
    log.read(&v); check(has(v.back().text, "NO expected hash"), "fetch: the log entry says no expected hash was supplied");
    fo.expect_sha256 = std::string(64, 'b');
    s = fetch(fk, log, url, d + "/f/bad.bin", fo, &r);
    check(!s && s.error.code == kErrHash && !fs::exists(d + "/f/bad.bin") && !fs::exists(d + "/f/bad.bin.part"), "fetch: a wrong expected hash deletes the file and fails");
    log.read(&v); check(v.back().kind == "fetch-failed", "fetch: the failure is logged as fetch-failed");
    fo.expect_sha256 = sha;
    s = fetch(fk, log, url, d + "/f/ok.bin", fo, &r);
    check(!s && has(s.error.message, "already exists"), "fetch: an existing output file is not overwritten");
    fo.force = true; fk.calls = 0; s = fetch(fk, log, url, d + "/f/ok.bin", fo, &r); fo.force = false;
    check(s && fk.calls == 1, "fetch: --force replaces it");
  }
  {
    Log log(d + "/f/log2"); FetchResult r; Fake fk; fk.content = content;
    FetchOptions o = fo; o.expect_sha256 = sha; o.max_retries = 0; fk.drops_left = 1; fk.drop_after = 100000;
    pnr::Status s = fetch(fk, log, url, d + "/f/res.bin", o, &r);
    check(!s && fs::exists(d + "/f/res.bin.part") && fs::file_size(d + "/f/res.bin.part") == 100000 && !fs::exists(d + "/f/res.bin"), "fetch: an interrupted transfer keeps the .part and writes no output file");
    fk.ranges.clear(); s = fetch(fk, log, url, d + "/f/res.bin", o, &r);
    check(s && r.resumed && fk.ranges.size() == 1 && fk.ranges[0] == "bytes=100000-" && slurp(d + "/f/res.bin") == content, "fetch: the next run resumes with a Range request and the hash still matches");
    Fake k2; k2.content = content; k2.drops_left = 1; k2.drop_after = 50000; o.max_retries = 3; s = fetch(k2, log, url, d + "/f/retry.bin", o, &r);
    check(s && k2.calls == 2 && slurp(d + "/f/retry.bin") == content, "fetch: a dropped connection is retried and resumed within one run");
    Fake k3; k3.content = content; k3.fail_status = 503; k3.fail_times = 2; s = fetch(k3, log, url, d + "/f/r503.bin", o, &r);
    check(s && k3.calls == 3, "fetch: HTTP 503 is retried");
    Fake k4; k4.content = content; k4.fail_status = 302; k4.fail_times = 1; s = fetch(k4, log, url, d + "/f/redir.bin", o, &r);
    check(!s && k4.calls == 1 && has(s.error.message, "redirect") && !fs::exists(d + "/f/redir.bin"), "fetch: redirects are refused, not followed");
    Fake k5; k5.content = content; k5.fail_status = 404; k5.fail_times = 1; s = fetch(k5, log, url, d + "/f/nf.bin", o, &r);
    check(!s && k5.calls == 1, "fetch: HTTP 404 fails without retrying");
    Fake k6; k6.content = content; k6.oversend = true; s = fetch(k6, log, url, d + "/f/over.bin", o, &r);
    check(!s && !fs::exists(d + "/f/over.bin") && !fs::exists(d + "/f/over.bin.part"), "fetch: a server sending more than it declared is refused and cleaned up");
    FetchOptions small = o; small.max_bytes = 1000; Fake k7; k7.content = content; s = fetch(k7, log, url, d + "/f/big.bin", small, &r);
    check(!s && has(s.error.message, "max-bytes") && !fs::exists(d + "/f/big.bin"), "fetch: --max-bytes is enforced");
    Fake k8; k8.content = content; k8.declare_no_length = true; o.expect_sha256 = ""; s = fetch(k8, log, url, d + "/f/nolen.bin", o, &r);
    check(s && !r.size_declared && !r.hash_verified, "fetch: a server that declares no size is flagged (size_declared=false)");
    std::vector<LogEntry> v; log.read(&v); bool nl = false; for (auto& e : v) if (has(e.text, "declared no size")) nl = true;
    check(nl, "fetch: the missing size declaration is recorded in the log");
  }
  {
    Log log(d + "/f/log3"); Fake fk; fk.content = content; FetchResult r; FetchOptions o = fo; o.expect_sha256 = "";
    auto refused = [&](const std::string& u, FetchOptions oo, const std::string& label) { fk.calls = 0; auto s = fetch(fk, log, u, d + "/f/never.bin", oo, &r); check(!s && fk.calls == 0 && !fs::exists(d + "/f/never.bin"), label); return s; };
    refused("http://files.example.invalid/x", o, "fetch: plain http is refused before any request");
    refused("https://user:pw@files.example.invalid/x", o, "fetch: URLs with userinfo are refused");
    refused("https://files.example.invalid:8443/x", o, "fetch: non-443 ports are refused");
    refused("https://files.example.invalid/x#frag", o, "fetch: fragments are refused");
    auto s = refused("https://store.playstation.com/x", o, "fetch: Sony hosts are refused (playstation.com subdomain)");
    check(s.error.code == kErrRefusedHost, "fetch: the Sony refusal has its own error code");
    refused("https://ca.account.sony.com/x", o, "fetch: Sony hosts are refused (sony.com subdomain)");
    FetchOptions nr = o; nr.rights = ""; refused(url, nr, "fetch: a missing rights statement is refused");
    FetchOptions bh = o; bh.expect_sha256 = "XYZ"; refused(url, bh, "fetch: a malformed --sha256 is refused");
    check(!fetch(fk, log, url, d + "/no/such/dir/x.bin", o, &r) && fk.calls == 0, "fetch: a missing output directory is refused");
    std::vector<LogEntry> v; log.read(&v); bool sony = false; for (auto& e : v) if (e.kind == "fetch-failed" && has(e.text, "Sony")) sony = true;
    check(sony, "fetch: refused Sony attempts are logged");
  }

  // ---- inspect
  {
    char exe[4096]; ssize_t n = ::readlink("/proc/self/exe", exe, sizeof exe - 1); exe[n > 0 ? n : 0] = 0;
    Inspection in;
    check(inspect_file(exe, &in) && in.kind == "elf" && has(render_inspection(in, false), "x86-64") && has(render_inspection(in, false), "PNR can launch natively: yes"), "inspect: a real x86-64 ELF is identified");
    spit(d + "/s.pkg", std::string("\x7f" "CNT", 4) + std::string(5000, '\x41'));
    check(inspect_file(d + "/s.pkg", &in) && in.sony_container && in.facts.empty() && has(render_inspection(in, false), "does not parse, decrypt"), "inspect: a Sony container is recognised by magic only and not opened");
    pnr::pkg::Manifest m; m.id = "demo.game"; m.title = "Demo"; m.version = "1.0"; m.executable = "bin/game";
    check(pnr::pkg::build_package(d + "/g.pnrgame", m, {{"bin/game", exe}}).ok, "inspect: test package built");
    check(inspect_file(d + "/g.pnrgame", &in) && in.kind == "pnrgame" && has(render_inspection(in, false), "demo.game") && fs::exists(d + "/g.pnrgame"), "inspect: a valid .pnrgame is validated read-only");
    std::string pg = slurp(d + "/g.pnrgame"); pg[pg.size() / 2] ^= 0x55; spit(d + "/g2.pnrgame", pg);
    check(inspect_file(d + "/g2.pnrgame", &in) && (has(in.kind, "not a valid") || in.kind == "pnrgame") && (in.kind != "pnrgame" || false), "inspect: a corrupted package is NOT reported as valid");
    spit(d + "/idx.json", "{\"packages\":[{},{}]}");
    check(inspect_file(d + "/idx.json", &in) && has(in.kind, "store index") && has(render_inspection(in, false), "NOT validated"), "inspect: a store-index-shaped JSON is identified, with a note that it is not validated");
    spit(d + "/bad.json", "{\"a\":"); check(inspect_file(d + "/bad.json", &in) && has(render_inspection(in, false), "not valid JSON"), "inspect: broken JSON is reported as such");
    std::string rnd(100000, 0); unsigned x = 12345; for (auto& c : rnd) { x = x * 1103515245u + 12345u; c = static_cast<char>((x >> 16) & 0xff); }
    spit(d + "/rnd.bin", rnd); check(inspect_file(d + "/rnd.bin", &in) && in.kind == "unknown" && in.entropy > 7.5 && has(render_inspection(in, false), "not proof"), "inspect: high entropy is flagged as an indication, not proof");
    check(!inspect_file(d + "/nope", &in) && !inspect_file(d, &in), "inspect: a missing file or a directory is an error");
    check(has(render_inspection(in, true), "\"kind\""), "inspect: JSON output is available");
  }

  // ---- CLI
  {
    std::string out; CliOptions co; Fake fk; fk.content = content;
    check(cmd_research({"log", "add", "hello"}, co, d + "/c", &out) == 0 && has(out, "logged #1"), "cli: log add");
    out.clear(); check(cmd_research({"log", "list"}, co, d + "/c", &out) == 0 && has(out, "hello"), "cli: log list");
    out.clear(); co.rights = "mine"; co.expect_sha256 = sha;
    check(cmd_research({"fetch", url, d + "/c.bin"}, co, d + "/c", &out, &fk, &fo) == 0 && has(out, "matched the sha256 you supplied") && has(out, "logged"), "cli: fetch reports verified integrity");
    out.clear(); co.expect_sha256 = ""; co.force = true;
    check(cmd_research({"fetch", url, d + "/c.bin"}, co, d + "/c", &out, &fk, &fo) == 0 && has(out, "UNVERIFIED"), "cli: fetch without a hash says UNVERIFIED");
    out.clear(); co.log_it = true;
    check(cmd_research({"inspect", d + "/c.bin", }, co, d + "/c", &out) == 0 && has(out, "logged"), "cli: inspect --log appends to the log");
    out.clear(); check(cmd_research({"log", "verify"}, co, d + "/c", &out) == 0 && has(out, "OK") && has(out, "4 entries"), "cli: the chain is intact after mixed entries");
    out.clear(); check(cmd_research({"bogus"}, co, d + "/c", &out) == 64 && has(out, "usage"), "cli: unknown subcommand prints usage");
  }
  fs::remove_all(d);
}
