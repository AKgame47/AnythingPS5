// Store core (slice A1) tests: registry, strict index validation, search,
// refresh against a scripted fake HTTP client. No real source is contacted.
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "psn/http.hpp"
#include "store/store_cli.hpp"

namespace fs = std::filesystem;
using namespace pnr::store;
namespace {
bool has(const std::string& h, const std::string& n) { return h.find(n) != std::string::npos; }
const std::string kHash(64, 'a');
std::string pkg(const std::string& id, const std::string& extra = "", const std::string& url = "https://games.example.invalid/p/x.pnrgame") {
  return "{\"id\":\"" + id + "\",\"name\":\"Game " + id + "\",\"version\":\"1.0\",\"license\":\"MIT\",\"kind\":\"pnr-package\",\"size\":1234,\"sha256\":\"" + kHash + "\",\"url\":\"" + url + "\",\"description\":\"A tiny demo\",\"category\":\"puzzle\"" + extra + "}";
}
std::string index_of(const std::string& pk, const std::string& head = "\"format\":\"pnr-store-index-1\",\"name\":\"Demo\",\"rights\":\"Own games by the author\"") {
  return "{" + head + ",\"packages\":[" + pk + "]}";
}
struct Fake : pnr::psn::HttpClient {
  int status = 200; std::string body; std::vector<std::string> urls; bool fail = false;
  pnr::Status perform(const pnr::psn::HttpRequest& r, pnr::psn::HttpResponse* res) override {
    urls.push_back(r.url); *res = pnr::psn::HttpResponse{};
    if (fail) return pnr::Status::Fail(214, "psn", "tls");
    res->status = status; res->body = body; return pnr::Status::Ok();
  }
};
}  // namespace

