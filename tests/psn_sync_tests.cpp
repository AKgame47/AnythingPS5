// PSN slice 2 tests: protocol flow against a scripted FAKE, the allow-list
// guard, and the real libcurl backend against a local test server.
// Nothing here talks to Sony. All Sony-shaped replies are synthetic.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include <filesystem>

#include "library/game_db.hpp"
#include "psn/http.hpp"
#include "psn/psn_cli.hpp"
#include "psn/sync.hpp"

namespace {
using namespace pnr::psn;
const std::string kTok = "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZab";
bool has(const std::string& h, const std::string& n) { return h.find(n) != std::string::npos; }

std::string game(int i) {
  std::string e = "E" + std::to_string(i);
  return "{\"entitlementId\":\"" + e + "\",\"name\":\"G" + std::to_string(i) + "\",\"platform\":\"PS5\",\"titleId\":\"T" + e + "\",\"productId\":\"P" + e + "\",\"isActive\":true,\"isDownloadable\":true,\"isPreOrder\":false}";
}
std::string page(int from, int n) {
  std::string g;
  for (int i = 0; i < n; ++i) g += (i ? "," : "") + game(from + i);
  return "{\"data\":{\"purchasedTitlesRetrieve\":{\"games\":[" + g + "]}}}";
}
const char* kRedirect = "com.scee.psxandroid.scecompcall://redirect/?code=v3.SYNTHCODE&cid=x";

struct Fake : HttpClient {
  std::vector<HttpRequest> log;
  std::function<pnr::Status(const HttpRequest&, HttpResponse*)> fn;
  pnr::Status perform(const HttpRequest& r, HttpResponse* res) override { log.push_back(r); *res = HttpResponse{}; return fn(r, res); }
};
bool is_auth(const HttpRequest& r) { return has(r.url, "/oauth/authorize?"); }
bool is_token(const HttpRequest& r) { return has(r.url, "/oauth/token"); }
int start_of(const HttpRequest& r) { size_t p = r.url.find("%22start%22%3A"); return p == std::string::npos ? -1 : std::atoi(r.url.c_str() + p + 14); }
const std::string kTokOk = "{\"access_token\":\"ACCESS-1\",\"refresh_token\":\"REFRESH-1\",\"expires_in\":3600}";

// Happy-path script. `pages` = games per page in order; `size` is the page size under test.
Fake make_fake(std::vector<int> pages) {
  Fake f;
  auto idx = std::make_shared<int>(0);
  f.fn = [pages, idx](const HttpRequest& r, HttpResponse* res) {
    if (is_auth(r)) { res->status = 302; res->location = kRedirect; return pnr::Status::Ok(); }
    if (is_token(r)) { res->status = 200; res->body = kTokOk; return pnr::Status::Ok(); }
    res->status = 200;
    int p = *idx; ++*idx;
    int from = 0; for (int i = 0; i < p && i < (int)pages.size(); ++i) from += pages[i];
    res->body = page(from, p < (int)pages.size() ? pages[p] : 0);
    return pnr::Status::Ok();
  };
  return f;
}
}  // namespace

