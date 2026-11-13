#include "psn/sync.hpp"

#include <algorithm>
#include <cstring>
#include <set>

#include "psn/json.hpp"

namespace pnr::psn {
namespace {
// Public client id/secret of Sony's own PlayStation mobile app, as published
// in the MIT psn-api source. Not a user credential.
const char* kBasic = "Basic MDk1MTUxNTktNzIzNy00MzcwLTliNDAtMzgwNmU2N2MwODkxOnVjUGprYTV0bnRCMktxc1A=";
const char* kAuthBase = "https://ca.account.sony.com/api/authz/v3/oauth";
const char* kGraphql = "https://web.np.playstation.com/api/graphql/v1/op";
const char* kPurchasedHash = "827a423f6a8ddca4107ac01395af2ec0eafd8396fc7fa204aaf9b7ed2eefa168";

std::string pct(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string o;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += static_cast<char>(c);
    else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
  }
  return o;
}
void wipe(std::string& s) { volatile char* p = s.empty() ? nullptr : &s[0]; for (size_t i = 0; i < s.size(); ++i) p[i] = 0; s.clear(); }

struct Tokens {
  std::string access, refresh;
  ~Tokens() { wipe(access); wipe(refresh); }
};

Status parse_tokens(const HttpResponse& r, Tokens* t) {
  Status h = classify_http(r.status, r.body);
  if (!h) return h;
  Json j; std::string e;
  if (!json_parse(r.body, &j, &e) || !j.is(Json::T::Obj)) return Status::Fail(kErrBadJson, "psn", "token response is not valid JSON");
  const Json* a = j.get("access_token");
  if (!a || !a->is(Json::T::Str) || a->s.empty() || a->s.size() > 8192) return Status::Fail(kErrSchema, "psn", "token response has no access_token");
  t->access = a->s;
  const Json* rf = j.get("refresh_token");
  t->refresh = (rf && rf->is(Json::T::Str) && rf->s.size() <= 8192) ? rf->s : "";
  return Status::Ok();
}

Status exchange_npsso(HttpClient& http, const std::string& npsso, Tokens* t, SyncReport* rep) {
  HttpRequest a;
  a.url = std::string(kAuthBase) + "/authorize?access_type=offline&client_id=09515159-7237-4370-9b40-3806e67c0891&redirect_uri=" +
          pct("com.scee.psxandroid.scecompcall://redirect") + "&response_type=code&scope=" + pct("psn:mobile.v2.core psn:clientapp");
  a.headers.push_back("Cookie: npsso=" + npsso);
  HttpResponse r;
  ++rep->requests;
  Status s = http.perform(a, &r);
  wipe(a.headers[0]);
  if (!s) return s;
  std::string code;
  if (r.status == 302 && parse_code_from_location(r.location, &code)) {
    HttpRequest p;
    p.method = "POST";
    p.url = std::string(kAuthBase) + "/token";
    p.headers = {std::string("Authorization: ") + kBasic, "Content-Type: application/x-www-form-urlencoded"};
    p.body = "code=" + pct(code) + "&redirect_uri=" + pct("com.scee.psxandroid.scecompcall://redirect") + "&grant_type=authorization_code&token_format=jwt";
    wipe(code);
    HttpResponse tr;
    ++rep->requests;
    s = http.perform(p, &tr);
    wipe(p.body);
    if (!s) return s;
    return parse_tokens(tr, t);
  }
  // No code: Sony answered but did not authorize. A 429/5xx is its own kind; everything else means the NPSSO was not accepted.
  if (r.status == 429 || (r.status >= 500 && r.status < 600)) return classify_http(r.status, r.body);
  return Status::Fail(kErrTokenRejected, "psn", "Sony did not accept the token (no authorization code returned, HTTP " + std::to_string(r.status) + ")");
}

Status refresh(HttpClient& http, Tokens* t, SyncReport* rep) {
  if (t->refresh.empty()) return Status::Fail(kErrTokenRejected, "psn", "access token rejected and no refresh token available");
  HttpRequest p;
  p.method = "POST";
  p.url = std::string(kAuthBase) + "/token";
  p.headers = {std::string("Authorization: ") + kBasic, "Content-Type: application/x-www-form-urlencoded"};
  p.body = "refresh_token=" + pct(t->refresh) + "&grant_type=refresh_token&token_format=jwt&scope=" + pct("psn:mobile.v2.core psn:clientapp");
  HttpResponse r;
  ++rep->requests; ++rep->refreshes;
  Status s = http.perform(p, &r);
  wipe(p.body);
  if (!s) return s;
  Tokens nt;
  s = parse_tokens(r, &nt);
  if (!s) return s;
  wipe(t->access); wipe(t->refresh);
  t->access = nt.access; t->refresh = nt.refresh;
  return Status::Ok();
}
}  // namespace

