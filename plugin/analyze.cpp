// PNR plugin system - static analysis (slice P3), GPL-2.0. See analyze.hpp for scope and honesty.
#include "plugin/analyze.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
namespace pnr::plugin {
namespace {
std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
bool has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }
const char* const kHosts[] = {"mega.nz", "mega.io", "mediafire.com", "1fichier.com", "pixeldrain.com", "gofile.io", "uploadhaven.com",
                              "rapidgator.net", "nitroflare.com", "uploaded.net", "zippyshare.com", "katfile.com", "thepiratebay.org",
                              "1337x.to", "nyaa.si", "rarbg.to", "cyberdrop.me", "bayfiles.com", "anonfiles.com"};
}  // namespace

bool is_refused_host(const std::string& host) {
  std::string h = lower(host);
  for (const char* k : kHosts) {
    std::string kk = k;
    if (h == kk || (h.size() > kk.size() && h.compare(h.size() - kk.size() - 1, std::string::npos, "." + kk) == 0)) return true;
  }
  return false;
}

bool is_console_binary_magic(const std::string& h, std::string* what) {
  if (h.size() >= 4 && (unsigned char)h[0] == 0x4F && (unsigned char)h[1] == 0x15 && (unsigned char)h[2] == 0x3D && (unsigned char)h[3] == 0x1D) {
    *what = "a Sony SELF container (not decrypted or run by PNR)"; return true;
  }
  // ELF with OS/ABI byte 9 = FreeBSD, the ABI console homebrew payloads use. A normal Linux ELF has 0 (SysV) or 3 (GNU).
  if (h.size() >= 8 && h.compare(0, 4, "\x7f" "ELF") == 0 && (unsigned char)h[7] == 9) { *what = "an ELF built for the FreeBSD ABI (console payload style); console binaries are never run directly"; return true; }
  return false;
}

std::vector<std::string> piracy_findings(const std::string& rel, const std::string& text_in) {
  std::vector<std::string> out;
  std::string t = lower(text_in), p = lower(rel);
  for (const char* k : kHosts) if (has(t, k)) { out.push_back(std::string("'") + rel + "' points at a file-locker or torrent host (" + k + "): download providers for copyrighted games are refused"); break; }
  if (has(t, "magnet:?")) out.push_back("'" + rel + "' contains a magnet link: torrent catalogs are refused");
  // Dump catalogs: lists of game packages meant to be fetched.
  int dumpish = 0;
  for (const char* k : {"fpkg", "pkg dump", "game dump", "dump list", "backport", "ps5 dumps", "ps4 dumps"}) if (has(t, k) || has(p, k)) ++dumpish;
  if (dumpish >= 1 && (has(t, "download") || has(t, "http"))) out.push_back("'" + rel + "' looks like a catalog of dumped game packages with download links: refused");
  // Jailbreak / exploit payload loaders.
  for (const char* k : {"kstuff", "etahen", "elfldr", "ps5-payload", "ps5 payload", "jailbreak", "kernel exploit", "webkit exploit", "pkg bypass", "fakepkg"})
    if (has(t, k)) { out.push_back("'" + rel + "' mentions a jailbreak/exploit payload (" + k + "): refused. PNR does not load them."); break; }
  return out;
}

Analysis analyze_dir(const std::string& dir) {
  Analysis a;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    ++a.files;
    std::string rel = fs::relative(it->path(), dir, ec).generic_string();
    std::string ext = lower(it->path().extension().string());
    std::ifstream in(it->path(), std::ios::binary);
    std::string head(16, '\0'); in.read(&head[0], 16); head.resize((std::size_t)in.gcount());
    std::string what;
    if (is_console_binary_magic(head, &what)) { a.refused = true; a.reasons.push_back("'" + rel + "' is " + what); continue; }
    if (ext == ".self" || ext == ".sprx" || ext == ".pup") { a.refused = true; a.reasons.push_back("'" + rel + "' has a console file extension (" + ext + ")"); continue; }
    if (head.size() >= 4 && head.compare(0, 4, "\x7f" "ELF") == 0) { ++a.native_binaries; a.warnings.push_back("'" + rel + "' is a native ELF binary; it can only run inside the sandbox"); continue; }
    if (head.size() >= 2 && head[0] == 'M' && head[1] == 'Z') { ++a.native_binaries; a.warnings.push_back("'" + rel + "' is a Windows PE binary; plugin execution on Windows is Blocked"); continue; }
    bool texty = ext == ".json" || ext == ".txt" || ext == ".md" || ext == ".js" || ext == ".py" || ext == ".sh" || ext == ".csv" || ext == ".yml" || ext == ".yaml" || ext == ".toml" || ext == ".xml" || ext == ".html" || ext.empty() || ext == ".cfg" || ext == ".ini";
    if (!texty) continue;
    std::string body(1u << 20, '\0'); in.clear(); in.seekg(0); in.read(&body[0], (std::streamsize)body.size()); body.resize((std::size_t)in.gcount());
    if (body.find('\0') != std::string::npos) continue;
    ++a.text_files_scanned;
    for (auto& f : piracy_findings(rel, body)) { a.refused = true; a.reasons.push_back(f); }
  }
  return a;
}

}  // namespace pnr::plugin
