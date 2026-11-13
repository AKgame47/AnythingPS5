#include "psn/http.hpp"

#include <algorithm>
#include <cctype>

#include "psn/psn.hpp"

namespace pnr::psn {

bool url_host(const std::string& url, std::string* scheme, std::string* host) {
  for (unsigned char c : url) if (c < 0x21 || c > 0x7e) return false;
  const size_t se = url.find("://");
  if (se == std::string::npos || se == 0) return false;
  std::string sc = url.substr(0, se);
  std::transform(sc.begin(), sc.end(), sc.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  size_t a = se + 3, b = url.find_first_of("/?#", a);
  std::string auth = url.substr(a, b == std::string::npos ? std::string::npos : b - a);
  if (auth.empty() || auth.find('@') != std::string::npos || auth.find('[') != std::string::npos) return false;
  const size_t colon = auth.find(':');
  std::string h = auth.substr(0, colon);
  if (colon != std::string::npos) {
    const std::string port = auth.substr(colon + 1);
    if (port != "443") return false;
  }
  if (h.empty() || h.back() == '.' ) return false;
  std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  for (char c : h) if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-')) return false;
  *scheme = sc; *host = h;
  return true;
}

bool url_allowed(const std::string& url, const std::vector<std::string>& allowed) {
  std::string sc, h;
  if (!url_host(url, &sc, &h) || sc != "https") return false;
  for (const auto& a : allowed) {
    std::string l = a;
    std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (l == h) return true;
  }
  return false;
}

Status HttpClient::download(const HttpRequest&, const BodySink&, HttpResponse*) {
  return Status::Fail(kErrTransport, "psn", "this HTTP client cannot stream downloads");
}

Status GuardedHttp::download(const HttpRequest& req, const BodySink& sink, HttpResponse* res) {
  if (!url_allowed(req.url, allowed_)) {
    ++blocked;
    return Status::Fail(kErrBlockedHost, "psn", "request refused by the host allow-list");
  }
  return inner_.download(req, sink, res);
}

Status GuardedHttp::perform(const HttpRequest& req, HttpResponse* res) {
  if (!url_allowed(req.url, allowed_)) {
    ++blocked;
    return Status::Fail(kErrBlockedHost, "psn", "request refused by the host allow-list");
  }
  return inner_.perform(req, res);
}

}  // namespace pnr::psn
