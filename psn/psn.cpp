#include "psn/psn.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <set>

#include "psn/json.hpp"

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <filesystem>

namespace pnr::psn {
namespace fs = std::filesystem;

const char* error_kind(int c) {
  switch (c) {
    case kErrNoToken: return "no-token";
    case kErrTokenFormat: return "token-format";
    case kErrTokenPerms: return "token-permissions";
    case kErrTokenIo: return "token-io";
    case kErrTokenRejected: return "token-rejected";
    case kErrRateLimited: return "rate-limited";
    case kErrServer: return "server-error";
    case kErrNetwork: return "network";
    case kErrTransport: return "transport";
    case kErrBlockedHost: return "blocked-host";
    case kErrBadJson: return "bad-json";
    case kErrSchema: return "schema-change";
    case kErrSonyErrors: return "sony-errors";
    case kErrBadInput: return "bad-input";
    case kErrLibrary: return "library-write";
    default: return "unknown";
  }
}

const char* error_advice(int c) {
  switch (c) {
    case kErrNoToken: return "no token stored: run 'psn set-token' with your own NPSSO value";
    case kErrTokenFormat: return "that is not a 64-character NPSSO value; see docs/psn.md";
    case kErrTokenPerms: return "token file is not owner-only; fix the permissions or run 'psn disconnect' then 'psn set-token'";
    case kErrTokenIo: return "could not access the token file; check the path and permissions";
    case kErrTokenRejected: return "Sony rejected the token (expired or revoked, they last about two months): get a fresh NPSSO and run 'psn set-token'";
    case kErrRateLimited: return "Sony is rate limiting: wait and try again later";
    case kErrServer: return "Sony server error: try again later";
    case kErrNetwork: return "no response from Sony: check your connection";
    case kErrTransport: return "could not talk to Sony: libcurl (libcurl.so.4) may be missing, or TLS verification failed; PNR never turns verification off";
    case kErrBlockedHost: return "refused: PNR only talks to Sony's official hosts over https";
    case kErrBadJson: return "the response is not valid JSON";
    case kErrSchema: return "the response is not the expected shape: Sony may have changed the API, nothing was imported";
    case kErrSonyErrors: return "Sony reported errors and returned no data: nothing was imported";
    case kErrBadInput: return "could not read the input file";
    case kErrLibrary: return "the local library could not be written: nothing was imported; check disk space and permissions";
    default: return "";
  }
}

// ---- token ---------------------------------------------------------------
static bool is_alnum(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }
static bool valid_token(const std::string& t) {
  if (t.size() != 64) return false;
  for (char c : t) if (!is_alnum(c)) return false;
  return true;
}
static std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

bool token_extract(const std::string& pasted, std::string* token) {
  std::string t = trim(pasted);
  if (t.size() > 4096) return false;
  if (!t.empty() && t[0] == '{') {
    Json j; std::string e;
    if (!json_parse(t, &j, &e)) return false;
    const Json* n = j.get("npsso");
    if (!n || !n->is(Json::T::Str)) return false;
    t = n->s;
  }
  if (!valid_token(t)) return false;
  *token = t;
  return true;
}

Status token_save(const std::string& path, const std::string& pasted) {
  std::string tok;
  if (!token_extract(pasted, &tok))
    return Status::Fail(kErrTokenFormat, "psn", "input is not a 64-character NPSSO value");
  std::error_code ec;
  const fs::path p(path);
  if (p.has_parent_path()) {
    fs::create_directories(p.parent_path(), ec);
#ifndef _WIN32
    if (!ec) ::chmod(p.parent_path().c_str(), 0700);
#endif
  }
  if (ec) return Status::Fail(kErrTokenIo, "psn", "cannot create the token directory");
  const std::string tmp = path + ".tmp";
#ifndef _WIN32
  ::unlink(tmp.c_str());
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  if (fd < 0) return Status::Fail(kErrTokenIo, "psn", "cannot create the token file");
  ::fchmod(fd, 0600);
  const std::string data = tok + "\n";
  ssize_t w = ::write(fd, data.data(), data.size());
  bool ok = w == static_cast<ssize_t>(data.size()) && ::fsync(fd) == 0;
  ::close(fd);
  if (!ok) { ::unlink(tmp.c_str()); return Status::Fail(kErrTokenIo, "psn", "cannot write the token file"); }
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return Status::Fail(kErrTokenIo, "psn", "cannot move the token file into place");
  }
#else
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    f << tok << "\n";
    if (!f) return Status::Fail(kErrTokenIo, "psn", "cannot write the token file");
  }
  fs::remove(path, ec);
  fs::rename(tmp, path, ec);
  if (ec) return Status::Fail(kErrTokenIo, "psn", "cannot move the token file into place");
#endif
  return Status::Ok();
}