void pnr_store_tests(int& pass, int& fail) {
  auto check = [&](bool ok, const std::string& label) { if (ok) ++pass; else ++fail; std::printf("%s %s\n", ok ? "PASS" : "FAIL", label.c_str()); };
  const std::string d = "/tmp/pnr_store_" + std::to_string(::getpid());
  fs::remove_all(d); fs::create_directories(d);
  std::string out;
  const std::string url = "https://games.example.invalid/index.json";

  // registry
  {
    Registry r(d + "/a"); check(r.load().ok && r.sources().empty(), "store: starts with no sources (no default source)");
    check(cmd_source_list(r, false, &out) == 0 && has(out, "none") && has(out, "starts empty"), "store: empty list says so");
    check(cmd_source_add(r, "demo", url, "", false, &out) == 1 && has(out, "rights statement is required"), "store: a rights statement is required");
    check(cmd_source_add(r, "demo", url, "   ", false, &out) == 1, "store: a blank rights statement is refused");
    for (const char* u : {"http://games.example.invalid/i.json", "https://user@games.example.invalid/i", "https://games.example.invalid:8443/i", "ftp://x/y", "file:///etc/passwd", "https://", "", "https://games.example.invalid./i"})
      check(cmd_source_add(r, "s", u, "mine", false, &out) == 1, std::string("store: source URL refused [") + u + "]");
    for (const char* id : {"", "A", "../x", ".hidden", "-x", "a b", "a/b", "x..y"}) check(cmd_source_add(r, id, url, "mine", false, &out) == 1, std::string("store: source id refused [") + id + "]");
    check(cmd_source_add(r, "demo", url, "Author's own games", false, &out) == 0 && has(out, "cannot be verified") && has(out, "nothing was fetched"), "store: add works, says nothing fetched and rights are unverified");
    check(cmd_source_add(r, "demo", url, "x", false, &out) == 1 && has(out, "already exists"), "store: duplicate id refused");
    Registry r2(d + "/a"); check(r2.load().ok && r2.sources().size() == 1 && r2.sources()[0].rights == "Author's own games" && r2.sources()[0].host == "games.example.invalid", "store: registry persists and reloads");
    check(cmd_source_remove(r2, "demo", false, false, &out) == 3 && r2.sources().size() == 1, "store: remove is a dry run without --yes");
    check(cmd_source_remove(r2, "nope", true, false, &out) == 2, "store: removing an unknown source is exit 2");
    check(cmd_source_remove(r2, "demo", true, false, &out) == 0 && r2.sources().empty(), "store: remove with --yes");
    { std::ofstream f(d + "/a/sources.json"); f << "{garbage"; }
    Registry r3(d + "/a"); check(!r3.load().ok, "store: a damaged registry is reported, not silently emptied");
  }

  // index validation
  {
    Source s{"demo", url, "mine", "games.example.invalid"}; Index idx;
    check(parse_index(index_of(pkg("alpha")), s, &idx).ok && idx.packages.size() == 1 && idx.packages[0].size == 1234 && label_for(idx.packages[0]) == "supported", "index: a valid index parses; pnr-package is 'supported'");
    check(parse_index(index_of(pkg("alpha", ",\"experimental\":true")), s, &idx).ok && label_for(idx.packages[0]) == "experimental", "index: experimental flag gives 'experimental'");
    std::string other = pkg("beta"); other.replace(other.find("pnr-package"), 11, "ps5-pkg");
    check(parse_index(index_of(other), s, &idx).ok && label_for(idx.packages[0]) == "unsupported", "index: any other kind is 'unsupported', never installable");
    check(parse_index("{\"format\":\"pnr-store-index-1\",\"name\":\"N\",\"rights\":\"r\",\"packages\":[]}", s, &idx).ok && idx.packages.empty(), "index: an empty package list is valid");
    auto rej = [&](const std::string& t, const std::string& label) { check(!parse_index(t, s, &idx).ok && idx.packages.empty(), "index: rejected, " + label); };
    rej("nope", "not JSON"); rej("[]", "not an object");
    rej(index_of(pkg("a"), "\"format\":\"v2\",\"name\":\"N\",\"rights\":\"r\""), "unknown format");
    rej(index_of(pkg("a"), "\"format\":\"pnr-store-index-1\",\"name\":\"N\""), "no rights statement");
    rej(index_of(pkg("a"), "\"format\":\"pnr-store-index-1\",\"rights\":\"r\""), "no name");
    rej("{\"format\":\"pnr-store-index-1\",\"name\":\"N\",\"rights\":\"r\"}", "no packages");
    rej(index_of(pkg("A")), "uppercase id"); rej(index_of(pkg("../x")), "path-like id"); rej(index_of(pkg("")), "empty id");
    rej(index_of(pkg("a") + "," + pkg("a")), "duplicate package id");
    std::string nolic = pkg("a"); nolic.replace(nolic.find("\"license\":\"MIT\""), 15, "\"license\":\"\""); rej(index_of(nolic), "empty license");
    std::string sh = pkg("a"); sh.replace(sh.find(kHash), 64, std::string(63, 'a')); rej(index_of(sh), "short hash");
    sh = pkg("a"); sh.replace(sh.find(kHash), 64, std::string(64, 'A')); rej(index_of(sh), "uppercase hash");
    sh = pkg("a"); sh.replace(sh.find(kHash), 64, std::string(64, 'g')); rej(index_of(sh), "non-hex hash");
    std::string sz = pkg("a"); sz.replace(sz.find("1234"), 4, "0"); rej(index_of(sz), "zero size");
    sz = pkg("a"); sz.replace(sz.find("1234"), 4, "12.5"); rej(index_of(sz), "fractional size");
    sz = pkg("a"); sz.replace(sz.find("1234"), 4, "-5"); rej(index_of(sz), "negative size");
    sz = pkg("a"); sz.replace(sz.find("1234"), 4, "99999999999999"); rej(index_of(sz), "size over 64 GiB");
    rej(index_of(pkg("a", "", "http://games.example.invalid/x")), "http download url");
    rej(index_of(pkg("a", "", "https://other.example.invalid/x")), "download from a different host than the index");
    rej(index_of(pkg("a", "", "https://games.example.invalid.evil.invalid/x")), "look-alike host");
    rej(index_of(pkg("a", "", "https://u@games.example.invalid/x")), "user-info in download url");
    rej(index_of(pkg("a", ",\"description\":\"bad\\u0001ctl\"")), "control character in text");
    std::string many = "{\"format\":\"pnr-store-index-1\",\"name\":\"N\",\"rights\":\"r\",\"packages\":[";
    for (int i = 0; i < 5001; ++i) many += std::string(i ? "," : "") + pkg("p" + std::to_string(i));
    rej(many + "]}", "more than 5000 packages");
    check(parse_index(index_of(pkg("a", ",\"surprise\":{\"x\":1}")), s, &idx).ok, "index: unknown fields are ignored, never acted on");
  }

  // refresh, search, show
  {
    Registry r(d + "/b"); r.load();
    cmd_source_add(r, "demo", url, "Author's own games", false, &out);
    Fake f; f.body = index_of(pkg("alpha") + "," + pkg("beta", ",\"category\":\"racing\""));
    check(cmd_refresh(r, "demo", false, &out, &f) == 0 && has(out, "2 packages") && f.urls.size() == 1 && f.urls[0] == url, "refresh: fetches the source's own index URL");
    check(cmd_search(r, "", "", false, &out) == 0 && has(out, "2 package(s)") && has(out, "[supported] demo/alpha") && has(out, "store get <source> <package>"), "search: lists packages with labels and the no-download note");
    check(cmd_search(r, "alph", "", true, &out) == 0 && has(out, "\"id\":\"alpha\"") && !has(out, "\"id\":\"beta\""), "search: substring filter (json)");
    check(cmd_search(r, "GAME", "", false, &out) == 0 && has(out, "2 package(s)"), "search: case-insensitive");
    check(cmd_search(r, "zzz", "", false, &out) == 0 && has(out, "0 package(s)"), "search: no match is not an error");
    check(cmd_search(r, "", "nope", false, &out) == 2, "search: unknown source is exit 2");
    check(cmd_show(r, "demo", "alpha", false, &out) == 0 && has(out, "sha256 (from the source's index): " + kHash) && has(out, "declared by you for the source: Author's own games") && has(out, "cannot be verified"), "show: displays hash, source and every declared rights statement");
    check(cmd_show(r, "demo", "missing", false, &out) == 2, "show: unknown package is exit 2");
    // a bad refresh must not replace the cache
    f.body = "garbage";
    check(cmd_refresh(r, "demo", false, &out, &f) == 1 && has(out, "FAILED") && has(out, "cached index unchanged"), "refresh: an invalid index is refused");
    check(cmd_search(r, "", "", false, &out) == 0 && has(out, "2 package(s)"), "refresh: the previous valid index is kept after a bad one");
    f.body = index_of(pkg("alpha", "", "https://evil.invalid/x"));
    check(cmd_refresh(r, "demo", false, &out, &f) == 1 && cmd_search(r, "", "", false, &out) == 0 && has(out, "2 package(s)"), "refresh: an index pointing downloads at another host is refused whole");
    f.status = 404; f.body = index_of(pkg("x"));
    check(cmd_refresh(r, "demo", false, &out, &f) == 1 && has(out, "HTTP 404"), "refresh: non-200 is a failure");
    f.status = 302;
    check(cmd_refresh(r, "demo", false, &out, &f) == 1 && has(out, "redirects are not followed"), "refresh: a redirect is not followed");
    f.fail = true;
    check(cmd_refresh(r, "demo", false, &out, &f) == 1, "refresh: transport failure is a failure");
    check(cmd_refresh(r, "nope", false, &out, &f) == 2, "refresh: unknown source is exit 2");
    // tampering with the cached file on disk is caught on read
    { std::ofstream cf(r.index_path("demo")); cf << index_of(pkg("alpha", "", "https://evil.invalid/x")); }
    check(cmd_search(r, "", "", false, &out) == 0 && has(out, "0 package(s)") && has(out, "sources without a usable index"), "search: a tampered cached index is re-validated and not shown");
    // removing a source deletes its cache
    cmd_source_remove(r, "demo", true, false, &out);
    check(!fs::exists(r.index_path("demo")), "remove: cached index deleted with the source");
    // GuardedHttp scoped to the source host: a registry entry for host A cannot fetch host B
    Registry r4(d + "/c"); r4.load(); cmd_source_add(r4, "a", "https://a.example.invalid/i.json", "mine", false, &out);
    Fake g; g.body = index_of(pkg("alpha", "", "https://a.example.invalid/x"), "\"format\":\"pnr-store-index-1\",\"name\":\"N\",\"rights\":\"r\"");
    check(cmd_refresh(r4, "a", false, &out, &g) == 0 && g.urls.size() == 1, "refresh: fetch only touches the source's own host");
  }
  fs::remove_all(d);
}
