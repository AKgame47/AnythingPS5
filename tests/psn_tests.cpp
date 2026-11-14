// PSN connector slice 1 tests (offline). All Sony-shaped input here is
// SYNTHETIC: hand-written to match the published psn-api type, never
// captured from Sony. These tests say nothing about live Sony behaviour.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "library/game_db.hpp"
#include "psn/json.hpp"
#include "psn/psn.hpp"
#include "psn/psn_cli.hpp"

namespace {
namespace fs = std::filesystem;
using namespace pnr::psn;
const std::string kTok = "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZab";
void spit(const std::string& p, const std::string& s) {
  fs::create_directories(fs::path(p).parent_path());
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f << s;
}
std::string slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
bool has(const std::string& h, const std::string& n) { return h.find(n) != std::string::npos; }
unsigned mode_of(const std::string& p) { struct stat st; return ::stat(p.c_str(), &st) == 0 ? (st.st_mode & 0777) : 0xFFFF; }

std::string game(const std::string& eid, const std::string& name, const std::string& plat = "PS5", bool dl = true) {
  return "{\"__typename\":\"GameLibraryTitle\",\"conceptId\":\"1000\",\"entitlementId\":\"" + eid + "\",\"image\":{\"__typename\":\"Media\",\"url\":\"https://example.invalid/img/" + eid +
         ".png\"},\"isActive\":true,\"isDownloadable\":" + (dl ? "true" : "false") + ",\"isPreOrder\":false,\"membership\":\"NONE\",\"name\":\"" + name +
         "\",\"platform\":\"" + plat + "\",\"productId\":\"PROD-" + eid + "\",\"titleId\":\"TITLE-" + eid + "\"}";
}
std::string resp(const std::string& games) { return "{\"data\":{\"purchasedTitlesRetrieve\":{\"__typename\":\"GameList\",\"games\":[" + games + "]}}}"; }
}  // namespace