TokenInfo token_info(const std::string& path) {
  TokenInfo ti;
#ifndef _WIN32
  struct stat st;
  if (::lstat(path.c_str(), &st) != 0) return ti;
  ti.exists = true;
  ti.regular = S_ISREG(st.st_mode);
  ti.perms_checked = true;
  ti.perms_ok = ti.regular && (st.st_mode & 077) == 0 && st.st_uid == ::geteuid();
  ti.bytes = static_cast<long long>(st.st_size);
#else
  std::error_code ec;
  const auto s = fs::symlink_status(path, ec);
  if (ec || !fs::exists(s)) return ti;
  ti.exists = true;
  ti.regular = fs::is_regular_file(s);
  ti.perms_checked = false;
  ti.bytes = ti.regular ? static_cast<long long>(fs::file_size(path, ec)) : 0;
#endif
  if (ti.regular && ti.bytes > 0 && ti.bytes <= 4096) {
    std::ifstream f(path, std::ios::binary);
    std::string c((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    ti.format_ok = valid_token(trim(c));
  }
  return ti;
}

Status token_load(const std::string& path, std::string* token) {
  TokenInfo ti = token_info(path);
  if (!ti.exists) return Status::Fail(kErrNoToken, "psn", "no token stored");
  if (!ti.regular) return Status::Fail(kErrTokenPerms, "psn", "token path is not a regular file");
  if (ti.perms_checked && !ti.perms_ok)
    return Status::Fail(kErrTokenPerms, "psn", "token file is not owner-only (mode must be 0600)");
  if (ti.bytes > 4096) return Status::Fail(kErrTokenFormat, "psn", "token file has an unexpected size");
  std::ifstream f(path, std::ios::binary);
  if (!f) return Status::Fail(kErrTokenIo, "psn", "cannot read the token file");
  std::string c((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  c = trim(c);
  if (!valid_token(c)) return Status::Fail(kErrTokenFormat, "psn", "stored token does not look like an NPSSO value");
  *token = c;
  return Status::Ok();
}

Status token_remove(const std::string& path) {
  std::error_code ec;
  const auto s = fs::symlink_status(path, ec);
  if (ec || !fs::exists(s)) return Status::Ok();
  if (fs::is_regular_file(s)) {
    const auto n = fs::file_size(path, ec);
    if (!ec && n > 0 && n <= 4096) {
      std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
      if (f) { std::string z(static_cast<size_t>(n), '\0'); f.write(z.data(), static_cast<std::streamsize>(z.size())); f.flush(); }
    }
  }
  if (!fs::remove(path, ec) || ec) return Status::Fail(kErrTokenIo, "psn", "cannot delete the token file");
  return Status::Ok();
}

std::string redact(const std::string& text, const std::string& token) {
  if (token.empty()) return text;
  std::string out;
  size_t i = 0;
  while (i < text.size()) {
    if (text.compare(i, token.size(), token) == 0) { out += "[redacted]"; i += token.size(); }
    else out += text[i++];
  }
  return out;
}

std::string scrub(const std::string& text) {
  std::string out;
  size_t i = 0;
  while (i < text.size()) {
    if (is_alnum(text[i])) {
      size_t j = i;
      while (j < text.size() && is_alnum(text[j])) ++j;
      out += (j - i >= 32) ? std::string("[redacted]") : text.substr(i, j - i);
      i = j;
    } else out += text[i++];
  }
  return out;
}

// ---- Sony responses -------------------------------------------------------
Status classify_http(int code, const std::string&) {
  if (code >= 200 && code < 300) return Status::Ok();
  std::string num = std::to_string(code);
  std::string m;
  if (code == 401 || code == 403) { m.append("Sony rejected the credentials (HTTP ").append(num).append(")"); return Status::Fail(kErrTokenRejected, "psn", m); }
  if (code == 429) { m.append("Sony rate limit (HTTP 429)"); return Status::Fail(kErrRateLimited, "psn", m); }
  if (code >= 500 && code < 600) { m.append("Sony server error (HTTP ").append(num).append(")"); return Status::Fail(kErrServer, "psn", m); }
  if (code <= 0) { m.append("no HTTP response"); return Status::Fail(kErrNetwork, "psn", m); }
  m.append("unexpected HTTP status ").append(num);
  return Status::Fail(kErrSchema, "psn", m);
}

namespace {
constexpr size_t kMaxField = 512;
constexpr size_t kMaxGames = 100000;
bool clean_str(const Json* j, std::string* out, bool* bad) {
  if (!j || j->is(Json::T::Null)) { out->clear(); return false; }
  if (!j->is(Json::T::Str)) { *bad = true; return false; }
  if (j->s.size() > kMaxField) { *bad = true; return false; }
  for (unsigned char c : j->s) if (c < 0x20 || c == 0x7f) { *bad = true; return false; }
  *out = j->s;
  return true;
}
bool clean_bool(const Json* j, bool* out, bool* bad) {
  if (!j || j->is(Json::T::Null)) { *out = false; return false; }
  if (!j->is(Json::T::Bool)) { *bad = true; return false; }
  *out = j->b;
  return true;
}
void note(ParseReport* r, const std::string& n) {
  if (r->notes.size() < 20) r->notes.push_back(scrub(n.substr(0, 200)));
}
}  // namespace

Status parse_purchased_games(const std::string& text, std::vector<library::OwnedRecord>* out, ParseReport* rep) {
  ParseReport local;
  ParseReport& r = rep ? *rep : local;
  r = ParseReport{};
  out->clear();
  Json j; std::string e;
  if (!json_parse(text, &j, &e)) return Status::Fail(kErrBadJson, "psn", "response is not valid JSON: " + scrub(e));
  if (!j.is(Json::T::Obj)) return Status::Fail(kErrSchema, "psn", "response top level is not an object");
  const Json* data = j.get("data");
  const Json* errs = j.get("errors");
  const Json* ptr = data && data->is(Json::T::Obj) ? data->get("purchasedTitlesRetrieve") : nullptr;
  const Json* games = ptr && ptr->is(Json::T::Obj) ? ptr->get("games") : nullptr;
  if (!games || !games->is(Json::T::Arr)) {
    if (errs && errs->is(Json::T::Arr) && !errs->a.empty()) {
      std::string m = "Sony reported errors and no data";
      const Json* first = &errs->a[0];
      const Json* msg = first->is(Json::T::Obj) ? first->get("message") : nullptr;
      if (msg && msg->is(Json::T::Str)) m += ": " + scrub(msg->s.substr(0, 200));
      return Status::Fail(kErrSonyErrors, "psn", m);
    }
    return Status::Fail(kErrSchema, "psn", "missing data.purchasedTitlesRetrieve.games array");
  }
  if (games->a.size() > kMaxGames) return Status::Fail(kErrSchema, "psn", "too many games in one response");
  r.games_in_response = static_cast<int>(games->a.size());
  std::vector<library::OwnedRecord> rows;
  std::set<std::string> seen;
  for (const Json& g : games->a) {
    if (!g.is(Json::T::Obj)) { ++r.skipped_invalid; continue; }
    library::OwnedRecord o;
    bool bad = false;
    std::string eid;
    if (!clean_str(g.get("entitlementId"), &eid, &bad) || eid.empty()) bad = true;
    clean_str(g.get("productId"), &o.product_id, &bad);
    clean_str(g.get("titleId"), &o.title_id, &bad);
    clean_str(g.get("conceptId"), &o.concept_id, &bad);
    clean_str(g.get("name"), &o.name, &bad);
    clean_str(g.get("platform"), &o.platform, &bad);
    clean_str(g.get("membership"), &o.membership, &bad);
    clean_bool(g.get("isActive"), &o.is_active, &bad);
    clean_bool(g.get("isDownloadable"), &o.is_downloadable, &bad);
    clean_bool(g.get("isPreOrder"), &o.is_preorder, &bad);
    const Json* img = g.get("image");
    if (img && img->is(Json::T::Obj)) {
      std::string u; bool ibad = false;
      if (clean_str(img->get("url"), &u, &ibad) && !ibad) {
        if (u.rfind("https://", 0) == 0) o.image_url = u; else ++r.dropped_image_urls;
      }
    }
    if (bad) { ++r.skipped_invalid; continue; }
    std::transform(o.platform.begin(), o.platform.end(), o.platform.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    o.entitlement_id = eid;
    if (!seen.insert(eid).second) {
      ++r.duplicates;
      for (auto& x : rows) if (x.entitlement_id == eid) { x = o; break; }
      continue;
    }
    rows.push_back(std::move(o));
  }
  if (r.games_in_response > 0 && rows.empty())
    return Status::Fail(kErrSchema, "psn", "no game in the response had the expected fields (entitlementId); the API may have changed");
  if (r.skipped_invalid > 0) note(&r, std::to_string(r.skipped_invalid) + " game entries skipped (missing entitlementId, wrong field types, control characters or oversize fields)");
  if (r.duplicates > 0) note(&r, std::to_string(r.duplicates) + " duplicate entitlement ids in one response; the last one was kept");
  if (r.dropped_image_urls > 0) note(&r, std::to_string(r.dropped_image_urls) + " non-https image URLs dropped");
  r.accepted = static_cast<int>(rows.size());
  *out = std::move(rows);
  return Status::Ok();
}

std::string owned_state_label(const library::OwnedRecord& r, const std::string& linked_state) {
  (void)r;
  if (linked_state.empty()) return "Owned on PSN (no PC build linked)";
  return linked_state;
}

}  // namespace pnr::psn
