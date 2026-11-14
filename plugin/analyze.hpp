// PNR plugin system - static analysis before install (slice P3), GPL-2.0.
// Reads a plugin directory and NEVER runs anything in it. It refuses:
//   - Sony SELF containers and other console formats (SELF magic, FreeBSD-ABI ELF as used by console payloads, .self/.sprx/.pup files),
//   - ps5-library-class payloads: piracy download providers (file-locker hosts, torrent/magnet links, direct-download catalogs),
//     dump catalogs of copyrighted game packages, and jailbreak/exploit payload loaders.
// These are HEURISTICS on file names, magic bytes and text. They catch the obvious cases; they are not a malware scanner and a
// clean result is NOT a safety guarantee (the sandbox and the permission grants are the real controls).
// Clean adapters (a PC-side plugin that handles the user's own lawful files) are what the system offers instead.
#pragma once
#include <string>
#include <vector>

namespace pnr::plugin {

struct Analysis {
  bool refused = false;
  std::vector<std::string> reasons;    // why it was refused, plain words (one per finding)
  std::vector<std::string> warnings;   // allowed but worth knowing (native x86-64 binary present, etc.)
  std::size_t files = 0, text_files_scanned = 0, native_binaries = 0;
};

Analysis analyze_dir(const std::string& dir);
// Pure helpers, exposed for tests.
bool is_console_binary_magic(const std::string& head, std::string* what);
std::vector<std::string> piracy_findings(const std::string& rel_path, const std::string& text);
// Hosts a plugin may never be granted net: access to (file lockers, torrent trackers).
bool is_refused_host(const std::string& host);

}  // namespace pnr::plugin
