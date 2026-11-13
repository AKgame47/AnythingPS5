// libcurl loaded at run time through dlopen. The ABI constants below are
// declared here (no curl headers needed); each is exercised by tests that
// talk to a local server. GPL-2.0.
#include <dlfcn.h>

#include <cctype>
#include <cstdlib>
#include <cstring>

#include "psn/http.hpp"
#include "psn/psn.hpp"

namespace pnr::psn {
namespace {
using CURLcode = int;
constexpr int CURLE_OK = 0;
constexpr int CURLOPT_WRITEDATA = 10001, CURLOPT_URL = 10002, CURLOPT_POSTFIELDS = 10015, CURLOPT_USERAGENT = 10018,
              CURLOPT_HTTPHEADER = 10023, CURLOPT_HEADERDATA = 10029, CURLOPT_CUSTOMREQUEST = 10036,
              CURLOPT_WRITEFUNCTION = 20011, CURLOPT_HEADERFUNCTION = 20079, CURLOPT_TIMEOUT = 13,
              CURLOPT_FOLLOWLOCATION = 52, CURLOPT_POSTFIELDSIZE = 60, CURLOPT_SSL_VERIFYPEER = 64,
              CURLOPT_SSL_VERIFYHOST = 81, CURLOPT_CONNECTTIMEOUT = 78, CURLOPT_NOSIGNAL = 99,
              CURLOPT_PROTOCOLS_STR = 10318, CURLOPT_REDIR_PROTOCOLS_STR = 10319, CURLOPT_MAXFILESIZE = 114;
constexpr int CURLINFO_RESPONSE_CODE = 0x200000 + 2;
constexpr long CURL_GLOBAL_DEFAULT = 3;

struct Api {
  void* lib = nullptr;
  CURLcode (*global_init)(long) = nullptr;
  void* (*easy_init)() = nullptr;
  CURLcode (*easy_setopt)(void*, int, ...) = nullptr;
  CURLcode (*easy_perform)(void*) = nullptr;
  CURLcode (*easy_getinfo)(void*, int, ...) = nullptr;
  void (*easy_cleanup)(void*) = nullptr;
  void* (*slist_append)(void*, const char*) = nullptr;
  void (*slist_free_all)(void*) = nullptr;
  const char* (*easy_strerror)(CURLcode) = nullptr;
};

struct Ctx {
  HttpResponse* res;
  std::string* body = nullptr;  // buffered mode
  size_t cap = 0;
  bool over = false;
  const BodySink* sink = nullptr;  // streaming mode
  bool sink_refused = false;
  int code = 0;  // from the status line, so the body callback knows it
  size_t discarded = 0;
};
std::string trim(std::string v) {
  while (!v.empty() && (v.back() == '\r' || v.back() == '\n' || v.back() == ' ')) v.pop_back();
  size_t a = 0; while (a < v.size() && v[a] == ' ') ++a;
  return v.substr(a);
}
bool ieq_prefix(const std::string& line, const char* name) {
  const size_t n = std::strlen(name);
  if (line.size() < n) return false;
  for (size_t i = 0; i < n; ++i) if (std::tolower(static_cast<unsigned char>(line[i])) != name[i]) return false;
  return true;
}
size_t on_body(char* p, size_t sz, size_t n, void* ud) {
  auto* c = static_cast<Ctx*>(ud);
  const size_t len = sz * n;
  if (c->sink) {
    if (c->code == 200 || c->code == 206) {
      if (!(*c->sink)(*c->res, p, len)) { c->sink_refused = true; return 0; }
      return len;
    }
    c->discarded += len;  // error pages: drained, never kept
    if (c->discarded > (1u << 20)) { c->over = true; return 0; }
    return len;
  }
  if (c->body->size() + len > c->cap) { c->over = true; return 0; }
  c->body->append(p, len);
  return len;
}
size_t on_header(char* p, size_t sz, size_t n, void* ud) {
  auto* c = static_cast<Ctx*>(ud);
  const size_t len = sz * n;
  const std::string line(p, len);
  if (line.compare(0, 5, "HTTP/") == 0) {  // a new response (e.g. after 100-continue): forget older headers
    const size_t sp = line.find(' ');
    c->code = sp == std::string::npos ? 0 : std::atoi(line.c_str() + sp + 1);
    c->res->status = c->code;
    c->res->location.clear(); c->res->content_range.clear(); c->res->etag.clear(); c->res->content_length = -1;
  } else if (ieq_prefix(line, "location:")) c->res->location = trim(line.substr(9));
  else if (ieq_prefix(line, "content-range:")) c->res->content_range = trim(line.substr(14));
  else if (ieq_prefix(line, "etag:")) c->res->etag = trim(line.substr(5));
  else if (ieq_prefix(line, "content-length:")) {
    const std::string v = trim(line.substr(15));
    if (!v.empty() && v.size() < 19 && v.find_first_not_of("0123456789") == std::string::npos) c->res->content_length = std::atoll(v.c_str());
  }
  return len;
}

class CurlHttp : public HttpClient {
public:
  CurlHttp(Api a, CurlOptions o) : api_(a), opt_(o) {}
  ~CurlHttp() override { if (api_.lib) dlclose(api_.lib); }
  Status perform(const HttpRequest& req, HttpResponse* res) override { return run(req, res, nullptr); }
  Status download(const HttpRequest& req, const BodySink& sink, HttpResponse* res) override { return run(req, res, &sink); }
  Status run(const HttpRequest& req, HttpResponse* res, const BodySink* stream) {
    *res = HttpResponse{};
    // Checked here too: PROTOCOLS_STR needs curl >= 7.85, older libcurl ignores it.
    {
      const std::string u = req.url.size() > 8 ? req.url.substr(0, 8) : req.url;
      const bool https = u == "https://" || u == "HTTPS://";
      const bool http = req.url.compare(0, 7, "http://") == 0;
      if (!https && !(http && opt_.allow_http_for_tests)) return Status::Fail(kErrTransport, "psn", "refusing a non-https URL");
    }
    void* h = api_.easy_init();
    if (!h) return Status::Fail(kErrTransport, "psn", "curl init failed");
    void* hdrs = nullptr;
    for (const auto& x : req.headers) hdrs = api_.slist_append(hdrs, x.c_str());
    Ctx ctx; ctx.res = res; ctx.body = &res->body; ctx.cap = static_cast<size_t>(opt_.max_response_bytes); ctx.sink = stream;
    auto set = [&](int o, auto v) { api_.easy_setopt(h, o, v); };
    set(CURLOPT_URL, req.url.c_str());
    set(CURLOPT_NOSIGNAL, 1L);
    set(CURLOPT_FOLLOWLOCATION, 0L);  // redirects are handled (and checked) by the caller
    set(CURLOPT_SSL_VERIFYPEER, 1L);
    set(CURLOPT_SSL_VERIFYHOST, 2L);
    set(CURLOPT_PROTOCOLS_STR, opt_.allow_http_for_tests ? "http,https" : "https");
    set(CURLOPT_REDIR_PROTOCOLS_STR, "https");
    set(181 /*CURLOPT_PROTOCOLS*/, opt_.allow_http_for_tests ? 3L : 2L);  // legacy form for curl < 7.85
    set(182 /*CURLOPT_REDIR_PROTOCOLS*/, 2L);
    if (stream) {  // long transfers: no total timeout, but give up if the link stalls
      set(CURLOPT_TIMEOUT, 0L);
      set(19 /*CURLOPT_LOW_SPEED_LIMIT*/, 1024L);
      set(20 /*CURLOPT_LOW_SPEED_TIME*/, 30L);
    } else set(CURLOPT_TIMEOUT, opt_.timeout_seconds);
    set(CURLOPT_CONNECTTIMEOUT, 15L);
    set(CURLOPT_USERAGENT, "pnr-psn/1 (unofficial; local tool)");
    set(CURLOPT_WRITEFUNCTION, reinterpret_cast<void*>(&on_body));
    set(CURLOPT_WRITEDATA, static_cast<void*>(&ctx));
    set(CURLOPT_HEADERFUNCTION, reinterpret_cast<void*>(&on_header));
    set(CURLOPT_HEADERDATA, static_cast<void*>(&ctx));
    if (hdrs) set(CURLOPT_HTTPHEADER, hdrs);
    if (req.method != "GET") {
      set(CURLOPT_CUSTOMREQUEST, req.method.c_str());
      set(CURLOPT_POSTFIELDS, req.body.c_str());
      set(CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));
    }
    CURLcode rc = api_.easy_perform(h);
    long code = 0;
    if (rc == CURLE_OK) api_.easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    std::string why = api_.easy_strerror ? api_.easy_strerror(rc) : "error";
    if (hdrs) api_.slist_free_all(hdrs);
    api_.easy_cleanup(h);
    if (rc != CURLE_OK) {
      if (ctx.sink_refused) return Status::Fail(kErrTransport, "psn", "download stopped: the receiver refused more data (size limit or disk)");
      std::string m = ctx.over ? "response larger than the limit" : "transfer failed: " + scrub(why);
      return Status::Fail(ctx.over && !stream ? kErrSchema : kErrTransport, "psn", m);
    }
    res->status = static_cast<int>(code);
    return Status::Ok();
  }
private:
  Api api_;
  CurlOptions opt_;
};
}  // namespace

