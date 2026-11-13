// Store downloads (slice A2): queue, resumable verified transfers, install
// preparation. GPL-2.0.
//
// Rules: https only, only the source's own host, no redirects, bytes go to
// <store>/downloads/<source>/<package>.part, the size and SHA-256 from the
// source's (validated) index are enforced BEFORE the file is called
// verified, and only a verified file can reach the installer. A hash from
// the index proves integrity against corruption, not against a malicious
// source (signatures are Planned).
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "store/store.hpp"

namespace pnr::store {

constexpr int kErrDownload = 305, kErrHashMismatch = 306, kErrDisk = 307, kErrNotVerified = 308, kErrUnsupportedKind = 309, kErrInterrupted = 310;

struct DlState {
  std::string source, pkg;
  std::string status;  // queued | paused | partial | verified | failed | installed
  std::string sha256, etag, error;
  long long size = 0, have = 0;  // have = bytes in the .part (or final) file now
};

// Slice B1b: provenance record written at install, re-checked before every launch.
// <store>/provenance/<game_id>.prov, lines of key=value. Local file: it does not
// defend against someone who can already rewrite your store directory.
Status check_provenance(const std::string& store_dir, Registry& reg, const std::string& game_id, std::string* note);

struct DownloadOptions {
  int max_retries = 3;
  int backoff_ms = 1000;                     // doubled per retry
  long long free_margin = 16LL << 20;        // required free space beyond the remaining bytes
  std::function<bool()> cancel;              // polled during transfer; true = stop, keep the .part
  std::function<void(long long, long long)> progress;  // have, total
};

class Queue {
public:
  Queue(Registry& reg, std::string store_dir) : reg_(reg), dir_(std::move(store_dir)) {}
  // Adds (or returns) the entry. Only packages whose label is supported or
  // experimental can be queued; 'unsupported' ones are refused.
  Status enqueue(const std::string& source, const std::string& pkg, DlState* out);
  Status list(std::vector<DlState>* out);
  Status set_paused(const std::string& source, const std::string& pkg, bool paused);
  Status cancel(const std::string& source, const std::string& pkg);  // deletes .part, .state and an unverified file
  // Downloads, resumes, verifies. On hash mismatch the data is deleted.
  Status run(psn::HttpClient& http, const std::string& source, const std::string& pkg, const DownloadOptions& opt, DlState* out);
  // Re-hashes the verified file and returns its path; only for supported/experimental packages.
  Status prepare_install(const std::string& source, const std::string& pkg, std::string* path, std::string* sha);
  Status mark_installed(const std::string& source, const std::string& pkg, const std::string& game_id);
  std::string part_path(const std::string& s, const std::string& p) const { return dir_ + "/downloads/" + s + "/" + p + ".part"; }
  std::string final_path(const std::string& s, const std::string& p) const { return dir_ + "/downloads/" + s + "/" + p + ".pnrgame"; }
  std::string state_path(const std::string& s, const std::string& p) const { return dir_ + "/downloads/" + s + "/" + p + ".state"; }
private:
  Status load_state(const std::string& s, const std::string& p, DlState* st, bool* exists);
  Status save_state(const DlState& st);
  Status find_pkg(const std::string& s, const std::string& p, Package* out);
  Registry& reg_;
  std::string dir_;
};

// SHA-256 of a file, streamed. Fails if unreadable.
Status hash_file(const std::string& path, std::string* hex, long long* bytes);

}  // namespace pnr::store
