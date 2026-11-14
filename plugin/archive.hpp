// PNR plugin system - archive reading (slice P3b), GPL-2.0.
// Reads a plugin package (.tar, or gzip-compressed .tar.gz/.tgz such as a GitHub source archive) fully in memory with hard limits
// and writes it into a new directory. Own inflate (RFC 1951) and ustar/pax reader, so there is no zlib dependency.
// Only regular files and directories are accepted. A symlink, hard link, device, absolute path, ".." or backslash path is refused
// and nothing is written. A single top-level folder (GitHub style "owner-repo-sha/") is stripped. Implemented, Tested on fixtures.
#pragma once
#include <string>

namespace pnr::plugin {

struct ArchiveLimits {
  std::size_t max_files = 2000;
  std::size_t max_total_bytes = 64u << 20;   // decompressed, also the zip-bomb guard
  bool skip_hidden = true;                   // drop .git, .github, .gitignore ... instead of failing; they are never installed
};

// Decompresses a gzip stream. False + *err on any malformed input or when the output would pass max_out.
bool gunzip(const std::string& in, std::string* out, std::size_t max_out, std::string* err);
// Extracts tar or tar.gz bytes into dest (must not exist yet). On any problem dest is removed and false is returned.
bool extract_archive(const std::string& bytes, const std::string& dest, const ArchiveLimits& lim, std::string* err);
bool extract_archive_file(const std::string& path, const std::string& dest, const ArchiveLimits& lim, std::string* err);

}  // namespace pnr::plugin