std::vector<std::string> official_hosts() { return {kAuthHost, kGraphqlHost}; }

bool parse_code_from_location(const std::string& loc, std::string* code) {
  if (loc.size() > 4096) return false;
  const size_t q = loc.find('?');
  if (q == std::string::npos) return false;
  size_t i = q + 1;
  while (i < loc.size()) {
    size_t e = loc.find('&', i);
    std::string kv = loc.substr(i, e == std::string::npos ? std::string::npos : e - i);
    if (kv.compare(0, 5, "code=") == 0) {
      std::string v = kv.substr(5);
      if (v.empty() || v.size() > 512) return false;
      for (unsigned char c : v) if (!(std::isalnum(c) || c == '.' || c == '-' || c == '_')) return false;
      *code = v;
      return true;
    }
    if (e == std::string::npos) break;
    i = e + 1;
  }
  return false;
}

std::string build_purchased_url(int start, int size) {
  std::string vars = "{\"isActive\":true,\"platform\":[\"ps4\",\"ps5\"],\"size\":" + std::to_string(size) + ",\"start\":" + std::to_string(start) +
                     ",\"sortBy\":\"ACTIVE_DATE\",\"sortDirection\":\"desc\"}";
  std::string ext = std::string("{\"persistedQuery\":{\"version\":1,\"sha256Hash\":\"") + kPurchasedHash + "\"}}";
  return std::string(kGraphql) + "?operationName=getPurchasedGameList&variables=" + pct(vars) + "&extensions=" + pct(ext);
}

Status fetch_purchased_games(HttpClient& raw, const std::string& npsso, const SyncOptions& opt,
                             std::vector<library::OwnedRecord>* rows, SyncReport* rep) {
  SyncReport local; SyncReport& R = rep ? *rep : local;
  R = SyncReport{};
  rows->clear();
  GuardedHttp http(raw, official_hosts());
  const int size = std::max(1, std::min(100, opt.page_size));
  Tokens tok;
  Status s = exchange_npsso(http, npsso, &tok, &R);
  if (!s) return s;
  std::vector<library::OwnedRecord> all;
  std::set<std::string> seen;
  bool refreshed = false;
  for (int page = 0; page < opt.max_pages; ++page) {
    HttpRequest g;
    g.url = build_purchased_url(page * size, size);
    HttpResponse r;
    for (int attempt = 0; attempt < 2; ++attempt) {
      g.headers = {"Authorization: Bearer " + tok.access, "Content-Type: application/json"};
      ++R.requests;
      s = http.perform(g, &r);
      wipe(g.headers[0]);
      if (!s) return s;
      if ((r.status == 401 || r.status == 403) && !refreshed) {
        s = refresh(http, &tok, &R);
        if (!s) return s;
        refreshed = true;
        continue;
      }
      break;
    }
    s = classify_http(r.status, r.body);
    if (!s) return s;
    std::vector<library::OwnedRecord> got;
    ParseReport pr;
    s = parse_purchased_games(r.body, &got, &pr);
    if (!s) return s;
    ++R.pages;
    R.parse.games_in_response += pr.games_in_response; R.parse.accepted += pr.accepted;
    R.parse.skipped_invalid += pr.skipped_invalid; R.parse.duplicates += pr.duplicates;
    R.parse.dropped_image_urls += pr.dropped_image_urls;
    for (auto& n : pr.notes) if (R.parse.notes.size() < 20) R.parse.notes.push_back(n);
    for (auto& x : got) if (seen.insert(x.entitlement_id).second) all.push_back(std::move(x));
    if (pr.games_in_response < size) { R.complete = true; break; }
  }
  *rows = std::move(all);
  return Status::Ok();
}

}  // namespace pnr::psn
