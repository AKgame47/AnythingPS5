// Minimal HTTP seam for the PSN transport. GPL-2.0.
// Policy: https only, exact-host allow-list, no automatic redirects, TLS
// verification always on, bounded response size. The guard runs BEFORE any
// bytes leave the machine, so credentials cannot be sent to another host.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/runtime.hpp"

namespace pnr::psn {

struct HttpRequest {
  std::string method = "GET";
  std::string url;
  std::vector<std::string> headers;  // "Name: value"
  std::string body;
};
struct HttpResponse {
  int status = 0;
  std::string body;
  std::string location;  // Location header, if any
  std::string content_range, etag;  // response headers, for resumable downloads
  long long content_length = -1;    // -1 = not sent
};
// Receives downloaded bytes with the response head (status, Content-Range,
// ETag, Content-Length) already filled in. Return false to abort (disk
// full, size limit, user cancel).
using BodySink = std::function<bool(const HttpResponse& head, const char* data, size_t len)>;

class HttpClient {
public:
  virtual ~HttpClient() = default;
  // Returns a non-ok Status only when no HTTP response was obtained.
  virtual Status perform(const HttpRequest& req, HttpResponse* res) = 0;
  // Streams a 200/206 body to `sink` (nothing is buffered, no size cap here:
  // the caller's sink enforces limits). Other statuses are drained and
  // discarded, with res->status set. Default: not supported.
  virtual Status download(const HttpRequest& req, const BodySink& sink, HttpResponse* res);
};

constexpr const char* kAuthHost = "ca.account.sony.com";
constexpr const char* kGraphqlHost = "web.np.playstation.com";

// Parses scheme://host[:port]/path. Rejects userinfo, empty host, non-ASCII,
// control characters. Port must be absent or 443.
bool url_host(const std::string& url, std::string* scheme, std::string* host);
// True only for https URLs whose host is in `allowed` (exact, case-insensitive).
bool url_allowed(const std::string& url, const std::vector<std::string>& allowed);

// Wraps a client; refuses any request the allow-list does not cover.
class GuardedHttp : public HttpClient {
public:
  GuardedHttp(HttpClient& inner, std::vector<std::string> allowed) : inner_(inner), allowed_(std::move(allowed)) {}
  Status perform(const HttpRequest& req, HttpResponse* res) override;
  Status download(const HttpRequest& req, const BodySink& sink, HttpResponse* res) override;
  int blocked = 0;
private:
  HttpClient& inner_;
  std::vector<std::string> allowed_;
};

struct CurlOptions {
  bool allow_http_for_tests = false;  // library-level only, never reachable from the CLI
  long timeout_seconds = 30;
  long max_response_bytes = 16 * 1024 * 1024;
};
// Loads libcurl at run time (no build dependency). Fails with kErrTransport
// if it is not installed. TLS peer and host verification are always on
// (except never: there is no option to disable them).
std::unique_ptr<HttpClient> make_curl_client(const CurlOptions& opt, Status* st);

}  // namespace pnr::psn