std::unique_ptr<HttpClient> make_curl_client(const CurlOptions& opt, Status* st) {
  Api a;
  for (const char* name : {"libcurl.so.4", "libcurl-gnutls.so.4"}) {
    a.lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
    if (a.lib) break;
  }
  if (!a.lib) { *st = Status::Fail(kErrTransport, "psn", "libcurl.so.4 not found"); return nullptr; }
  auto sym = [&](const char* n) { return dlsym(a.lib, n); };
  a.global_init = reinterpret_cast<decltype(a.global_init)>(sym("curl_global_init"));
  a.easy_init = reinterpret_cast<decltype(a.easy_init)>(sym("curl_easy_init"));
  a.easy_setopt = reinterpret_cast<decltype(a.easy_setopt)>(sym("curl_easy_setopt"));
  a.easy_perform = reinterpret_cast<decltype(a.easy_perform)>(sym("curl_easy_perform"));
  a.easy_getinfo = reinterpret_cast<decltype(a.easy_getinfo)>(sym("curl_easy_getinfo"));
  a.easy_cleanup = reinterpret_cast<decltype(a.easy_cleanup)>(sym("curl_easy_cleanup"));
  a.slist_append = reinterpret_cast<decltype(a.slist_append)>(sym("curl_slist_append"));
  a.slist_free_all = reinterpret_cast<decltype(a.slist_free_all)>(sym("curl_slist_free_all"));
  a.easy_strerror = reinterpret_cast<decltype(a.easy_strerror)>(sym("curl_easy_strerror"));
  if (!a.global_init || !a.easy_init || !a.easy_setopt || !a.easy_perform || !a.easy_getinfo || !a.easy_cleanup || !a.slist_append || !a.slist_free_all) {
    dlclose(a.lib);
    *st = Status::Fail(kErrTransport, "psn", "libcurl is missing expected functions");
    return nullptr;
  }
  a.global_init(CURL_GLOBAL_DEFAULT);
  *st = Status::Ok();
  return std::make_unique<CurlHttp>(a, opt);
}

}  // namespace pnr::psn
