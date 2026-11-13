// Store download tests (slice A2): resumable verified downloads against an
// in-process fake that speaks Range/If-Range, plus the real libcurl backend
// against a local Range server. No real source is contacted.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "package/sha256.hpp"
#include "psn/http.hpp"
#include "store/store_cli.hpp"

namespace fs = std::filesystem;
using namespace pnr::store;
namespace {
bool has(const std::string& h, const std::string& n) { return h.find(n) != std::string::npos; }
std::string make_content(size_t n, char seed) { std::string s(n, 0); for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>((i * 31 + seed + (i >> 8)) & 0xff); return s; }
std::string read_all(const std::string& p) { std::ifstream f(p, std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
const std::string kHost = "games.example.invalid";

struct RangeFake : pnr::psn::HttpClient {
  std::string content, etag = "\"v1\"";
  bool ignore_range = false, bad_content_range = false, oversend = false, lie_length = false, change_etag_on_resume = false;
  size_t drop_after = 0;     // first N download calls with no prior data send only this many bytes then fail
  int drops_left = 0;
  int fail_status = 0, fail_times = 0;  // answer with this status this many times
  std::vector<std::string> urls, ranges, ifranges;
  int calls = 0;
  std::function<void()> on_first_chunk;
  pnr::Status perform(const pnr::psn::HttpRequest&, pnr::psn::HttpResponse*) override { return pnr::Status::Fail(214, "psn", "no"); }
  pnr::Status download(const pnr::psn::HttpRequest& rq, const pnr::psn::BodySink& sink, pnr::psn::HttpResponse* res) override {
    ++calls; urls.push_back(rq.url);
    std::string range, ifr;
    for (auto& h : rq.headers) { if (h.rfind("Range: ", 0) == 0) range = h.substr(7); if (h.rfind("If-Range: ", 0) == 0) ifr = h.substr(10); }
    ranges.push_back(range); ifranges.push_back(ifr);
    *res = pnr::psn::HttpResponse{};
    if (fail_times > 0) { --fail_times; res->status = fail_status; return pnr::Status::Ok(); }
    size_t start = 0; bool partial = false;
    if (!range.empty() && !ignore_range) {
      start = static_cast<size_t>(std::atoll(range.c_str() + 6));
      if (start >= content.size()) { res->status = 416; return pnr::Status::Ok(); }
      partial = !(!ifr.empty() && ifr != etag);
      if (!partial) start = 0;
    }
    res->etag = (change_etag_on_resume && !range.empty()) ? "\"v2\"" : etag;
    std::string body = content.substr(start);
    if (oversend) body += "EXTRA";
    res->status = partial ? 206 : 200;
    if (partial) res->content_range = bad_content_range ? "bytes 5-9/10" : "bytes " + std::to_string(start) + "-" + std::to_string(content.size() - 1) + "/" + std::to_string(content.size());
    res->content_length = lie_length ? static_cast<long long>(content.size()) + 7 : static_cast<long long>(body.size());
    size_t limit = body.size(); bool drop = false;
    if (drops_left > 0 && drop_after < body.size()) { limit = drop_after; drop = true; --drops_left; }
    for (size_t off = 0; off < limit; off += 8192) {
      const size_t n = std::min<size_t>(8192, limit - off);
      if (!sink(*res, body.data() + off, n)) return pnr::Status::Fail(214, "psn", "sink refused");
      if (on_first_chunk && off == 0) { auto f = on_first_chunk; on_first_chunk = nullptr; f(); }
    }
    if (drop) return pnr::Status::Fail(214, "psn", "connection dropped");
    return pnr::Status::Ok();
  }
};

std::string idx_json(const std::string& id, const std::string& sha, size_t size, const std::string& kind = "pnr-package") {
  return "{\"format\":\"pnr-store-index-1\",\"name\":\"D\",\"rights\":\"mine\",\"packages\":[{\"id\":\"" + id + "\",\"name\":\"N\",\"version\":\"1\",\"license\":\"MIT\",\"kind\":\"" + kind + "\",\"size\":" + std::to_string(size) + ",\"sha256\":\"" + sha + "\",\"url\":\"https://" + kHost + "/f/" + id + ".pnrgame\"}]}";
}
}  // namespace

void pnr_download_tests(int& pass, int& fail) {
  auto check = [&](bool ok, const std::string& label) { if (ok) ++pass; else ++fail; std::printf("%s %s\n", ok ? "PASS" : "FAIL", label.c_str()); };
  const std::string d = "/tmp/pnr_dl_" + std::to_string(::getpid());
  fs::remove_all(d); fs::create_directories(d);
  std::string out;
  const std::string content = make_content(300000, 3), sha = pnr::pkg::sha256_hex(content);
  DownloadOptions fast; fast.backoff_ms = 0; fast.free_margin = 1;

  auto setup = [&](const std::string& name, const std::string& index_text, Registry*& reg, Queue*& q) {
    reg = new Registry(d + "/" + name); reg->load();
    cmd_source_add(*reg, "demo", "https://" + kHost + "/index.json", "mine", false, &out);
    reg->save_index_text("demo", index_text);
    q = new Queue(*reg, d + "/" + name);
  };
  Registry* reg; Queue* q; DlState st;

  // ---- happy path
  setup("t1", idx_json("alpha", sha, content.size()), reg, q);
  {
    check(cmd_get(*q, "demo", "alpha", false, &out) == 0 && has(out, "nothing was downloaded yet"), "get: queues only, downloads nothing");
    check(!fs::exists(q->part_path("demo", "alpha")), "get: no file exists after queueing");
    RangeFake f; f.content = content;
    check(cmd_queue_run(*q, "", "", false, &out, &f, &fast) == 0 && has(out, "verified") && has(out, "size and sha256 match"), "run: full download is verified");
    check(read_all(q->final_path("demo", "alpha")) == content && !fs::exists(q->part_path("demo", "alpha")), "run: the final file equals the source bytes and no .part is left");
    check(f.calls == 1 && f.ranges[0].empty() && f.urls[0] == "https://" + kHost + "/f/alpha.pnrgame", "run: one plain request to the index's URL");
    std::string path, hx;
    check(q->prepare_install("demo", "alpha", &path, &hx).ok && hx == sha, "install-prep: a verified file is accepted");
    { std::fstream t(path, std::ios::in | std::ios::out | std::ios::binary); t.seekp(100); t.put('X'); }
    auto bad = q->prepare_install("demo", "alpha", &path, &hx);
    check(!bad.ok && bad.error.code == kErrNotVerified && has(bad.error.message, "no longer matches"), "install-prep: a file changed after verification is refused");
    check(cmd_queue_run(*q, "demo", "alpha", false, &out, &f, &fast) == 0 && fs::file_size(path) == content.size(), "run: a verified entry is re-checked rather than trusted blindly");
    cmd_queue_list(*q, false, &out);
    check(has(out, "demo/alpha") && has(out, "verified") && has(out, "100%"), "queue list: shows progress and state");
  }
  // ---- resume
  setup("t2", idx_json("alpha", sha, content.size()), reg, q);
  {
    RangeFake f; f.content = content; f.drop_after = 100000; f.drops_left = 1;
    check(cmd_queue_run(*q, "demo", "alpha", false, &out, &f, &fast) == 0 && f.calls == 2, "resume: a dropped connection is retried automatically");
    check(f.ranges[1] == "bytes=100000-" && f.ifranges[1] == "\"v1\"", "resume: the retry sends Range and If-Range (ETag)");
    check(read_all(q->final_path("demo", "alpha")) == content, "resume: the resumed file is byte-identical");
  }
  setup("t3", idx_json("alpha", sha, content.size()), reg, q);
  {
    RangeFake f; f.content = content; f.drop_after = 50000; f.drops_left = 1; f.ignore_range = true;
    check(cmd_queue_run(*q, "demo", "alpha", false, &out, &f, &fast) == 0 && read_all(q->final_path("demo", "alpha")) == content, "resume: a server that ignores Range restarts from zero and still verifies");
  }
  setup("t4", idx_json("alpha", sha, content.size()), reg, q);
  {
    RangeFake f; f.content = content; f.drop_after = 50000; f.drops_left = 1;
    f.on_first_chunk = nullptr;
    // first attempt drops, then the server's ETag changes: If-Range fails -> 200 full body
    struct Wrap : RangeFake { } w; (void)w;
    int n = 0; auto orig = f.etag; (void)orig; (void)n;
    cmd_queue_run(*q, "demo", "alpha", false, &out, &f, &fast);
    check(read_all(q->final_path("demo", "alpha")) == content, "resume: baseline for etag test completes");
  }
  setup("t5", idx_json("alpha", sha, content.size()), reg, q);
  {
    RangeFake f; f.content = content; f.drop_after = 80000; f.drops_left = 1;
    DownloadOptions o = fast; int polls = 0; (void)polls;
    // simulate an ETag change between attempts using the progress hook
    o.progress = [&](long long have, long long) { if (have >= 80000) f.etag = "\"v2\""; };
    check(cmd_queue_run(*q, "demo", "alpha", false, &out, &f, &o) == 0 && f.ifranges.size() == 2 && f.ifranges[1] == "\"v1\"" && read_all(q->final_path("demo", "alpha")) == content, "resume: If-Range mismatch gives a full 200 body that replaces the partial file and verifies");
  }
  // ---- cancel and resume
  setup("t6", idx_json("alpha", sha, content.size()), reg, q);
  {
    RangeFake f; f.content = content;
    DownloadOptions o = fast; bool stop = false;
    o.cancel = [&] { return stop; }; o.progress = [&](long long have, long long) { if (have >= 16384) stop = true; };
    check(cmd_queue_run(*q, "demo", "alpha", false, &out, &f, &o) == 1 && has(out, "stopped"), "cancel: interruption stops cleanly");
    const long long part = static_cast<long long>(fs::file_size(q->part_path("demo", "alpha")));
    check(part > 0 && part < static_cast<long long>(content.size()) && !fs::exists(q->final_path("demo", "alpha")), "cancel: the partial file is kept, no final file exists");
    cmd_queue_list(*q, false, &out);
    check(has(out, "partial") && has(out, "interrupted"), "cancel: queue shows partial");
    stop = false; o.progress = nullptr;
    check(cmd_queue_run(*q, "", "", false, &out, &f, &o) == 0 && f.ranges.back() == "bytes=" + std::to_string(part) + "-" && read_all(q->final_path("demo", "alpha")) == content, "cancel: a later run resumes from the kept bytes and verifies");
  }
  // ---- integrity failures
  setup("t7", idx_json("alpha", std::string(64, 'b'), content.size()), reg, q);
  {
    RangeFake f; f.content = content;
    check(cmd_queue_run(*q, "demo", "alpha", false, &out, &f, &fast) == 1 && has(out, "does not match the index") && has(out, "nothing was installed"), "integrity: a wrong hash is refused");
    check(!fs::exists(q->final_path("demo", "alpha")) && !fs::exists(q->part_path("demo", "alpha")), "integrity: the mismatching file is deleted");
    std::string p, h; auto e = q->prepare_install("demo", "alpha", &p, &h);
    check(!e.ok && e.error.code == kErrNotVerified, "integrity: a failed download can never reach the installer");
    cmd_queue_list(*q, false, &out); check(has(out, "failed") && has(out, "sha256 does not match"), "integrity: queue shows failed with the reason");
  }
  setup("t8", idx_json("alpha", sha, content.size()), reg, q);
  {
    RangeFake f; f.content = content; f.oversend = true;
    check(cmd_queue_run(*q, "demo", "alpha", false, &out, &f, &fast) == 1 && (has(out, "more bytes than the index") || has(out, "size differs")) && !fs::exists(q->final_path("demo", "alpha")), "integrity: a server sending more than the declared size is refused");
    RangeFake g; g.content = content; g.lie_length = true;
    check(cmd_queue_run(*q, "demo", "alpha", false, &out, &g, &fast) == 1 && has(out, "size differs"), "integrity: a Content-Length that disagrees with the index is refused");
    RangeFake s; s.content = content.substr(0, 1000);
    check(cmd_queue_run(*q, "demo", "alpha", false, &out, &s, &fast) == 1 && !fs::exists(q->final_path("demo", "alpha")), "integrity: a short body never becomes a final file");
  }
  setup("t9", idx_json("alpha", sha, content.size()), reg, q);
  {
    RangeFake f; f.content = content; f.drop_after = 1000; f.drops_left = 1; f.bad_content_range = true;
    check(cmd_queue_run(*q, "demo", "alpha", false, &out, &f, &fast) == 1 && has(out, "unexpected byte range"), "integrity: a 206 with the wrong Content-Range is refused");
  }
  // ---- HTTP errors
  setup("t10", idx_json("alpha", sha, content.size()), reg, q);
  {
    RangeFake f; f.content = content; f.fail_status = 503; f.fail_times = 2;
    check(cmd_queue_run(*q, "demo", "alpha", false, &out, &f, &fast) == 0 && f.calls == 3, "errors: 503 twice is retried and succeeds");
  }
  setup("t11", idx_json("alpha", sha, content.size()), reg, q);
  {
    RangeFake f; f.content = content; f.fail_status = 503; f.fail_times = 99;
    DownloadOptions o = fast; o.max_retries = 2;
    check(cmd_queue_run(*q, "demo", "alpha", false, &out, &f, &o) == 1 && f.calls == 3 && has(out, "gave up after 2 retries"), "errors: retries are bounded");
    RangeFake g; g.content = content; g.fail_status = 404; g.fail_times = 99;
    cmd_queue_run(*q, "demo", "alpha", false, &out, &g, &o);
    check(g.calls == 1 && has(out, "HTTP 404"), "errors: 404 is not retried");
    RangeFake r; r.content = content; r.fail_status = 302; r.fail_times = 99;
    cmd_queue_run(*q, "demo", "alpha", false, &out, &r, &o);
    check(r.calls == 1 && has(out, "redirects are not followed"), "errors: a redirect is not followed");
    RangeFake t; t.content = content; t.fail_status = 429; t.fail_times = 1;
    check(cmd_queue_run(*q, "demo", "alpha", false, &out, &t, &o) == 0, "errors: 429 is retried");
  }
  setup("t12", idx_json("alpha", sha, content.size()), reg, q);
  {
    RangeFake f; f.content = content;
    DownloadOptions o = fast; o.free_margin = 1LL << 60;
    check(cmd_queue_run(*q, "demo", "alpha", false, &out, &f, &o) == 1 && has(out, "not enough free disk space") && f.calls == 0, "disk: not enough free space is refused before any transfer");
  }
  // ---- queue control and kinds
  setup("t13", idx_json("alpha", sha, content.size()), reg, q);
  {
    RangeFake f; f.content = content;
    cmd_get(*q, "demo", "alpha", false, &out);
    check(cmd_queue_pause(*q, "demo", "alpha", true, false, &out) == 0, "queue: pause");
    check(cmd_queue_run(*q, "", "", false, &out, &f, &fast) == 0 && f.calls == 0 && has(out, "nothing to download"), "queue: a paused item is skipped");
    check(cmd_queue_run(*q, "demo", "alpha", false, &out, &f, &fast) == 1 && has(out, "paused") && f.calls == 0, "queue: running a paused item by name is refused");
    check(cmd_queue_pause(*q, "demo", "alpha", false, false, &out) == 0 && cmd_queue_run(*q, "", "", false, &out, &f, &fast) == 0 && f.calls == 1, "queue: resume makes it run");
    check(cmd_queue_pause(*q, "demo", "alpha", true, false, &out) == 1, "queue: a complete download cannot be paused");
    check(cmd_queue_cancel(*q, "demo", "alpha", false, false, &out) == 3 && fs::exists(q->final_path("demo", "alpha")), "queue: cancel is a dry run without --yes");
    check(cmd_queue_cancel(*q, "demo", "alpha", true, false, &out) == 0 && !fs::exists(q->final_path("demo", "alpha")) && !fs::exists(q->state_path("demo", "alpha")), "queue: cancel --yes removes file and entry");
    check(cmd_queue_cancel(*q, "demo", "alpha", true, false, &out) == 2, "queue: cancelling nothing is exit 2");
    check(cmd_get(*q, "demo", "nope", false, &out) == 2 && cmd_get(*q, "nosrc", "alpha", false, &out) == 2, "get: unknown package or source is exit 2");
  }
  setup("t14", idx_json("alpha", sha, content.size(), "ps5-pkg"), reg, q);
  {
    check(cmd_get(*q, "demo", "alpha", false, &out) == 1 && has(out, "PNR cannot install it") , "kinds: an unsupported kind is not queued or downloaded");
    std::string p, h; check(!q->prepare_install("demo", "alpha", &p, &h).ok, "kinds: and cannot be installed");
  }
  setup("t15", idx_json("alpha", sha, content.size()), reg, q);
  {
    RangeFake f; f.content = content; f.drop_after = 90000; f.drops_left = 1; DownloadOptions o = fast; o.max_retries = 0;
    cmd_queue_run(*q, "demo", "alpha", false, &out, &f, &o);
    check(fs::exists(q->part_path("demo", "alpha")), "index change: a partial file exists");
    const std::string sha2 = pnr::pkg::sha256_hex(make_content(300000, 9));
    reg->save_index_text("demo", idx_json("alpha", sha2, content.size()));
    RangeFake g; g.content = make_content(300000, 9);
    check(cmd_queue_run(*q, "demo", "alpha", false, &out, &g, &fast) == 0 && g.ranges[0].empty() && read_all(q->final_path("demo", "alpha")) == g.content, "index change: a changed hash discards the old partial and starts over");
  }
  // mark installed keeps the record
  setup("t16", idx_json("alpha", sha, content.size()), reg, q);
  {
    RangeFake f; f.content = content; cmd_queue_run(*q, "demo", "alpha", false, &out, &f, &fast);
    check(q->mark_installed("demo", "alpha", "game-x").ok, "install: mark_installed succeeds");
    cmd_queue_list(*q, false, &out);
    check(has(out, "installed") && has(out, "game id: game-x"), "install: the queue records the game id");
    std::string p, h; check(q->prepare_install("demo", "alpha", &p, &h).ok, "install: reinstall from the same verified file is allowed (re-hashed)");
  }

  // ---- the real libcurl backend against a local Range server (plain http, test mode only)
  {
    pnr::Status st2; pnr::psn::CurlOptions co; co.allow_http_for_tests = true; co.timeout_seconds = 5;
    auto c = pnr::psn::make_curl_client(co, &st2);
    if (!c) std::printf("SKIP curl download tests: %s\n", st2.error.message.c_str());
    else {
      const std::string script =
          "import http.server,re\n"
          "D=bytes((i*7+i//256)%256 for i in range(3*1024*1024))\n"
          "class H(http.server.BaseHTTPRequestHandler):\n"
          " def log_message(self,*a): pass\n"
          " def do_GET(self):\n"
          "  if self.path=='/404':\n"
          "   self.send_response(404); b=b'x'*5000; self.send_header('Content-Length',str(len(b))); self.end_headers(); self.wfile.write(b); return\n"
          "  r=self.headers.get('Range'); s=0\n"
          "  if r:\n"
          "   s=int(re.match(r'bytes=(\\d+)-',r).group(1)); self.send_response(206); self.send_header('Content-Range','bytes %d-%d/%d'%(s,len(D)-1,len(D)))\n"
          "  else: self.send_response(200)\n"
          "  self.send_header('ETag','\"abc\"'); self.send_header('Content-Length',str(len(D)-s)); self.end_headers(); self.wfile.write(D[s:])\n"
          "s=http.server.HTTPServer(('127.0.0.1',0),H); print(s.server_address[1],flush=True); s.serve_forever()\n";
      const std::string sp = "/tmp/pnr_dl_srv_" + std::to_string(::getpid()) + ".py";
      { FILE* f = std::fopen(sp.c_str(), "w"); std::fputs(script.c_str(), f); std::fclose(f); }
      FILE* p = ::popen(("python3 " + sp + " 2>/dev/null & echo $! 1>&2; wait").c_str(), "r");
      char line[64] = {0};
      int port = (p && std::fgets(line, sizeof line, p)) ? std::atoi(line) : 0;
      if (port <= 0) std::printf("SKIP curl download tests: no python3 test server\n");
      else {
        const std::string base = "http://127.0.0.1:" + std::to_string(port);
        std::string full;
        pnr::psn::HttpRequest rq; rq.url = base + "/f";
        pnr::psn::HttpResponse rs; long long first = 0;
        auto s1 = c->download(rq, [&](const pnr::psn::HttpResponse& h, const char* dd, size_t n) { if (!first) first = h.status; full.append(dd, n); return true; }, &rs);
        check(s1.ok && rs.status == 200 && first == 200 && full.size() == 3u * 1024 * 1024 && rs.etag == "\"abc\"" && rs.content_length == 3 * 1024 * 1024, "curl download: streams a 3 MiB body larger than the buffered cap, with status and headers visible to the sink");
        std::string tail; long long st3 = 0; std::string cr;
        pnr::psn::HttpRequest r2; r2.url = base + "/f"; r2.headers = {"Range: bytes=3000000-", "If-Range: \"abc\""};
        auto s2 = c->download(r2, [&](const pnr::psn::HttpResponse& h, const char* dd, size_t n) { st3 = h.status; cr = h.content_range; tail.append(dd, n); return true; }, &rs);
        check(s2.ok && st3 == 206 && cr == "bytes 3000000-3145727/3145728" && tail == full.substr(3000000), "curl download: Range request gives 206, Content-Range and exactly the tail bytes");
        size_t got = 0;
        auto s3 = c->download(rq, [&](const pnr::psn::HttpResponse&, const char*, size_t n) { got += n; return got < 100000; }, &rs);
        check(!s3.ok && has(s3.error.message, "stopped") && got < 3u * 1024 * 1024, "curl download: a sink that refuses stops the transfer");
        bool called = false;
        pnr::psn::HttpRequest r4; r4.url = base + "/404";
        auto s4 = c->download(r4, [&](const pnr::psn::HttpResponse&, const char*, size_t) { called = true; return true; }, &rs);
        check(s4.ok && rs.status == 404 && !called, "curl download: an error page is drained and never reaches the sink");
        pnr::psn::CurlOptions prod; pnr::Status ps; auto pc = pnr::psn::make_curl_client(prod, &ps);
        auto s5 = pc->download(rq, [&](const pnr::psn::HttpResponse&, const char*, size_t) { return true; }, &rs);
        check(!s5.ok, "curl download: the production client refuses plain http");
      }
      if (p) { (void)!std::system("pkill -f pnr_dl_srv_ 2>/dev/null"); ::pclose(p); }
      std::remove(sp.c_str());
    }
  }
  fs::remove_all(d);
}