void pnr_psn_sync_tests(int& pass, int& fail);  // psn_sync_tests.cpp, run after these
void pnr_download_tests(int& pass, int& fail);
void pnr_signature_tests(int& pass, int& fail);
void pnr_sandbox_tests(int& pass, int& fail);
void pnr_scan_tests(int& pass, int& fail);
void pnr_research_tests(int& pass, int& fail);
void pnr_ask_tests(int& pass, int& fail);
void pnr_suggest_tests(int& pass, int& fail);
void pnr_ui_model_tests(int& pass, int& fail);
void pnr_module_tests(int& pass, int& fail);
void pnr_kernel_tests(int& pass, int& fail);
void pnr_libc_tests(int& pass, int& fail);
void pnr_pm4_tests(int& pass, int& fail);
void pnr_gfx_state_tests(int& pass, int& fail);
void pnr_gpu_va_tests(int& pass, int& fail);
void pnr_gfx_draw_tests(int& pass, int& fail);
void pnr_hw_tests(int& pass, int& fail);
void pnr_plugin_tests(int& pass, int& fail);
void pnr_store_tests(int& pass, int& fail);     // store_tests.cpp, also run from here (test_main.cpp is too large for the browser editor)
void pnr_psn_tests(int& pass, int& fail) {
  struct RunSync { int& p; int& f; ~RunSync() { pnr_psn_sync_tests(p, f); pnr_store_tests(p, f); pnr_download_tests(p, f); pnr_signature_tests(p, f); pnr_sandbox_tests(p, f); pnr_scan_tests(p, f); pnr_research_tests(p, f); pnr_ask_tests(p, f); pnr_suggest_tests(p, f); pnr_ui_model_tests(p, f); pnr_module_tests(p, f); pnr_kernel_tests(p, f); pnr_libc_tests(p, f); pnr_pm4_tests(p, f); pnr_gfx_state_tests(p, f); pnr_gpu_va_tests(p, f); pnr_gfx_draw_tests(p, f); pnr_hw_tests(p, f); pnr_plugin_tests(p, f); } } run_sync{pass, fail};
  auto check = [&](bool ok, const std::string& label) { if (ok) ++pass; else ++fail; std::printf("%s %s\n", ok ? "PASS" : "FAIL", label.c_str()); };
  const std::string dir = "/tmp/pnr_psn_" + std::to_string(::getpid());
  fs::remove_all(dir); fs::create_directories(dir);

  // ---- JSON reader
  {
    Json j; std::string e;
    check(json_parse("{\"a\":[1,2.5e1,-3,true,false,null,\"x\\u00e9\\n\"],\"b\":{}}", &j, &e) && j.get("a") && j.get("a")->a.size() == 7, "json: valid document parses");
    check(j.get("a")->a[6].s == "x\xC3\xA9\n", "json: \\u escape and \\n decode to UTF-8");
    check(json_parse("\"\\ud83d\\ude00\"", &j, &e) && j.s == "\xF0\x9F\x98\x80", "json: surrogate pair decodes");
    for (const char* bad : {"", "{", "[1,]", "{\"a\":1,}", "{'a':1}", "[1] x", "\"\\ud83d\"", "\"\\udc00\"", "01", "1.", "-", "nul", "{\"a\" 1}", "[\"a\nb\"]", "\"\\q\"", "1e999", "[1 2]"})
      check(!json_parse(bad, &j, &e) && !e.empty(), std::string("json: rejects [") + (bad[0] == '[' && bad[1] == '"' ? "string-with-newline" : bad) + "]");
    std::string deep(40, '['), deep2(40, ']');
    check(!json_parse(deep + deep2, &j, &e), "json: nesting deeper than 32 rejected");
    std::string ok(30, '['); ok += std::string(30, ']');
    check(json_parse(ok, &j, &e), "json: nesting of 30 accepted");
    check(!json_parse(std::string(kJsonMaxInput + 1, ' '), &j, &e), "json: input over 16 MiB rejected");
    check(!json_parse("{\"secret\":" + kTok + "}", &j, &e) && !has(e, kTok), "json: error text never echoes input");
  }

  // ---- token extraction and validation
  {
    std::string t;
    check(token_extract(kTok, &t) && t == kTok, "token: bare 64-char value accepted");
    check(token_extract("  " + kTok + "\n", &t) && t == kTok, "token: surrounding whitespace trimmed");
    check(token_extract("{\"npsso\":\"" + kTok + "\"}", &t) && t == kTok, "token: pasted {\"npsso\":...} JSON accepted");
    check(!token_extract(kTok.substr(1), &t), "token: 63 chars rejected");
    check(!token_extract(kTok + "a", &t), "token: 65 chars rejected");
    check(!token_extract(kTok.substr(0, 63) + "-", &t), "token: non-alphanumeric rejected");
    check(!token_extract("{\"other\":\"" + kTok + "\"}", &t), "token: JSON without npsso rejected");
    check(!token_extract("{\"npsso\":12}", &t), "token: npsso of wrong type rejected");
    check(!token_extract(std::string(5000, 'a'), &t), "token: oversize paste rejected");
    check(!token_extract("", &t), "token: empty rejected");
  }

  // ---- token store on disk
  {
    const std::string tp = dir + "/cfg/psn/npsso.token";
    check(token_info(tp).exists == false, "store: absent token reported absent");
    std::string t;
    check(token_load(tp, &t).error.code == kErrNoToken, "store: load without a token is no-token");
    check(token_save(tp, kTok).ok, "store: save succeeds");
    check(mode_of(tp) == 0600, "store: token file mode is exactly 0600");
    check(mode_of(dir + "/cfg/psn") == 0700, "store: token directory mode is 0700");
    check(!fs::exists(tp + ".tmp"), "store: no temp file left behind");
    TokenInfo ti = token_info(tp);
    check(ti.exists && ti.regular && ti.perms_ok && ti.format_ok, "store: info says present, regular, owner-only, valid format");
    check(token_load(tp, &t).ok && t == kTok, "store: load round-trips");
    check(token_save(tp, "{\"npsso\":\"" + std::string(64, 'Z') + "\"}").ok && token_load(tp, &t).ok && t == std::string(64, 'Z'), "store: save replaces the old token");
    check(mode_of(tp) == 0600, "store: replaced file is still 0600");
    auto bad = token_save(tp, kTok.substr(5));
    check(!bad.ok && bad.error.code == kErrTokenFormat && !has(bad.error.message, kTok.substr(5)), "store: a bad paste is refused without echoing it");
    check(token_load(tp, &t).ok && t == std::string(64, 'Z'), "store: a refused paste leaves the old token alone");
    ::chmod(tp.c_str(), 0644);
    check(token_load(tp, &t).error.code == kErrTokenPerms && !token_info(tp).perms_ok, "store: group/other-readable token is refused");
    ::chmod(tp.c_str(), 0640);
    check(token_load(tp, &t).error.code == kErrTokenPerms, "store: group-readable token is refused");
    ::chmod(tp.c_str(), 0600);
    spit(tp, "not a token\n"); ::chmod(tp.c_str(), 0600);
    check(token_load(tp, &t).error.code == kErrTokenFormat && !token_info(tp).format_ok, "store: corrupt contents are token-format");
    fs::remove(tp);
    spit(dir + "/real.token", kTok + "\n"); ::chmod((dir + "/real.token").c_str(), 0600);
    fs::create_symlink(dir + "/real.token", tp);
    check(token_load(tp, &t).error.code == kErrTokenPerms && !token_info(tp).regular, "store: a symlinked token path is refused");
    check(token_save(tp, std::string(64, 'Q')).ok && slurp(dir + "/real.token") == kTok + "\n" && token_info(tp).regular && mode_of(tp) == 0600, "store: save replaces a symlink with a real file and never writes through it");
    fs::remove(tp);
    fs::create_directories(tp);
    check(token_load(tp, &t).error.code == kErrTokenPerms, "store: a directory at the token path is refused");
    fs::remove_all(tp);
    check(token_save(tp, kTok).ok, "store: save again");
    check(token_remove(tp).ok && !fs::exists(tp), "store: remove deletes the file");
    check(token_remove(tp).ok, "store: removing an absent token is ok");
    fs::remove(dir + "/real.token");
  }

  // ---- redaction
  {
    check(redact("x " + kTok + " y " + kTok, kTok) == "x [redacted] y [redacted]", "redact: every occurrence replaced");
    check(redact("abc", "") == "abc", "redact: empty token leaves text alone");
    check(scrub("id " + kTok + " end") == "id [redacted] end", "scrub: 64-char run replaced");
    check(scrub("short words and 31chars_aaaaaaaaaaaaaaaaaaaaaaa") == "short words and 31chars_aaaaaaaaaaaaaaaaaaaaaaa", "scrub: short runs untouched");
    check(has(scrub(std::string(32, 'q')), "[redacted]"), "scrub: 32-char run replaced");
  }

  // ---- error taxonomy
  {
    check(classify_http(200, "").ok && classify_http(204, "").ok, "http: 2xx ok");
    check(classify_http(401, "").error.code == kErrTokenRejected && classify_http(403, "").error.code == kErrTokenRejected, "http: 401/403 is token-rejected");
    check(classify_http(429, "").error.code == kErrRateLimited, "http: 429 is rate-limited");
    check(classify_http(500, "").error.code == kErrServer && classify_http(503, "").error.code == kErrServer, "http: 5xx is server-error");
    check(classify_http(0, "").error.code == kErrNetwork, "http: no response is network");
    check(classify_http(404, "").error.code == kErrSchema && classify_http(302, "").error.code == kErrSchema, "http: other statuses are schema-change");
    for (int c : {kErrNoToken, kErrTokenFormat, kErrTokenPerms, kErrTokenIo, kErrTokenRejected, kErrRateLimited, kErrServer, kErrNetwork, kErrBadJson, kErrSchema, kErrSonyErrors, kErrBadInput, kErrLibrary})
      check(std::string(error_kind(c)) != "unknown" && std::string(error_advice(c)).size() > 10, "taxonomy: code " + std::to_string(c) + " has a name and advice");
  }

  // ---- parser (synthetic fixtures)
  {
    std::vector<pnr::library::OwnedRecord> rows; ParseReport rep;
    auto s = parse_purchased_games(resp(game("E1", "Alpha") + "," + game("E2", "Beta", "ps4", false)), &rows, &rep);
    check(s.ok && rows.size() == 2 && rep.accepted == 2 && rep.skipped_invalid == 0, "parse: two valid games accepted");
    check(rows[0].entitlement_id == "E1" && rows[0].title_id == "TITLE-E1" && rows[0].product_id == "PROD-E1" && rows[0].concept_id == "1000" && rows[0].name == "Alpha" && rows[0].platform == "PS5" && rows[0].membership == "NONE" && rows[0].is_active && rows[0].is_downloadable && !rows[0].is_preorder && rows[0].image_url == "https://example.invalid/img/E1.png", "parse: every field maps");
    check(rows[1].platform == "PS4" && !rows[1].is_downloadable, "parse: platform upper-cased, downloadable flag kept as Sony says");
    check(parse_purchased_games(resp(""), &rows, &rep).ok && rows.empty() && rep.games_in_response == 0, "parse: empty library is ok");
    check(parse_purchased_games(resp("{\"entitlementId\":\"E9\"}"), &rows, &rep).ok && rows.size() == 1 && rows[0].name.empty() && rows[0].image_url.empty(), "parse: missing optional fields tolerated");
    check(parse_purchased_games(resp("{\"entitlementId\":\"E9\",\"newSonyField\":{\"x\":1},\"conceptId\":null}"), &rows, &rep).ok && rows.size() == 1, "parse: unknown fields and null concept ignored");
    check(parse_purchased_games(resp(game("E1", "A") + ",{\"name\":\"no id\"}," + game("E3", "C")), &rows, &rep).ok && rows.size() == 2 && rep.skipped_invalid == 1 && !rep.notes.empty(), "parse: entry without entitlementId skipped and reported");
    check(parse_purchased_games(resp(game("E1", "A") + ",42,\"str\",null"), &rows, &rep).ok && rows.size() == 1 && rep.skipped_invalid == 3, "parse: non-object entries skipped");
    check(parse_purchased_games(resp("{\"entitlementId\":\"E1\",\"isActive\":\"yes\"}," + game("E2", "B")), &rows, &rep).ok && rows.size() == 1 && rep.skipped_invalid == 1, "parse: wrong field type skips the entry");
    check(parse_purchased_games(resp("{\"entitlementId\":\"E1\",\"name\":\"bad\\u0007name\"}," + game("E2", "B")), &rows, &rep).ok && rows.size() == 1, "parse: control characters in a field skip the entry");
    check(parse_purchased_games(resp("{\"entitlementId\":\"E1\",\"name\":\"" + std::string(600, 'n') + "\"}," + game("E2", "B")), &rows, &rep).ok && rows.size() == 1, "parse: oversize field skips the entry");
    check(parse_purchased_games(resp("{\"entitlementId\":\"E1\",\"image\":{\"url\":\"http://x.invalid/a.png\"}}"), &rows, &rep).ok && rows[0].image_url.empty() && rep.dropped_image_urls == 1, "parse: non-https image URL dropped");
    check(parse_purchased_games(resp("{\"entitlementId\":\"E1\",\"image\":{\"url\":\"javascript:alert(1)\"}}"), &rows, &rep).ok && rows[0].image_url.empty(), "parse: javascript: image URL dropped");
    check(parse_purchased_games(resp(game("E1", "First") + "," + game("E1", "Second")), &rows, &rep).ok && rows.size() == 1 && rows[0].name == "Second" && rep.duplicates == 1, "parse: duplicate ids in one response, last wins");
    // failures: nothing produced
    auto f = parse_purchased_games("not json", &rows, &rep);
    check(!f.ok && f.error.code == kErrBadJson && rows.empty(), "parse: invalid JSON is bad-json");
    check(parse_purchased_games("[]", &rows, &rep).error.code == kErrSchema, "parse: top-level array is schema-change");
    check(parse_purchased_games("{\"data\":{}}", &rows, &rep).error.code == kErrSchema, "parse: missing purchasedTitlesRetrieve is schema-change");
    check(parse_purchased_games("{\"data\":{\"purchasedTitlesRetrieve\":{\"games\":\"x\"}}}", &rows, &rep).error.code == kErrSchema, "parse: games not an array is schema-change");
    check(parse_purchased_games("{\"data\":{\"purchasedTitlesRetrieve\":null}}", &rows, &rep).error.code == kErrSchema, "parse: null purchasedTitlesRetrieve is schema-change");
    check(parse_purchased_games(resp("{\"id\":\"renamed\"},{\"id\":\"renamed2\"}"), &rows, &rep).error.code == kErrSchema && rows.empty(), "parse: every entry lacking entitlementId reads as an API change, not an empty library");
    auto se = parse_purchased_games("{\"errors\":[{\"message\":\"Not authorised " + kTok + "\"}],\"data\":null}", &rows, &rep);
    check(se.error.code == kErrSonyErrors && !has(se.error.message, kTok) && has(se.error.message, "Not authorised"), "parse: Sony errors array is sony-errors and Sony text is scrubbed");
    check(parse_purchased_games("{\"errors\":[],\"data\":{}}", &rows, &rep).error.code == kErrSchema, "parse: empty errors array with no data is schema-change");
    auto bj = parse_purchased_games("{\"x\":" + kTok + "}", &rows, &rep);
    check(!has(bj.error.message, kTok), "parse: bad-json message does not echo the input");
  }

  // ---- database
  pnr::library::GameDb db;
  check(db.open(dir + "/lib/library.db").ok, "db: opens with the owned_entitlements table");
  auto mk = [&](const std::string& json) { std::vector<pnr::library::OwnedRecord> r; ParseReport p; auto s = parse_purchased_games(json, &r, &p); return r; };
  {
    pnr::library::OwnedSyncCounts c;
    check(db.sync_owned(mk(resp(game("E1", "Zeta") + "," + game("E2", "alpha") + "," + game("E3", "Mid"))), true, 1000, &c).ok && c.added == 3 && c.updated == 0 && c.unchanged == 0, "db: first sync adds three");
    auto all = db.list_owned();
    check(all.size() == 3 && all[0].name == "alpha" && all[1].name == "Mid" && all[2].name == "Zeta", "db: owned list sorted by name, case-insensitive");
    check(db.list_games().empty() && !db.get_installation("E1") && !db.get_compatibility("E1"), "db: owned entries are not games, installs or compatibility records");
    check(db.sync_owned(mk(resp(game("E1", "Zeta") + "," + game("E2", "alpha") + "," + game("E3", "Mid"))), true, 2000, &c).ok && c.unchanged == 3 && c.added == 0, "db: identical resync is unchanged (idempotent)");
    check(db.list_owned().size() == 3 && db.get_owned("E1")->first_seen == 1000 && db.get_owned("E1")->last_synced == 2000, "db: first_seen kept, last_synced moves");
    check(db.sync_owned(mk(resp(game("E1", "Zeta Renamed") + "," + game("E2", "alpha"))), true, 3000, &c).ok && c.updated == 1 && c.unchanged == 1 && c.marked_missing == 1, "db: rename updates, absent row marked in a complete sync");
    check(db.get_owned("E3") && !db.get_owned("E3")->in_last_sync && db.get_owned("E3")->name == "Mid", "db: missing row is kept, not deleted");
    check(db.sync_owned(mk(resp(game("E4", "New"))), false, 4000, &c).ok && c.added == 1 && c.marked_missing == 0 && db.get_owned("E1")->in_last_sync, "db: merge sync never marks rows missing");
    check(db.sync_owned(mk(resp(game("E3", "Mid"))), false, 5000, &c).ok && c.restored == 1 && db.get_owned("E3")->in_last_sync, "db: a returning row is restored");
    check(db.sync_owned({}, true, 6000, &c).ok && c.marked_missing == 4 && db.list_owned().size() == 4, "db: an empty complete sync marks all, deletes none");
    check(!db.sync_owned(mk(resp(game("E1", "Zeta Renamed"))), true, 7000, &c).ok == false, "db: sync after that still works");
  }
  {
    pnr::library::GameRecord g; g.game_id = "real.game"; g.title = "Real"; g.version = "1"; 
    db.add_game(g);
    check(db.link_owned("E1", "real.game").ok && db.get_owned("E1")->linked_game_id == "real.game", "link: a person can link an entitlement to a real game");
    check(!db.link_owned("E1", "nope").ok && !db.link_owned("NOPE", "real.game").ok, "link: unknown game or entitlement refused");
    pnr::library::OwnedSyncCounts c;
    db.sync_owned(mk(resp(game("E1", "Zeta Again"))), false, 8000, &c);
    check(db.get_owned("E1")->linked_game_id == "real.game" && db.get_owned("E1")->name == "Zeta Again", "link: a resync keeps the link");
    check(!db.get_compatibility("real.game") || db.get_compatibility("real.game")->state == "Untested", "link: linking does not change compatibility");
    check(db.remove_game("real.game").ok && db.get_owned("E1")->linked_game_id.empty() && db.get_owned("E1"), "link: removing the game unlinks but keeps the owned row");
    db.add_game(g);
    db.link_owned("E2", "real.game");
    check(db.unlink_owned("E2").ok && db.get_owned("E2")->linked_game_id.empty() && !db.unlink_owned("NOPE").ok, "link: unlink works, unknown refused");
  }

  // ---- command layer
  {
    const std::string tp = dir + "/cmd/psn/npsso.token";
    std::string out, all;
    auto grab = [&](int rc, const char* label, int want) { all += out; return rc == want ? true : (std::printf("  rc=%d for %s: %s\n", rc, label, out.c_str()), false); };
    check(grab(cmd_status(db, tp, false, &out), "status", 0) && has(out, "token: absent") && has(out, "only `psn sync` contacts Sony"), "cmd: status without a token says absent and local");
    check(grab(cmd_set_token(tp, "garbage", false, &out), "bad set-token", 1) && has(out, "token-format") && !has(out, "garbage"), "cmd: set-token with garbage is refused, not echoed");
    check(grab(cmd_set_token(tp, kTok + "\n", false, &out), "set-token", 0) && mode_of(tp) == 0600, "cmd: set-token stores an owner-only file");
    check(grab(cmd_status(db, tp, false, &out), "status", 0) && has(out, "token: present") && has(out, "never printed"), "cmd: status says present");
    check(grab(cmd_status(db, tp, true, &out), "status json", 0) && has(out, "\"token\":\"present\"") && has(out, "\"network_code\":false"), "cmd: status --json");
    ::chmod(tp.c_str(), 0644);
    check(grab(cmd_status(db, tp, false, &out), "status loose", 0) && has(out, "present-loose-permissions") && has(out, "WARNING"), "cmd: status warns about loose permissions");
    ::chmod(tp.c_str(), 0600);
    spit(dir + "/good.json", resp(game("C1", "Cmd One") + "," + game("C2", "Cmd Two", "PS4")));
    check(grab(cmd_import_json(db, dir + "/good.json", false, false, &out), "import", 0) && has(out, "PSN IMPORT OK") && has(out, "nothing contacted Sony") && has(out, "not installed, not runnable, not a download"), "cmd: import-json reports what it did and what it is not");
    check(grab(cmd_import_json(db, dir + "/good.json", true, true, &out), "import json", 0) && has(out, "\"unchanged\":2"), "cmd: re-import is unchanged (json)");
    spit(dir + "/bad.json", "{\"data\":{\"nope\":1}}");
    const size_t before = db.list_owned().size();
    check(grab(cmd_import_json(db, dir + "/bad.json", true, false, &out), "bad import", 1) && has(out, "schema-change") && has(out, "nothing was imported") && db.list_owned().size() == before && db.get_owned("C1")->in_last_sync, "cmd: schema change imports nothing and marks nothing");
    spit(dir + "/err.json", "{\"errors\":[{\"message\":\"token " + kTok + " expired\"}]}");
    check(grab(cmd_import_json(db, dir + "/err.json", false, false, &out), "sony errors", 1) && has(out, "sony-errors") && !has(out, kTok), "cmd: Sony errors are shown scrubbed");
    check(grab(cmd_import_json(db, dir + "/missing.json", false, false, &out), "missing file", 1) && has(out, "bad-input"), "cmd: missing file is bad-input");
    check(grab(cmd_import_json(db, dir, false, false, &out), "dir as file", 1), "cmd: a directory is not a response file");
    check(grab(cmd_owned(db, false, "", &out), "owned", 0) && has(out, "OWNED ON PSN") && has(out, "Cmd One") && has(out, "Owned on PSN (no PC build linked)") && has(out, "PNR has no download feature"), "cmd: owned lists with the access/download disclaimer");
    check(grab(cmd_owned(db, false, "ps4", &out), "owned ps4", 0) && has(out, "Cmd Two") && !has(out, "Cmd One"), "cmd: owned --platform filter (case-insensitive)");
    check(grab(cmd_owned(db, true, "", &out), "owned json", 0) && has(out, "\"installed\":false") && has(out, "\"runnable\":false") && has(out, "sony_downloadable_flag"), "cmd: owned --json says installed:false runnable:false");
    check(grab(cmd_link(db, "C1", "real.game", false, &out), "link", 0) && has(out, "does not make the game runnable"), "cmd: link says it changes nothing about running");
    check(grab(cmd_owned(db, false, "", &out), "owned linked", 0) && has(out, "linked to PNR game real.game, linked by a person") && has(out, "state: Untested"), "cmd: a linked entry shows the game's compatibility state");
    db.set_compatibility_human({"real.game", "Launches Only", "n"});
    check(grab(cmd_owned(db, false, "", &out), "owned linked2", 0) && has(out, "state: Launches Only"), "cmd: linked entry follows the existing compatibility engine");
    check(grab(cmd_link(db, "C1", "ghost", false, &out), "link ghost", 2) && has(out, "LINK REFUSED"), "cmd: link to unknown game refused");
    check(grab(cmd_unlink(db, "C1", false, &out), "unlink", 0) && grab(cmd_unlink(db, "NOPE", false, &out), "unlink nope", 2), "cmd: unlink and refusal");
    // disconnect
    check(grab(cmd_disconnect(db, tp, true, false, false, &out), "disc dry", 3) && has(out, "WOULD DELETE") && fs::exists(tp) && !db.list_owned().empty(), "cmd: disconnect --forget-owned is a dry run without --yes");
    check(grab(cmd_disconnect(db, tp, false, false, false, &out), "disc", 0) && !fs::exists(tp) && !db.list_owned().empty() && has(out, "cannot revoke the token at Sony"), "cmd: disconnect removes the token, keeps owned rows, says it cannot revoke at Sony");
    check(grab(cmd_disconnect(db, tp, false, false, false, &out), "disc again", 0), "cmd: disconnect twice is fine");
    check(grab(cmd_disconnect(db, tp, true, true, true, &out), "forget", 0) && db.list_owned().empty() && has(out, "\"owned_deleted\":true"), "cmd: --forget-owned --yes deletes owned rows");
    check(db.list_games().size() == 1, "cmd: installed games survive disconnect");
    check(!has(all, kTok), "secrets: the token never appears in any command output");
    check(!has(slurp(dir + "/lib/library.db"), kTok) && !has(slurp(dir + "/lib/library.db-wal"), kTok), "secrets: the token is never written to the library database");
    check(token_path_for(dir + "/a/library.db") == dir + "/a/psn/npsso.token", "cmd: default token path sits beside the library, outside the games tree");
  }
  db.close();
  fs::remove_all(dir);
}
