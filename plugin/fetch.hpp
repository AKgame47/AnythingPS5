// PNR plugin system - install from a GitHub URL or a package file (slice P3b), GPL-2.0.
// Every route ends in the SAME analysis and signature check as a local directory (PluginManager::install_from_dir).
//   package file: .tar / .tar.gz / .tgz, read in memory with hard limits (see archive.hpp). Implemented, Tested on fixtures.
//   GitHub URL:   https://github.com/<owner>/<repo>[/tree/<ref>] -> https://codeload.github.com/<owner>/<repo>/tar.gz/<ref>.
//                 Fetched through psn::GuardedHttp restricted to codeload.github.com, https only, size capped.
//                 EXPERIMENTAL: the URL mapping is tested offline; no test ever contacts GitHub, and the live fetch has not been run.
//                 With no /tree/<ref>, the ref "HEAD" is used (unverified against GitHub).
// Plugins from a repository are untrusted like any other. Installing grants nothing.
#pragma once
#include <string>

#include "plugin/manager.hpp"
#include "psn/http.hpp"

namespace pnr::plugin {

struct GithubRef { std::string owner, repo, ref; };
// False for anything but a plain github.com repo URL (user-info, ports, other hosts, extra path parts, odd characters).
bool parse_github_url(const std::string& url, GithubRef* out, std::string* why);
std::string github_archive_url(const GithubRef& g);

InstallResult install_from_archive_file(PluginManager& pm, const std::string& path);
// http must be able to reach codeload.github.com; it is wrapped in a guard that allows only that host.
InstallResult install_from_github(PluginManager& pm, psn::HttpClient& http, const std::string& url, std::size_t max_bytes = 32u << 20);

}  // namespace pnr::plugin
