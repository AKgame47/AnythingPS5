// PNR Unofficial Store, core (slice A1): pluggable sources, strict index
// validation, local search. GPL-2.0.
//
// Content-neutral mechanism like a package manager's custom repositories:
// the store starts with NO sources. Every source is added by the user with
// an https index URL and a rights statement. PNR cannot verify rights
// statements; the user is responsible for the sources they add.
// Downloads and install are in store/download.hpp. Signatures: Planned.
#pragma once
#include <string>
#include <vector>

#include "core/runtime.hpp"
#include "psn/http.hpp"

namespace pnr::store {

constexpr int kErrSignature = 305;
constexpr int kErrBadSource = 300, kErrBadIndex = 301, kErrNoSource = 302, kErrDuplicate = 303, kErrIo = 304;

struct Source {
  std::string id;         // [a-z0-9][a-z0-9._-]{0,39}
  std::string index_url;  // https, host, no userinfo, port 443
  std::string rights;     // declared by the user; PNR does not verify it
  std::string host;       // derived
  std::string pubkey;     // optional pinned Ed25519 public key, 64 lowercase hex (slice B1)
};

struct Package {
  std::string source_id;
  std::string id, name, version, description, license, rights;
  std::string category, kind, url, sha256;
  long long size = 0;
  bool experimental = false;
  std::string signature;  // optional Ed25519 signature, 128 lowercase hex (slice B1)
};
// Message a source signs for one package (binds source, id, version, hash, size).
std::string signed_message(const std::string& source_id, const Package& p);
// Signature policy. Source without a pinned key: ok, mode "unsigned (hash only)".
// Source with a pinned key: the package MUST carry a signature that verifies, else failure.
Status check_signature(const Source& s, const Package& p, std::string* mode);
// Derived by PNR, never taken from the index.
//   "supported":   kind == "pnr-package" (the only format PNR can install)
//   "experimental": same kind, index marks it experimental
//   "unsupported": any other kind: listed, cannot be installed by PNR
std::string label_for(const Package& p);

struct Index {
  std::string name;
  std::string rights;  // source-level declared rights statement
  std::vector<Package> packages;
};

// Validation. A bad index is rejected whole, with the reason.
bool valid_source_id(const std::string& id);
Status parse_index(const std::string& text, const Source& src, Index* out);

class Registry {
public:
  explicit Registry(std::string dir) : dir_(std::move(dir)) {}
  Status load();  // missing file = empty registry
  const std::vector<Source>& sources() const { return sources_; }
  const std::string& dir() const { return dir_; }
  const Source* find(const std::string& id) const;
  Status set_pubkey(const std::string& id, const std::string& hex);  // "" unpins
  Status add(const std::string& id, const std::string& url, const std::string& rights);
  Status remove(const std::string& id, bool* existed);  // also deletes its cached index
  std::string index_path(const std::string& id) const { return dir_ + "/index/" + id + ".json"; }
  Status save_index_text(const std::string& id, const std::string& text);  // atomic
  Status load_index(const std::string& id, Index* out);                    // re-validated on every read
private:
  Status save();
  std::string dir_;
  std::vector<Source> sources_;
};

std::string store_dir_for(const std::string& db_path);  // PNR_STORE_DIR or <db dir>/store

// Fetches and validates one source's index; replaces the cache only if valid.
Status refresh_source(psn::HttpClient& http, Registry& reg, const std::string& id, int* packages);

}  // namespace pnr::store