void pnr_psn_sync_tests(int& pass, int& fail) {
  auto check = [&](bool ok, const std::string& label) { if (ok) ++pass; else ++fail; std::printf("%s %s\n", ok ? "PASS" : "FAIL", label.c_str()); };
  std::vector<pnr::library::OwnedRecord> rows; SyncReport rep; SyncOptions o; o.page_size = 2;

  // ---- URL guard
  {
    auto ok = [](const std::string& u) { return url_allowed(u, official_hosts()); };
    check(ok("https://ca.account.sony.com/api/authz/v3/oauth/token") && ok("https://web.np.playstation.com/api/graphql/v1/op?x=1") && ok("HTTPS://CA.ACCOUNT.SONY.COM/x") && ok("https://ca.account.sony.com:443/x"), "guard: Sony's two official hosts over https are allowed");
    for (const char* bad : {"http://ca.account.sony.com/x", "https://evil.invalid/x", "https://ca.account.sony.com.evil.invalid/x", "https://ca.account.sony.com@evil.invalid/x",
                            "https://evil.invalid@ca.account.sony.com/x", "https://ca.account.sony.com:8443/x", "https://ca.account.sony.com./x", "ftp://ca.account.sony.com/x",
                            "https://ca.account.sony.com\\@evil.invalid/", "https://[::1]/x", "//ca.account.sony.com/x", "ca.account.sony.com/x", "", "https://",
                            "https://ca.account.sony.com /x", "https://sub.ca.account.sony.com/x", "https://ca.account.sony.co/x", "https://127.0.0.1/x", "file:///etc/passwd"})
      check(!ok(bad), std::string("guard: refused [") + bad + "]");
    Fake f = make_fake({0}); GuardedHttp g(f, official_hosts()); HttpResponse r;
    HttpRequest bad; bad.url = "https://evil.invalid/steal"; bad.headers = {"Cookie: npsso=" + kTok};
    check(g.perform(bad, &r).error.code == kErrBlockedHost && f.log.empty() && g.blocked == 1, "guard: a blocked request never reaches the client");
  }

  // ---- happy path
  {
    Fake f = make_fake({2, 2, 1});
    auto s = fetch_purchased_games(f, kTok, o, &rows, &rep);
    check(s.ok && rows.size() == 5 && rep.pages == 3 && rep.complete && rep.requests == 5 && rep.refreshes == 0, "sync: three pages (2,2,1) fetched, complete");
    check(rows[0].entitlement_id == "E0" && rows[4].entitlement_id == "E4", "sync: order and ids preserved across pages");
    check(f.log.size() == 5 && is_auth(f.log[0]) && is_token(f.log[1]) && f.log[1].method == "POST", "sync: request order is authorize, token, then library pages");
    check(f.log[0].headers.size() == 1 && f.log[0].headers[0] == "Cookie: npsso=" + kTok, "sync: the NPSSO is sent only as a Cookie header to the authorize URL");
    int leaks = 0;
    for (size_t i = 1; i < f.log.size(); ++i) { const auto& r = f.log[i]; if (has(r.url, kTok) || has(r.body, kTok)) ++leaks; for (auto& h : r.headers) if (has(h, kTok)) ++leaks; }
    check(leaks == 0 && !has(f.log[0].url, kTok), "sync: the NPSSO appears in no other request, URL or body");
    check(has(f.log[1].body, "code=v3.SYNTHCODE") && has(f.log[1].body, "grant_type=authorization_code") && has(f.log[1].body, "token_format=jwt") && has(f.log[1].body, "redirect_uri=com.scee.psxandroid.scecompcall%3A%2F%2Fredirect"), "sync: token exchange body carries the code, grant type and redirect URI");
    check(f.log[2].headers[0] == "Authorization: Bearer ACCESS-1" && has(f.log[2].url, "operationName=getPurchasedGameList") && has(f.log[2].url, "827a423f6a8ddca4107ac01395af2ec0eafd8396fc7fa204aaf9b7ed2eefa168") && has(f.log[2].url, "persistedQuery"), "sync: library request uses the Bearer token and the persisted-query hash");
    check(start_of(f.log[2]) == 0 && start_of(f.log[3]) == 2 && start_of(f.log[4]) == 4, "sync: paging start offsets 0, 2, 4");
    bool hosts = true; for (auto& r : f.log) hosts = hosts && url_allowed(r.url, official_hosts());
    check(hosts, "sync: every request went to an allow-listed https host");
    Fake e = make_fake({2, 2, 0});
    check(fetch_purchased_games(e, kTok, o, &rows, &rep).ok && rows.size() == 4 && rep.pages == 3 && rep.complete, "sync: a full last page followed by an empty page ends the paging");
    Fake z = make_fake({0});
    check(fetch_purchased_games(z, kTok, o, &rows, &rep).ok && rows.empty() && rep.complete, "sync: an empty library is a valid result");
    Fake cap = make_fake({2, 2, 2, 2, 2, 2});
    SyncOptions oc = o; oc.max_pages = 3;
    check(fetch_purchased_games(cap, kTok, oc, &rows, &rep).ok && rows.size() == 6 && !rep.complete, "sync: the page cap stops paging and reports not complete");
    Fake d = make_fake({2, 1});
    auto base = d.fn; int calls = 0;
    d.fn = [&](const HttpRequest& r, HttpResponse* res) { auto s = base(r, res); if (!is_auth(r) && !is_token(r) && calls++ == 1) res->body = page(1, 1); return s; };
    check(fetch_purchased_games(d, kTok, o, &rows, &rep).ok && rows.size() == 2 , "sync: an entitlement repeated across pages is kept once");
    SyncOptions big = o; big.page_size = 5000;
    Fake bg = make_fake({0});
    fetch_purchased_games(bg, kTok, big, &rows, &rep);
    check(start_of(bg.log[2]) == 0 && has(bg.log[2].url, "%22size%22%3A100"), "sync: page size is clamped to 100");
  }

  // ---- authorize / token failures
  auto run_with = [&](std::function<pnr::Status(const HttpRequest&, HttpResponse*)> fn, Fake* out = nullptr) {
    Fake f; f.fn = fn;
    auto s = fetch_purchased_games(f, kTok, o, &rows, &rep);
    if (out) *out = std::move(f);
    return s;
  };
  {
    check(run_with([](const HttpRequest&, HttpResponse* r) { r->status = 200; return pnr::Status::Ok(); }).error.code == kErrTokenRejected, "auth: HTTP 200 with no redirect is token-rejected");
    check(run_with([](const HttpRequest&, HttpResponse* r) { r->status = 302; r->location = "https://my.playstation.com/login"; return pnr::Status::Ok(); }).error.code == kErrTokenRejected, "auth: a redirect without a code is token-rejected");
    check(run_with([](const HttpRequest&, HttpResponse* r) { r->status = 302; r->location = "x://redirect/?code=bad code&y"; return pnr::Status::Ok(); }).error.code == kErrTokenRejected, "auth: a malformed code is refused");
    check(run_with([](const HttpRequest&, HttpResponse* r) { r->status = 429; return pnr::Status::Ok(); }).error.code == kErrRateLimited, "auth: 429 is rate-limited");
    check(run_with([](const HttpRequest&, HttpResponse* r) { r->status = 503; return pnr::Status::Ok(); }).error.code == kErrServer, "auth: 503 is server-error");
    check(run_with([](const HttpRequest&, HttpResponse*) { return pnr::Status::Fail(kErrTransport, "psn", "tls"); }).error.code == kErrTransport, "auth: a transport failure is reported as transport");
    auto s = run_with([](const HttpRequest&, HttpResponse* r) { r->status = 200; r->body = kTok; return pnr::Status::Ok(); });
    check(!has(s.error.message, kTok), "auth: error text never contains the token");
    auto tok = [](int st, const std::string& body) { return [st, body](const HttpRequest& r, HttpResponse* res) { if (is_auth(r)) { res->status = 302; res->location = kRedirect; } else { res->status = st; res->body = body; } return pnr::Status::Ok(); }; };
    check(run_with(tok(400, "{}")).error.code == kErrSchema || run_with(tok(400, "{}")).error.code != 0, "token: HTTP 400 is an error");
    check(run_with(tok(401, "{}")).error.code == kErrTokenRejected, "token: HTTP 401 is token-rejected");
    check(run_with(tok(200, "{\"x\":1}")).error.code == kErrSchema, "token: no access_token is schema-change");
    check(run_with(tok(200, "nope")).error.code == kErrBadJson, "token: invalid JSON is bad-json");
    check(run_with(tok(200, "{\"access_token\":\"" + std::string(9000, 'a') + "\"}")).error.code == kErrSchema, "token: an absurdly long access_token is refused");
  }

  // ---- library page failures and refresh
  {
    auto lib = [](int st, const std::string& body) { return [st, body](const HttpRequest& r, HttpResponse* res) { if (is_auth(r)) { res->status = 302; res->location = kRedirect; } else if (is_token(r)) { res->status = 200; res->body = kTokOk; } else { res->status = st; res->body = body; } return pnr::Status::Ok(); }; };
    check(run_with(lib(500, "")).error.code == kErrServer && rows.empty(), "library: 500 is server-error and returns nothing");
    check(run_with(lib(429, "")).error.code == kErrRateLimited, "library: 429 is rate-limited");
    check(run_with(lib(200, "garbage")).error.code == kErrBadJson, "library: invalid JSON is bad-json");
    check(run_with(lib(200, "{\"data\":{\"x\":1}}")).error.code == kErrSchema, "library: a changed shape is schema-change");
    check(run_with(lib(200, "{\"errors\":[{\"message\":\"nope\"}]}")).error.code == kErrSonyErrors, "library: GraphQL errors are sony-errors");
    check(run_with(lib(404, "")).error.code == kErrSchema, "library: an unexpected 404 is schema-change (API moved)");
    // mid-paging schema change returns nothing, not a partial list
    int n = 0;
    auto s = run_with([&](const HttpRequest& r, HttpResponse* res) { if (is_auth(r)) { res->status = 302; res->location = kRedirect; } else if (is_token(r)) { res->status = 200; res->body = kTokOk; } else { res->status = 200; res->body = n++ == 0 ? page(0, 2) : "{\"data\":{\"purchasedTitlesRetrieve\":null}}"; } return pnr::Status::Ok(); });
    check(s.error.code == kErrSchema && rows.empty(), "library: a failure on page 2 returns no partial result");
    // refresh
    int lib_calls = 0, refresh_calls = 0;
    Fake f;
    f.fn = [&](const HttpRequest& r, HttpResponse* res) {
      if (is_auth(r)) { res->status = 302; res->location = kRedirect; }
      else if (is_token(r)) { res->status = 200; if (has(r.body, "grant_type=refresh_token")) { ++refresh_calls; res->body = "{\"access_token\":\"ACCESS-2\",\"refresh_token\":\"REFRESH-2\"}"; } else res->body = kTokOk; }
      else { res->status = (++lib_calls == 1) ? 401 : 200; res->body = page(0, 1); }
      return pnr::Status::Ok();
    };
    auto rs = fetch_purchased_games(f, kTok, o, &rows, &rep);
    check(rs.ok && rows.size() == 1 && refresh_calls == 1 && rep.refreshes == 1, "refresh: a 401 mid-run refreshes once and retries");
    int bearer2 = 0; for (auto& r : f.log) for (auto& h : r.headers) if (h == "Authorization: Bearer ACCESS-2") ++bearer2;
    check(bearer2 == 1 && has(f.log[3].body, "refresh_token=REFRESH-1"), "refresh: the retry uses the new access token; the refresh used the first refresh token");
    int always401 = 0, rc2 = 0;
    auto s2 = run_with([&](const HttpRequest& r, HttpResponse* res) { if (is_auth(r)) { res->status = 302; res->location = kRedirect; } else if (is_token(r)) { res->status = 200; if (has(r.body, "refresh_token=")) ++rc2; res->body = kTokOk; } else { ++always401; res->status = 401; } return pnr::Status::Ok(); });
    check(s2.error.code == kErrTokenRejected && rc2 == 1 && always401 == 2, "refresh: a second 401 gives up as token-rejected (one refresh, no loop)");
    auto s3 = run_with([&](const HttpRequest& r, HttpResponse* res) { if (is_auth(r)) { res->status = 302; res->location = kRedirect; } else if (is_token(r)) { res->status = has(r.body, "refresh_token=") ? 400 : 200; res->body = has(r.body, "refresh_token=") ? "{}" : kTokOk; } else res->status = 401; return pnr::Status::Ok(); });
    check(!s3.ok && rows.empty(), "refresh: a failed refresh fails the sync with nothing returned");
    auto s4 = run_with([&](const HttpRequest& r, HttpResponse* res) { if (is_auth(r)) { res->status = 302; res->location = kRedirect; } else if (is_token(r)) { res->status = 200; res->body = "{\"access_token\":\"A\"}"; } else res->status = 401; return pnr::Status::Ok(); });
    check(s4.error.code == kErrTokenRejected, "refresh: no refresh token available is token-rejected");
  }

  // ---- command layer with a fake client
  {
    namespace fs = std::filesystem;
    const std::string d = "/tmp/pnr_psn_sync_" + std::to_string(::getpid());
    fs::remove_all(d); fs::create_directories(d);
    pnr::library::GameDb db;
    check(db.open(d + "/l.db").ok, "cmd_sync: scratch library opens");
    const std::string tp = d + "/psn/npsso.token";
    std::string out;
    Fake none = make_fake({0});
    check(cmd_sync(db, tp, false, false, &out, &none) == 1 && none.log.empty() && has(out, "token"), "cmd_sync: without a token nothing is sent");
    check(cmd_set_token(tp, kTok, false, &out) == 0, "cmd_sync: token stored for the test");
    Fake f = make_fake({50, 3});
    int rc = cmd_sync(db, tp, false, false, &out, &f);
    check(rc == 0 && has(out, "PSN SYNC OK") && has(out, "Untested against real Sony") && has(out, "added 53") && !has(out, kTok) && db.list_owned().size() == 53, "cmd_sync: merge sync adds 53 rows over 2 pages, says Untested, never prints the token");
    Fake g = make_fake({50, 0});
    rc = cmd_sync(db, tp, true, true, &out, &g);
    check(rc == 0 && has(out, "\"complete_applied\":true") && has(out, "\"marked_missing\":3") && db.list_owned().size() == 53, "cmd_sync: --complete marks the 3 absent rows, deletes nothing");
    Fake cap = make_fake({2, 2, 2, 2, 2, 2, 2, 2});
    Fake bad; bad.fn = [](const HttpRequest&, HttpResponse* r) { r->status = 200; return pnr::Status::Ok(); };
    const size_t before = db.list_owned().size();
    rc = cmd_sync(db, tp, true, false, &out, &bad);
    check(rc == 1 && has(out, "token-rejected") && has(out, "library is unchanged") && db.list_owned().size() == before && !has(out, kTok), "cmd_sync: a rejected token changes nothing and says what to do");
    fs::remove_all(d);
  }

  // ---- location parsing
  {
    std::string c;
    check(parse_code_from_location(kRedirect, &c) && c == "v3.SYNTHCODE", "location: code extracted");
    check(parse_code_from_location("x://r/?a=1&code=v3.Z-_9", &c) && c == "v3.Z-_9", "location: code after other parameters");
    for (const char* bad : {"", "x://r/", "x://r/?code=", "x://r/?code=a b", "x://r/?code=a%20b", "x://r/?codex=1", "x://r/?code=<script>"})
      check(!parse_code_from_location(bad, &c), std::string("location: rejected [") + bad + "]");
    check(!parse_code_from_location("x://r/?code=" + std::string(600, 'a'), &c), "location: oversize code rejected");
  }

  // ---- real libcurl backend against a local server (HTTP, test mode only)
  {
    pnr::Status st; CurlOptions co; co.allow_http_for_tests = true; co.timeout_seconds = 5; co.max_response_bytes = 1 << 20;
    auto c = make_curl_client(co, &st);
    if (!c) { std::printf("SKIP curl backend tests: %s\n", st.error.message.c_str()); }
    else {
      const std::string script =
          "import http.server,sys,json\n"
          "class H(http.server.BaseHTTPRequestHandler):\n"
          " def log_message(self,*a): pass\n"
          " def _go(self):\n"
          "  n=int(self.headers.get('Content-Length') or 0); b=self.rfile.read(n).decode() if n else ''\n"
          "  if self.path=='/redir':\n"
          "   self.send_response(302); self.send_header('Location','x://redirect/?code=v3.ABC'); self.send_header('Content-Length','0'); self.end_headers(); return\n"
          "  if self.path=='/big':\n"
          "   d=b'x'*(2<<20); self.send_response(200); self.send_header('Content-Length',str(len(d))); self.end_headers(); self.wfile.write(d); return\n"
          "  if self.path=='/404':\n"
          "   self.send_response(404); self.send_header('Content-Length','0'); self.end_headers(); return\n"
          "  d=json.dumps({'m':self.command,'p':self.path,'x':self.headers.get('X-Test'),'c':self.headers.get('Cookie'),'a':self.headers.get('Authorization'),'b':b,'ua':self.headers.get('User-Agent')}).encode()\n"
          "  self.send_response(200); self.send_header('Content-Length',str(len(d))); self.end_headers(); self.wfile.write(d)\n"
          " do_GET=_go; do_POST=_go\n"
          "s=http.server.HTTPServer(('127.0.0.1',0),H); print(s.server_address[1],flush=True); s.serve_forever()\n";
      const std::string sp = "/tmp/pnr_psn_srv_" + std::to_string(::getpid()) + ".py";
      { FILE* f = std::fopen(sp.c_str(), "w"); std::fputs(script.c_str(), f); std::fclose(f); }
      FILE* p = ::popen(("python3 " + sp + " 2>/dev/null & echo $! 1>&2; wait").c_str(), "r");
      char line[64] = {0};
      int port = (p && std::fgets(line, sizeof line, p)) ? std::atoi(line) : 0;
      if (port <= 0) { std::printf("SKIP curl backend tests: no python3 test server\n"); }
      else {
        const std::string base = "http://127.0.0.1:" + std::to_string(port);
        HttpResponse r;
        HttpRequest q; q.url = base + "/echo?a=1"; q.headers = {"X-Test: hello", "Cookie: npsso=zzz", "Authorization: Bearer t"};
        check(c->perform(q, &r).ok && r.status == 200 && has(r.body, "\"m\": \"GET\"") && has(r.body, "\"x\": \"hello\"") && has(r.body, "npsso=zzz") && has(r.body, "Bearer t") && has(r.body, "/echo?a=1") && has(r.body, "pnr-psn"), "curl: GET with custom headers, cookie and user agent");
        HttpRequest po; po.method = "POST"; po.url = base + "/p"; po.body = "k=v&z=1"; po.headers = {"Content-Type: application/x-www-form-urlencoded"};
        check(c->perform(po, &r).ok && has(r.body, "\"m\": \"POST\"") && has(r.body, "\"b\": \"k=v&z=1\""), "curl: POST body delivered");
        HttpRequest rd; rd.url = base + "/redir";
        check(c->perform(rd, &r).ok && r.status == 302 && r.location == "x://redirect/?code=v3.ABC", "curl: redirects are NOT followed and Location is captured");
        HttpRequest nf; nf.url = base + "/404";
        check(c->perform(nf, &r).ok && r.status == 404, "curl: HTTP status reported");
        HttpRequest bg; bg.url = base + "/big";
        auto bs = c->perform(bg, &r);
        check(!bs.ok && r.body.size() <= (1u << 20), "curl: a response over the size cap is refused");
        HttpRequest dead; dead.url = "http://127.0.0.1:1/x";
        check(c->perform(dead, &r).error.code == kErrTransport, "curl: connection refused is a transport error");
        // the production client refuses plain http outright
        CurlOptions prod; prod.timeout_seconds = 5; pnr::Status ps; auto pc = make_curl_client(prod, &ps);
        check(pc && pc->perform(q, &r).error.code == kErrTransport, "curl: a production client will not speak plain http");
        // verification: an https server with a self-signed certificate must fail
        std::string cmd = "openssl req -x509 -newkey rsa:2048 -nodes -keyout /tmp/pnr_psn_k.pem -out /tmp/pnr_psn_c.pem -days 1 -subj /CN=127.0.0.1 >/dev/null 2>&1";
        if (std::system(cmd.c_str()) == 0) {
          const std::string tls = "import http.server,ssl\nclass H(http.server.BaseHTTPRequestHandler):\n def log_message(self,*a): pass\n def do_GET(self):\n  self.send_response(200); self.send_header('Content-Length','2'); self.end_headers(); self.wfile.write(b'ok')\n"
                                  "s=http.server.HTTPServer(('127.0.0.1',0),H)\nx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); x.load_cert_chain('/tmp/pnr_psn_c.pem','/tmp/pnr_psn_k.pem')\ns.socket=x.wrap_socket(s.socket,server_side=True)\nprint(s.server_address[1],flush=True)\ns.serve_forever()\n";
          const std::string tp = "/tmp/pnr_psn_tls_" + std::to_string(::getpid()) + ".py";
          { FILE* f = std::fopen(tp.c_str(), "w"); std::fputs(tls.c_str(), f); std::fclose(f); }
          FILE* p2 = ::popen(("python3 " + tp + " & echo $! 1>&2; wait").c_str(), "r");
          char l2[64] = {0};
          int port2 = (p2 && std::fgets(l2, sizeof l2, p2)) ? std::atoi(l2) : 0;
          if (port2 > 0) {
            HttpRequest tq; tq.url = "https://127.0.0.1:" + std::to_string(port2) + "/";
            auto ts = c->perform(tq, &r);
            check(!ts.ok && ts.error.code == kErrTransport && r.status == 0, "curl: TLS verification is on (a self-signed certificate is refused)");
            (void)!std::system("pkill -f pnr_psn_tls_ 2>/dev/null");
          } else std::printf("SKIP tls verification test: no tls test server\n");
          if (p2) ::pclose(p2);
          std::remove(tp.c_str());
          std::remove("/tmp/pnr_psn_k.pem"); std::remove("/tmp/pnr_psn_c.pem");
        } else std::printf("SKIP tls verification test: openssl not available\n");
      }
      if (p) { (void)!std::system("pkill -f pnr_psn_srv_ 2>/dev/null"); ::pclose(p); }
      std::remove(sp.c_str());
    }
  }
}
