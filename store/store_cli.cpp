#include "store/store_cli.hpp"

#include <algorithm>
#include <cctype>
#include <csignal>
#include <memory>

namespace pnr::store {
namespace {
volatile std::sig_atomic_t g_stop = 0;
extern "C" void on_sigint(int) { g_stop = 1; }
std::string esc(const std::string& s) {
  std::string o;
  for (unsigned char c : s) {
    if (c == '"') o += "\\\""; else if (c == '\\') o += "\\\\";
    else if (c == '\n') o += "\\n"; else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += static_cast<char>(c);
  }
  return o;
}
std::string q(const std::string& s) { return "\"" + esc(s) + "\""; }
std::string fail(const Status& s, bool json) {
  if (json) return "{\"ok\":false,\"code\":" + std::to_string(s.error.code) + ",\"message\":" + q(s.error.message) + "}\n";
  return "store error: " + s.error.message + "\n";
}
std::string lower(std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
std::string size_text(long long b) {
  const char* u[] = {"B", "KiB", "MiB", "GiB"};
  double v = static_cast<double>(b); int i = 0;
  while (v >= 1024 && i < 3) { v /= 1024; ++i; }
  char buf[32]; std::snprintf(buf, sizeof buf, i ? "%.1f %s" : "%.0f %s", v, u[i]);
  return buf;
}
const char* kNote = "rights are declared by the source and cannot be verified by PNR; you are responsible for the sources you add\n";
std::string pkg_json(const Package& p) {
  return "{\"source\":" + q(p.source_id) + ",\"id\":" + q(p.id) + ",\"name\":" + q(p.name) + ",\"version\":" + q(p.version) + ",\"label\":" + q(label_for(p)) +
         ",\"license\":" + q(p.license) + ",\"category\":" + q(p.category) + ",\"size\":" + std::to_string(p.size) + ",\"sha256\":" + q(p.sha256) + "}";
}
}  // namespace

int cmd_source_add(Registry& r, const std::string& id, const std::string& url, const std::string& rights, bool json, std::string* out) {
  Status s = r.add(id, url, rights);
  if (!s) { *out = fail(s, json); return 1; }
  *out = json ? "{\"ok\":true,\"id\":" + q(id) + "}\n"
              : "SOURCE ADDED: " + id + "\n  index: " + url + "\n  declared rights: " + rights + "\n  nothing was fetched yet: run 'store refresh " + id + "'\n  " + kNote;
  return 0;
}

int cmd_source_remove(Registry& r, const std::string& id, bool yes, bool json, std::string* out) {
  if (!r.find(id)) { *out = fail(Status::Fail(kErrNoSource, "store", "no such source"), json); return 2; }
  if (!yes) { *out = json ? "{\"ok\":false,\"needs_confirmation\":true}\n" : "WOULD REMOVE source " + id + " and its cached index (add --yes). Installed games are never touched.\n"; return 3; }
  bool ex = false;
  Status s = r.remove(id, &ex);
  if (!s) { *out = fail(s, json); return 1; }
  *out = json ? "{\"ok\":true,\"removed\":true}\n" : "SOURCE REMOVED: " + id + " (its cached index was deleted; installed games are untouched)\n";
  return 0;
}

int cmd_source_pin(Registry& r, const std::string& id, const std::string& hex, bool json, std::string* out) {
  Status s = r.set_pubkey(id, hex == "none" ? std::string() : hex);
  if (!s) { *out = fail(s, json); return 2; }
  *out = json ? "{\"ok\":true}\n" : (hex == "none" ? "unpinned " + id + ": its packages are hash-only again\n" : "pinned Ed25519 key for " + id + ": its packages now must carry a valid signature to install. A signature proves the key holder produced the package, not that it is safe.\n");
  return 0;
}

int cmd_source_list(Registry& r, bool json, std::string* out) {
  if (json) {
    *out = "{\"sources\":[";
    for (size_t i = 0; i < r.sources().size(); ++i) { auto& s = r.sources()[i]; *out += std::string(i ? "," : "") + "{\"id\":" + q(s.id) + ",\"index_url\":" + q(s.index_url) + ",\"rights\":" + q(s.rights) + "}"; }
    *out += "]}\n";
    return 0;
  }
  if (r.sources().empty()) { *out = "STORE SOURCES: none. The store starts empty; add your own with 'store source add <id> <https index url> --rights \"...\"'.\n"; return 0; }
  *out = "STORE SOURCES (" + std::to_string(r.sources().size()) + ")\n";
  for (auto& s : r.sources()) {
    Index idx; Status l = r.load_index(s.id, &idx);
    *out += " " + s.id + "  " + s.index_url + "\n   declared rights: " + s.rights + "\n   signing: " + (s.pubkey.empty() ? "no pinned key (hash only)" : "pinned key " + s.pubkey.substr(0, 16) + "...") + "\n   index: " + (l ? std::to_string(idx.packages.size()) + " packages (" + idx.name + ")" : "not fetched or invalid: " + l.error.message) + "\n";
  }
  *out += kNote;
  return 0;
}

int cmd_refresh(Registry& r, const std::string& id, bool json, std::string* out, psn::HttpClient* client) {
  std::vector<std::string> ids;
  if (id.empty()) for (auto& s : r.sources()) ids.push_back(s.id); else ids.push_back(id);
  if (!id.empty() && !r.find(id)) { *out = fail(Status::Fail(kErrNoSource, "store", "no such source"), json); return 2; }
  if (ids.empty()) { *out = json ? "{\"ok\":true,\"results\":[]}\n" : "no sources to refresh\n"; return 0; }
  std::unique_ptr<psn::HttpClient> own;
  if (!client) {
    Status cs; own = psn::make_curl_client(psn::CurlOptions{}, &cs);
    if (!own) { *out = fail(cs, json); return 1; }
    client = own.get();
  }
  int rc = 0; std::string txt, js;
  for (auto& i : ids) {
    int n = 0;
    Status s = refresh_source(*client, r, i, &n);
    if (!s) { rc = 1; txt += " " + i + ": FAILED: " + s.error.message + " (cached index unchanged)\n"; js += std::string(js.empty() ? "" : ",") + "{\"id\":" + q(i) + ",\"ok\":false,\"message\":" + q(s.error.message) + "}"; }
    else { txt += " " + i + ": ok, " + std::to_string(n) + " packages\n"; js += std::string(js.empty() ? "" : ",") + "{\"id\":" + q(i) + ",\"ok\":true,\"packages\":" + std::to_string(n) + "}"; }
  }
  *out = json ? "{\"ok\":" + std::string(rc ? "false" : "true") + ",\"results\":[" + js + "]}\n" : "STORE REFRESH\n" + txt + "(index is checked before it replaces the cache; nothing was downloaded)\n";
  return rc;
}

int cmd_search(Registry& r, const std::string& text, const std::string& source, bool json, std::string* out) {
  const std::string t = lower(text);
  std::vector<Package> hits; std::string problems;
  for (auto& s : r.sources()) {
    if (!source.empty() && s.id != source) continue;
    Index idx; Status l = r.load_index(s.id, &idx);
    if (!l) { problems += " " + s.id + ": " + l.error.message + "\n"; continue; }
    for (auto& p : idx.packages)
      if (t.empty() || lower(p.id).find(t) != std::string::npos || lower(p.name).find(t) != std::string::npos || lower(p.description).find(t) != std::string::npos || lower(p.category).find(t) != std::string::npos) hits.push_back(p);
  }
  if (!source.empty() && !r.find(source)) { *out = fail(Status::Fail(kErrNoSource, "store", "no such source"), json); return 2; }
  if (json) {
    *out = "{\"packages\":[";
    for (size_t i = 0; i < hits.size(); ++i) *out += std::string(i ? "," : "") + pkg_json(hits[i]);
    *out += "]}\n";
    return 0;
  }
  *out = "STORE: " + std::to_string(hits.size()) + " package(s)\n";
  for (auto& p : hits)
    *out += " [" + label_for(p) + "] " + p.source_id + "/" + p.id + "  " + p.name + " " + p.version + "  " + size_text(p.size) + "  license: " + p.license + "\n";
  if (!problems.empty()) *out += "sources without a usable index:\n" + problems;
  *out += "labels: supported = a PNR package PNR can install; experimental = same, marked so by the source; unsupported = listed only, PNR cannot install it\n";
  *out += "download with: store get <source> <package>, then store queue run\n";
  return 0;
}

int cmd_show(Registry& r, const std::string& source, const std::string& pkg, bool json, std::string* out) {
  const Source* s = r.find(source);
  if (!s) { *out = fail(Status::Fail(kErrNoSource, "store", "no such source"), json); return 2; }
  Index idx; Status l = r.load_index(source, &idx);
  if (!l) { *out = fail(l, json); return 1; }
  for (auto& p : idx.packages) if (p.id == pkg) {
    if (json) { *out = pkg_json(p) + "\n"; return 0; }
    *out = p.name + " " + p.version + "\n  label: " + label_for(p) + "\n  source: " + p.source_id + " (" + s->index_url + ")\n  license: " + p.license + "\n  category: " + p.category +
           "\n  size: " + size_text(p.size) + "\n  sha256 (from the source's index): " + p.sha256 + "\n  description: " + p.description +
           "\n  rights, declared by you for the source: " + s->rights + "\n  rights, declared by the index: " + idx.rights + (p.rights.empty() ? "" : "\n  rights, declared for this package: " + p.rights) + "\n  " + kNote;
    return 0;
  }
  *out = fail(Status::Fail(kErrNoSource, "store", "no such package in that source"), json);
  return 2;
}


namespace {
std::string state_json(const DlState& s) {
  return "{\"source\":" + q(s.source) + ",\"package\":" + q(s.pkg) + ",\"status\":" + q(s.status) + ",\"bytes\":" + std::to_string(s.have) + ",\"size\":" + std::to_string(s.size) + ",\"error\":" + q(s.error) + "}";
}
std::string state_line(const DlState& s) {
  const int pct = s.size > 0 ? static_cast<int>(s.have * 100 / s.size) : 0;
  return " " + s.source + "/" + s.pkg + "  " + s.status + "  " + size_text(s.have) + " of " + size_text(s.size) + " (" + std::to_string(pct) + "%)" + (s.error.empty() ? "" : "  [" + s.error + "]") + "\n";
}
}  // namespace

int cmd_get(Queue& qu, const std::string& source, const std::string& pkg, bool json, std::string* out) {
  DlState st;
  Status s = qu.enqueue(source, pkg, &st);
  if (!s) { *out = fail(s, json); return s.error.code == kErrNoSource ? 2 : 1; }
  *out = json ? state_json(st) + "\n" : "QUEUED: " + source + "/" + pkg + " (" + size_text(st.size) + ")\n  nothing was downloaded yet: run 'store queue run'\n  it will be checked against the source's size and sha256 before it can be installed\n";
  return 0;
}

int cmd_queue_list(Queue& qu, bool json, std::string* out) {
  std::vector<DlState> v;
  Status s = qu.list(&v);
  if (!s) { *out = fail(s, json); return 1; }
  if (json) { *out = "{\"queue\":["; for (size_t i = 0; i < v.size(); ++i) *out += std::string(i ? "," : "") + state_json(v[i]); *out += "]}\n"; return 0; }
  *out = "STORE QUEUE (" + std::to_string(v.size()) + ")\n";
  for (auto& s2 : v) *out += state_line(s2);
  *out += "status: queued/partial = will run; paused = skipped; verified = size and sha256 match the index, ready to install; failed = see the note; installed\n";
  return 0;
}

int cmd_queue_run(Queue& qu, const std::string& source, const std::string& pkg, bool json, std::string* out, psn::HttpClient* client, const DownloadOptions* optp) {
  std::vector<DlState> todo;
  if (!pkg.empty()) { DlState st; Status e = qu.enqueue(source, pkg, &st); if (!e) { *out = fail(e, json); return e.error.code == kErrNoSource ? 2 : 1; } todo.push_back(st); }
  else { std::vector<DlState> all; qu.list(&all); for (auto& s : all) if (s.status == "queued" || s.status == "partial") todo.push_back(s); }
  if (todo.empty()) { *out = json ? "{\"ok\":true,\"ran\":[]}\n" : "nothing to download (queue empty or everything paused/complete)\n"; return 0; }
  std::unique_ptr<psn::HttpClient> own;
  if (!client) {
    Status cs; own = psn::make_curl_client(psn::CurlOptions{}, &cs);
    if (!own) { *out = fail(cs, json); return 1; }
    client = own.get();
    g_stop = 0;
    std::signal(SIGINT, on_sigint);
  }
  DownloadOptions opt = optp ? *optp : DownloadOptions{};
  if (!optp) opt.cancel = [] { return g_stop != 0; };
  int rc = 0; std::string txt, js;
  for (auto& t : todo) {
    DlState st;
    Status s = qu.run(*client, t.source, t.pkg, opt, &st);
    if (!s) { rc = 1; txt += " " + t.source + "/" + t.pkg + ": " + s.error.message + "\n"; js += std::string(js.empty() ? "" : ",") + "{\"source\":" + q(t.source) + ",\"package\":" + q(t.pkg) + ",\"ok\":false,\"message\":" + q(s.error.message) + "}"; if (s.error.code == kErrInterrupted) break; }
    else { txt += " " + t.source + "/" + t.pkg + ": " + st.status + ", " + size_text(st.have) + ", size and sha256 match the index\n"; js += std::string(js.empty() ? "" : ",") + "{\"source\":" + q(t.source) + ",\"package\":" + q(t.pkg) + ",\"ok\":true,\"status\":" + q(st.status) + "}"; }
  }
  *out = json ? "{\"ok\":" + std::string(rc ? "false" : "true") + ",\"ran\":[" + js + "]}\n" : "STORE QUEUE RUN\n" + txt + "(a verified file is not installed yet: use 'store install')\n";
  return rc;
}

int cmd_queue_pause(Queue& qu, const std::string& source, const std::string& pkg, bool pause, bool json, std::string* out) {
  Status s = qu.set_paused(source, pkg, pause);
  if (!s) { *out = fail(s, json); return s.error.code == kErrNoSource ? 2 : 1; }
  *out = json ? "{\"ok\":true}\n" : std::string(pause ? "PAUSED: " : "RESUMED: ") + source + "/" + pkg + "\n";
  return 0;
}

int cmd_queue_cancel(Queue& qu, const std::string& source, const std::string& pkg, bool yes, bool json, std::string* out) {
  if (!yes) { *out = json ? "{\"ok\":false,\"needs_confirmation\":true}\n" : "WOULD DELETE the partial download and queue entry for " + source + "/" + pkg + " (add --yes). Installed games are never touched.\n"; return 3; }
  Status s = qu.cancel(source, pkg);
  if (!s) { *out = fail(s, json); return s.error.code == kErrNoSource ? 2 : 1; }
  *out = json ? "{\"ok\":true}\n" : "CANCELED: " + source + "/" + pkg + " (partial file and entry deleted)\n";
  return 0;
}

}  // namespace pnr::store
