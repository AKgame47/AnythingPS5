// PNR research tools (slice C1). GPL-2.0.
//
//  * Research log: an append-only, hash-chained text log of what you fetched,
//    inspected or noted. The chain makes casual edits, deletions and
//    reordering DETECTABLE (`research log verify`). It is NOT tamper-proof:
//    anyone who can rewrite the whole file can recompute the chain.
//  * Generic fetch: one user-supplied https URL into one file. https only,
//    exact host of the URL, no redirects, resumable (.part + Range), optional
//    expected SHA-256 that is enforced. You must state a rights statement; it
//    is recorded, never verified. No default sources. Sony hosts are refused:
//    PNR has no authorized way to download Sony content.
//  * Inspect: read-only identification of PNR's own formats (.pnrgame
//    packages, x86-64 ELF, store index JSON) plus size, SHA-256 and entropy.
//    Sony package containers are recognised by their 4-byte magic and NOT
//    opened, decrypted or parsed.
#pragma once
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "psn/http.hpp"

namespace pnr::research {

constexpr int kErrBadInput = 330, kErrLogIO = 331, kErrLogCorrupt = 332, kErrFetch = 333, kErrHash = 334, kErrRefusedHost = 335, kErrInspect = 336;

struct LogEntry {
  long long seq = 0;
  std::string time, kind, subject, text, sha256, prev, hash;
};

class Log {
public:
  explicit Log(std::string dir) : dir_(std::move(dir)) {}
  std::function<std::string()> clock;  // UTC ISO-8601; default is the system clock (tests inject)
  std::string path() const { return dir_ + "/log.jsonl"; }
  // kind: note | fetch | fetch-failed | inspect. subject: url, path or empty. sha256: empty or 64 hex.
  Status append(const std::string& kind, const std::string& subject, const std::string& text, const std::string& sha256, LogEntry* out = nullptr);
  Status read(std::vector<LogEntry>* out);   // fails if the chain is broken
  // Checks every line: hash, prev link, sequence. detail names the first bad line.
  Status verify(size_t* entries, std::string* detail);
private:
  std::string dir_;
};

struct FetchOptions {
  std::string rights;        // required, declared, unverifiable
  std::string expect_sha256; // optional; enforced when present
  long long max_bytes = 2LL << 30;
  long long free_margin = 16LL << 20;
  int max_retries = 3, backoff_ms = 1000;
  bool force = false;        // allow replacing an existing output file
};
struct FetchResult {
  std::string path, sha256;
  long long bytes = 0;
  bool hash_verified = false;  // true only when an expected hash was supplied and matched
  bool size_declared = false;  // the server declared a total size and it matched
  bool resumed = false;
};
Status fetch(psn::HttpClient& http, Log& log, const std::string& url, const std::string& out_path, const FetchOptions& opt, FetchResult* res);

struct Inspection {
  std::string path, kind, sha256;
  long long size = 0;
  double entropy = 0;  // Shannon bits per byte over the first 1 MiB
  std::string magic_hex;
  bool sony_container = false;
  std::vector<std::pair<std::string, std::string>> facts;
  std::vector<std::string> notes;
};
Status inspect_file(const std::string& path, Inspection* out);
std::string render_inspection(const Inspection& i, bool json);

// `pnr-launcher research ...`. pos excludes the word "research". Returns the exit code.
struct CliOptions { std::string rights, expect_sha256, limit = "20"; bool json = false, force = false, log_it = false; };
int cmd_research(const std::vector<std::string>& pos, const CliOptions& o, const std::string& research_dir, std::string* out, psn::HttpClient* client = nullptr, const FetchOptions* fetch_override = nullptr);

}  // namespace pnr::research
