// PSN account connector, slice 1: OFFLINE core. GPL-2.0.
//
// What exists here: a local NPSSO token store (owner-only file, never
// printed), secret scrubbing, an error taxonomy, and a parser that turns a
// saved "purchased games" response into owned-on-PSN entitlement rows.
// What does NOT exist here: any network code. Nothing in this module
// contacts Sony. The response shape follows the MIT-licensed psn-api
// library's published PurchasedGamesResponse type
// (data.purchasedTitlesRetrieve.games[]); it has been exercised only
// against synthetic fixtures, never against live Sony output.
//
// Owning a purchase (ACCESS) is not permission to download it and is not
// PC compatibility. This module records access metadata only.
#pragma once
#include <string>
#include <vector>

#include "core/runtime.hpp"
#include "library/game_db.hpp"

namespace pnr::psn {

// Error codes (Status.error.code, subsystem "psn").
enum : int {
  kErrNoToken = 200,        // no token stored
  kErrTokenFormat = 201,    // input is not a 64-char alphanumeric NPSSO value
  kErrTokenPerms = 202,     // token file is readable by others or not a regular file
  kErrTokenIo = 203,        // could not read/write/remove the token file
  kErrTokenRejected = 210,  // Sony answered 401/403: token expired or revoked
  kErrRateLimited = 211,    // HTTP 429
  kErrServer = 212,         // HTTP 5xx
  kErrNetwork = 213,        // no response at all (reserved for the transport slice)
  kErrTransport = 214,      // libcurl missing, TLS verification failed, or other transport failure
  kErrBlockedHost = 215,    // URL refused by the host allow-list (not Sony's official hosts, or not https)
  kErrBadJson = 220,        // response is not valid JSON
  kErrSchema = 221,         // valid JSON, not the expected shape (API change?)
  kErrSonyErrors = 222,     // response carried a GraphQL errors array and no data
  kErrBadInput = 223,       // file unreadable, too large
  kErrLibrary = 224,        // the local library database could not be written
  kErrConsent = 225,        // live sync not confirmed (F1 warning gate); nothing was sent
};

// Short stable name for a code, e.g. "token-rejected".
const char* error_kind(int code);
// One-line next step for a person, per code (no secrets).
const char* error_advice(int code);

// ---- token store ---------------------------------------------------------
struct TokenInfo {
  bool exists = false;
  bool regular = false;       // regular file, not a symlink or directory
  bool perms_ok = false;      // POSIX: no group/other bits. Windows: always false (not checked)
  bool perms_checked = false; // false on Windows
  bool format_ok = false;     // contents are a valid-looking NPSSO (value never exposed)
  long long bytes = 0;
};

// Accepts the bare 64-char value or the JSON a person copies from
// ca.account.sony.com/api/v1/ssocookie ({"npsso":"..."}). Writes
// atomically with mode 0600 (POSIX). Never echoes the input in errors.
Status token_save(const std::string& path, const std::string& pasted);
Status token_load(const std::string& path, std::string* token);  // refuses loose permissions
TokenInfo token_info(const std::string& path);
// Overwrites the file with zeros, then deletes it. Absent file: ok.
Status token_remove(const std::string& path);
// Pull the token out of pasted text without validating the file system.
bool token_extract(const std::string& pasted, std::string* token);

// Replaces every occurrence of `token` (if non-empty) with "[redacted]".
std::string redact(const std::string& text, const std::string& token);
// Replaces any run of 32+ letters/digits with "[redacted]". Used on every
// message shown from Sony-controlled text.
std::string scrub(const std::string& text);

// ---- Sony response handling ---------------------------------------------
// Maps an HTTP status (and optionally body) to a Status. 2xx returns Ok.
Status classify_http(int http_status, const std::string& body);

struct ParseReport {
  int games_in_response = 0;
  int accepted = 0;
  int skipped_invalid = 0;    // missing entitlementId, control chars, oversize fields
  int duplicates = 0;         // same entitlementId twice in one response (last wins)
  int dropped_image_urls = 0; // non-https image URLs removed
  std::vector<std::string> notes;  // bounded, scrubbed
};

// Parses a saved purchased-games response. Never invents fields. DLC,
// editions, prices and purchase dates are NOT in this endpoint's shape and
// are therefore not produced.
Status parse_purchased_games(const std::string& json_text,
                             std::vector<library::OwnedRecord>* out, ParseReport* rep);

// Text shown for an owned row's state: its own state, or the linked
// game's compatibility state. `linked_state` empty means not linked.
std::string owned_state_label(const library::OwnedRecord& r, const std::string& linked_state);

}  // namespace pnr::psn
