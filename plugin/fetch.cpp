// PNR plugin system - install from a GitHub URL or package file (slice P3b), GPL-2.0. See fetch.hpp.
#include "plugin/fetch.hpp"

#include <unistd.h>

#include <atomic>
#include <vector>
#include <filesystem>
#include <fstream>

#include "plugin/archive.hpp"

namespace fs = std::filesystem;
namespace pnr::plugin {
namespace {
bool name_ok(const std::string& s) {
  if (s.empty() || s.size() > 100 || s == "." || s == "..") return false;
  for (char c : s) if (!(isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.')) return false;
  return true;
}
std::string stage_dir(const PluginManager& pm) {
  static std::atomic<int> n{0};
  return pm.root() + "/plugins/.stage-" + std::to_string(::getpid()) + "-" + std::to_string(n++);
}
InstallResult from_bytes_or_file(PluginManager& pm, const std::string* bytes, const std::string& path) {
  InstallResult r; std::error_code ec; std::string err;
  const std::string st = stage_dir(pm);
  fs::create_directories(pm.root() + "/plugins", ec);
  ArchiveLimits lim;
  bool ok = bytes ? extract_archive(*bytes, st, lim, &err) : extract_archive_file(path, st, lim, &err);
  if (!ok) { r.errors.push_back("package not installed: " + err); fs::remove_all(st, ec); return r; }
  r = pm.install_from_dir(st);
  fs::remove_all(st, ec);
  return r;
}
}  // namespace

bool parse_github_url(const std::string& url, GithubRef* out, std::string* why) {
  const std::string pre = "https://github.com/";
  if (url.compare(0, pre.size(), pre) != 0) { *why = "only https://github.com/<owner>/<repo> URLs are accepted"; return false; }
  std::string rest = url.substr(pre.size());
  if (rest.find_first_of("?#@\\ ") != std::string::npos) { *why = "the URL must be a plain repository address (no query, fragment or user-info)"; return false; }
  while (!rest.empty() && rest.back() == '/') rest.pop_back();
  std::vector<std::string> seg; std::size_t s = 0;
  while (s <= rest.size()) { std::size_t q = rest.find('/', s); if (q == std::string::npos) q = rest.size(); seg.push_back(rest.substr(s, q - s)); s = q + 1; }
  if (seg.size() != 2 && !(seg.size() == 4 && seg[2] == "tree")) { *why = "use https://github.com/<owner>/<repo> or .../tree/<ref>"; return false; }
  GithubRef g; g.owner = seg[0]; g.repo = seg[1];
  if (g.repo.size() > 4 && g.repo.compare(g.repo.size() - 4, 4, ".git") == 0) g.repo.resize(g.repo.size() - 4);
  g.ref = seg.size() == 4 ? seg[3] : "HEAD";
  if (!name_ok(g.owner) || !name_ok(g.repo) || !name_ok(g.ref)) { *why = "owner, repository or ref has characters GitHub does not use here"; return false; }
  *out = g; return true;
}

std::string github_archive_url(const GithubRef& g) { return "https://codeload.github.com/" + g.owner + "/" + g.repo + "/tar.gz/" + g.ref; }

InstallResult install_from_archive_file(PluginManager& pm, const std::string& path) { return from_bytes_or_file(pm, nullptr, path); }

InstallResult install_from_github(PluginManager& pm, psn::HttpClient& http, const std::string& url, std::size_t max_bytes) {
  InstallResult r; GithubRef g; std::string why;
  if (!parse_github_url(url, &g, &why)) { r.errors.push_back(why); return r; }
  psn::GuardedHttp guard(http, {"codeload.github.com"});
  psn::HttpRequest req; req.url = github_archive_url(g);
  std::string body; bool too_big = false; psn::HttpResponse res;
  auto sink = [&](const psn::HttpResponse&, const char* d, std::size_t n) { if (body.size() + n > max_bytes) { too_big = true; return false; } body.append(d, n); return true; };
  Status st = guard.download(req, sink, &res);
  if (too_big) { r.errors.push_back("download is larger than the " + std::to_string(max_bytes >> 20) + " MiB limit; nothing was installed"); return r; }
  if (!st.ok) { r.errors.push_back("download failed: " + st.error.message); return r; }
  if (res.status != 200) { r.errors.push_back("GitHub answered HTTP " + std::to_string(res.status) + " for " + g.owner + "/" + g.repo + " ref " + g.ref + "; nothing was installed"); return r; }
  return from_bytes_or_file(pm, &body, "");
}

}  // namespace pnr::plugin
