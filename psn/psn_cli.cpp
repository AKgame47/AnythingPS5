#include "psn/psn_cli.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>

#include "psn/psn.hpp"

namespace pnr::psn {
namespace {
std::string esc(const std::string& s) {
  std::string o;
  for (unsigned char c : s) {
    if (c == '"') o += "\\\""; else if (c == '\\') o += "\\\\";
    else if (c == '\n') o += "\\n"; else if (c == '\r') o += "\\r"; else if (c == '\t') o += "\\t";
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += static_cast<char>(c);
  }
  return o;
}
std::string q(const std::string& s) { return "\"" + esc(s) + "\""; }
const char* yn(bool b) { return b ? "yes" : "no"; }
std::string when(double t) {
  if (t <= 0) return "never";
  std::time_t tt = static_cast<std::time_t>(t);
  char b[40];
  std::tm tmv{};
#ifdef _WIN32
  gmtime_s(&tmv, &tt);
#else
  gmtime_r(&tt, &tmv);
#endif
  std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S UTC", &tmv);
  return b;
}
std::string err_text(const Status& s) {
  return std::string(error_kind(s.error.code)) + ": " + scrub(s.error.message) + "\n  next step: " + error_advice(s.error.code) + "\n";
}
std::string err_json(const Status& s) {
  return "{\"ok\":false,\"error\":" + q(error_kind(s.error.code)) + ",\"message\":" + q(scrub(s.error.message)) +
         ",\"advice\":" + q(error_advice(s.error.code)) + "}\n";
}
std::string fail_out(const Status& s, bool json) { return json ? err_json(s) : err_text(s); }
const char* kBanner = "owned on PSN = purchase access only: not installed, not runnable, not a download\n";
}  // namespace

std::string token_path_for(const std::string& db_path) {
  if (const char* e = std::getenv("PNR_PSN_TOKEN_FILE")) if (*e) return e;
  std::filesystem::path p(db_path);
  auto parent = p.parent_path();
  return (parent / "psn" / "npsso.token").string();
}

int cmd_status(library::GameDb& db, const std::string& tp, bool json, std::string* out) {
  TokenInfo ti = token_info(tp);
  auto rows = db.list_owned();
  int linked = 0, missing = 0;
  double last = 0;
  for (auto& r : rows) { if (!r.linked_game_id.empty()) ++linked; if (!r.in_last_sync) ++missing; last = std::max(last, r.last_synced); }
  const char* tstate = !ti.exists ? "absent" : !ti.regular ? "not-a-regular-file" : !ti.format_ok ? "present-bad-format" : (ti.perms_checked && !ti.perms_ok) ? "present-loose-permissions" : "present";
  if (json) {
    *out = std::string("{\"network_code\":false,\"token_file\":") + q(tp) + ",\"token\":" + q(tstate) +
           ",\"permissions_checked\":" + (ti.perms_checked ? "true" : "false") + ",\"owned\":" + std::to_string(rows.size()) +
           ",\"linked\":" + std::to_string(linked) + ",\"not_in_last_complete_sync\":" + std::to_string(missing) +
           ",\"last_sync\":" + q(when(last)) + "}\n";
    return 0;
  }
  *out = "PSN CONNECTOR STATUS (offline core: contains no network code, nothing here contacts Sony)\n";
  *out += "token file: " + tp + "\n";
  *out += std::string("token: ") + tstate + "\n";
  if (ti.exists && ti.regular && !ti.perms_checked) *out += "permissions: not checked on Windows (owner-only hardening is Planned)\n";
  if (ti.exists && ti.perms_checked && !ti.perms_ok) *out += "WARNING: token file is not owner-only; it will not be used until fixed\n";
  *out += "owned entries: " + std::to_string(rows.size()) + " (" + std::to_string(linked) + " linked to a PNR game, " + std::to_string(missing) + " not in the last complete sync)\n";
  *out += "last sync: " + when(last) + "\n";
  *out += "the token value is never printed\n";
  return 0;
}

int cmd_set_token(const std::string& tp, const std::string& input, bool json, std::string* out) {
  Status s = token_save(tp, input);
  if (!s) { *out = fail_out(s, json); return 1; }
  *out = json ? "{\"ok\":true,\"stored\":true}\n"
              : "TOKEN STORED locally at " + tp + " (owner-only). It is never printed, logged or committed.\n";
  return 0;
}

int cmd_import_json(library::GameDb& db, const std::string& file, bool complete, bool json, std::string* out) {
  std::error_code ec;
  const auto sz = std::filesystem::file_size(file, ec);
  if (ec || !std::filesystem::is_regular_file(file, ec)) { *out = fail_out(Status::Fail(kErrBadInput, "psn", "cannot read that file"), json); return 1; }
  if (sz > 16u * 1024 * 1024) { *out = fail_out(Status::Fail(kErrBadInput, "psn", "file larger than 16 MiB"), json); return 1; }
  std::ifstream f(file, std::ios::binary);
  std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::vector<library::OwnedRecord> rows;
  ParseReport rep;
  Status s = parse_purchased_games(text, &rows, &rep);
  if (!s) { *out = fail_out(s, json) ; if (!json) *out += "nothing was imported; the library is unchanged\n"; return 1; }
  library::OwnedSyncCounts c;
  s = db.sync_owned(rows, complete, static_cast<double>(std::time(nullptr)), &c);
  if (!s) { *out = fail_out(Status::Fail(kErrLibrary, "psn", "library write failed: " + scrub(s.error.message)), json); return 1; }
  if (json) {
    *out = "{\"ok\":true,\"complete\":" + std::string(complete ? "true" : "false") + ",\"accepted\":" + std::to_string(rep.accepted) +
           ",\"skipped_invalid\":" + std::to_string(rep.skipped_invalid) + ",\"added\":" + std::to_string(c.added) +
           ",\"updated\":" + std::to_string(c.updated) + ",\"unchanged\":" + std::to_string(c.unchanged) +
           ",\"restored\":" + std::to_string(c.restored) + ",\"marked_missing\":" + std::to_string(c.marked_missing) + "}\n";
    return 0;
  }
  *out = "PSN IMPORT OK (from a saved response file; nothing contacted Sony)\n";
  *out += "  entries accepted: " + std::to_string(rep.accepted) + " of " + std::to_string(rep.games_in_response) + "\n";
  *out += "  added " + std::to_string(c.added) + ", updated " + std::to_string(c.updated) + ", unchanged " + std::to_string(c.unchanged) +
          ", restored " + std::to_string(c.restored) + ", marked not-returned " + std::to_string(c.marked_missing) + "\n";
  for (auto& n : rep.notes) *out += "  note: " + n + "\n";
  *out += complete ? "  mode: complete (entries absent from this file are marked, never deleted)\n"
                   : "  mode: merge (nothing is marked missing; use --complete when the file is the whole library)\n";
  *out += kBanner;
  return 0;
}

int cmd_owned(library::GameDb& db, bool json, const std::string& plat, std::string* out) {
  auto rows = db.list_owned();
  std::string want = plat;
  std::transform(want.begin(), want.end(), want.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  std::vector<library::OwnedRecord> v;
  for (auto& r : rows) if (want.empty() || r.platform == want) v.push_back(r);
  auto state_of = [&](const library::OwnedRecord& r) {
    std::string ls;
    if (!r.linked_game_id.empty()) { auto c = db.get_compatibility(r.linked_game_id); ls = c ? c->state : "Untested"; }
    return owned_state_label(r, ls);
  };
  if (json) {
    std::string s = "{\"owned_on_psn\":[";
    bool first = true;
    for (auto& r : v) {
      if (!first) s += ",";
      first = false;
      s += "{\"entitlement_id\":" + q(r.entitlement_id) + ",\"name\":" + q(r.name) + ",\"platform\":" + q(r.platform) +
           ",\"product_id\":" + q(r.product_id) + ",\"title_id\":" + q(r.title_id) + ",\"concept_id\":" + q(r.concept_id) +
           ",\"membership\":" + q(r.membership) + ",\"image_url\":" + q(r.image_url) +
           ",\"sony_active\":" + (r.is_active ? "true" : "false") + ",\"sony_downloadable_flag\":" + (r.is_downloadable ? "true" : "false") +
           ",\"sony_preorder\":" + (r.is_preorder ? "true" : "false") + ",\"in_last_sync\":" + (r.in_last_sync ? "true" : "false") +
           ",\"linked_game_id\":" + q(r.linked_game_id) + ",\"state\":" + q(state_of(r)) +
           ",\"installed\":false,\"runnable\":false}";
    }
    *out = s + "]}\n";
    return 0;
  }
  std::string t = "OWNED ON PSN: purchase access only - not installed, not runnable, not a download\n";
  if (v.empty()) t += "  (none)\n";
  int n = 0;
  for (auto& r : v) {
    t += std::to_string(++n) + ". " + (r.name.empty() ? "(no name)" : r.name) + "  [" + (r.platform.empty() ? "?" : r.platform) + "]\n";
    t += "   entitlement " + r.entitlement_id + ", title " + r.title_id + ", product " + r.product_id + "\n";
    t += std::string("   Sony flags: active ") + yn(r.is_active) + ", downloadable " + yn(r.is_downloadable) + ", pre-order " + yn(r.is_preorder) +
         (r.membership.empty() ? "" : ", membership " + r.membership) + "\n";
    t += "   state: " + state_of(r);
    if (!r.linked_game_id.empty()) t += "  (linked to PNR game " + r.linked_game_id + ", linked by a person)";
    t += "\n";
    if (!r.in_last_sync) t += "   not returned by the last complete sync (kept, not deleted)\n";
  }
  t += "'downloadable' is Sony's flag for the entitlement. PNR has no download feature and cannot install PS4/PS5 content.\n";
  *out = t;
  return 0;
}

int cmd_link(library::GameDb& db, const std::string& eid, const std::string& gid, bool json, std::string* out) {
  Status s = db.link_owned(eid, gid);
  if (!s) {
    *out = json ? "{\"ok\":false,\"error\":" + q(s.error.message) + "}\n" : "LINK REFUSED: " + s.error.message + "\n";
    return s.error.code == 120 || s.error.code == 121 ? 2 : 1;
  }
  *out = json ? "{\"ok\":true}\n" : "LINKED: " + eid + " -> PNR game " + gid + " (set by a person; this does not make the game runnable or change its compatibility state)\n";
  return 0;
}

int cmd_unlink(library::GameDb& db, const std::string& eid, bool json, std::string* out) {
  Status s = db.unlink_owned(eid);
  if (!s) { *out = json ? "{\"ok\":false,\"error\":" + q(s.error.message) + "}\n" : "UNLINK REFUSED: " + s.error.message + "\n"; return 2; }
  *out = json ? "{\"ok\":true}\n" : "UNLINKED: " + eid + "\n";
  return 0;
}

int cmd_disconnect(library::GameDb& db, const std::string& tp, bool forget, bool yes, bool json, std::string* out) {
  const size_t owned = db.list_owned().size();
  if (forget && !yes) {
    *out = json ? "{\"ok\":false,\"needs_confirmation\":true,\"would_delete_owned\":" + std::to_string(owned) + "}\n"
                : "WOULD DELETE the token file and " + std::to_string(owned) + " owned-on-PSN entries (add --yes). Installed games are never touched.\n";
    return 3;
  }
  Status s = token_remove(tp);
  if (!s) { *out = fail_out(s, json); return 1; }
  if (forget) {
    s = db.forget_owned();
    if (!s) { *out = fail_out(Status::Fail(kErrLibrary, "psn", "library write failed"), json); return 1; }
  }
  *out = json ? std::string("{\"ok\":true,\"token_removed\":true,\"owned_deleted\":") + (forget ? "true" : "false") + "}\n"
              : std::string("DISCONNECTED: local token file removed (zeroed, then deleted).") +
                (forget ? " Owned-on-PSN entries deleted.\n" : " Owned-on-PSN entries were kept (use --forget-owned --yes to delete them).\n") +
                "PNR cannot revoke the token at Sony. If you think it leaked, change your PSN password and check your account security settings.\n";
  return 0;
}

}  // namespace pnr::psn
