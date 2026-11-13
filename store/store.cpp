#include "store/store.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "psn/json.hpp"

namespace fs = std::filesystem;
namespace pnr::store {
namespace {
bool clean_text(const std::string& s, size_t max, bool allow_empty) {
  if ((s.empty() && !allow_empty) || s.size() > max) return false;
  for (unsigned char c : s) if (c < 0x20 || c == 0x7f) return false;
  return true;
}
bool is_hex64(const std::string& s) {
  if (s.size() != 64) return false;
  for (unsigned char c : s) if (!(std::isdigit(c) || (c >= 'a' && c <= 'f'))) return false;
  return true;
}
bool valid_pkg_id(const std::string& s) {
  if (s.empty() || s.size() > 64) return false;
  for (unsigned char c : s) if (!(std::islower(c) || std::isdigit(c) || c == '.' || c == '-' || c == '_')) return false;
  return s[0] != '.' && s[0] != '-';
}
std::string esc(const std::string& s) {
  std::string o;
  for (unsigned char c : s) {
    if (c == '"') o += "\\\""; else if (c == '\\') o += "\\\\";
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += static_cast<char>(c);
  }
  return o;
}
Status write_atomic(const std::string& path, const std::string& data) {
  std::error_code ec;
  fs::create_directories(fs::path(path).parent_path(), ec);
  const std::string tmp = path + ".tmp";
  ::unlink(tmp.c_str());
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  if (fd < 0) return Status::Fail(kErrIo, "store", "cannot write the store directory");
  bool ok = ::write(fd, data.data(), data.size()) == static_cast<ssize_t>(data.size()) && ::fsync(fd) == 0;
  ::close(fd);
  if (!ok || ::rename(tmp.c_str(), path.c_str()) != 0) { ::unlink(tmp.c_str()); return Status::Fail(kErrIo, "store", "cannot write the store directory"); }
  return Status::Ok();
}
Status read_file(const std::string& path, std::string* out, size_t max) {
  std::error_code ec;
  if (!fs::is_regular_file(path, ec) || fs::is_symlink(path, ec)) return Status::Fail(kErrIo, "store", "file missing");
  if (fs::file_size(path, ec) > max) return Status::Fail(kErrIo, "store", "file too large");
  std::ifstream f(path, std::ios::binary);
  out->assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return Status::Ok();
}
const std::string* str_of(const psn::Json& o, const char* k) {
  const psn::Json* v = o.get(k);
  return (v && v->is(psn::Json::T::Str)) ? &v->s : nullptr;
}
Status bad(const std::string& m) { return Status::Fail(kErrBadIndex, "store", m); }
}  // namespace

std::string label_for(const Package& p) {
  if (p.kind != "pnr-package") return "unsupported";
  return p.experimental ? "experimental" : "supported";
}

bool valid_source_id(const std::string& id) {
  if (id.empty() || id.size() > 40) return false;
  for (unsigned char c : id) if (!(std::islower(c) || std::isdigit(c) || c == '.' || c == '-' || c == '_')) return false;
  return id[0] != '.' && id[0] != '-' && id.find("..") == std::string::npos;
}

Status parse_index(const std::string& text, const Source& src, Index* out) {
  *out = Index{};
  psn::Json j; std::string e;
  if (!psn::json_parse(text, &j, &e) || !j.is(psn::Json::T::Obj)) return bad("index is not valid JSON");
  const std::string* fmt = str_of(j, "format");
  if (!fmt || *fmt != "pnr-store-index-1") return bad("unknown or missing index format (expected pnr-store-index-1)");
  const std::string* nm = str_of(j, "name");
  const std::string* rt = str_of(j, "rights");
  if (!nm || !clean_text(*nm, 120, false)) return bad("index needs a name (1..120 characters)");
  if (!rt || !clean_text(*rt, 2000, false)) return bad("index needs a rights statement");
  const psn::Json* pk = j.get("packages");
  if (!pk || !pk->is(psn::Json::T::Arr)) return bad("index has no packages array");
  if (pk->a.size() > 5000) return bad("index lists more than 5000 packages");
  Index idx; idx.name = *nm; idx.rights = *rt;
  std::vector<std::string> seen;
  for (size_t i = 0; i < pk->a.size(); ++i) {
    const psn::Json& p = pk->a[i];
    const std::string at = "package " + std::to_string(i + 1) + ": ";
    if (!p.is(psn::Json::T::Obj)) return bad(at + "not an object");
    Package k; k.source_id = src.id;
    auto need = [&](const char* key, std::string* dst, size_t max, bool allow_empty) -> bool {
      const std::string* v = str_of(p, key);
      if (!v) return allow_empty;
      if (!clean_text(*v, max, allow_empty)) return false;
      *dst = *v; return true;
    };
    if (!need("id", &k.id, 64, false) || k.id.empty() || !valid_pkg_id(k.id)) return bad(at + "bad id");
    if (std::find(seen.begin(), seen.end(), k.id) != seen.end()) return bad(at + "duplicate id " + k.id);
    seen.push_back(k.id);
    if (!need("name", &k.name, 120, false) || k.name.empty()) return bad(at + "bad name");
    if (!need("version", &k.version, 40, false) || k.version.empty()) return bad(at + "bad version");
    if (!need("license", &k.license, 120, false) || k.license.empty()) return bad(at + "license is required");
    if (!need("description", &k.description, 1000, true) || !need("category", &k.category, 40, true) || !need("rights", &k.rights, 2000, true)) return bad(at + "bad text field");
    if (!need("kind", &k.kind, 40, false) || k.kind.empty()) return bad(at + "kind is required");
    const std::string* sh = str_of(p, "sha256");
    if (!sh || !is_hex64(*sh)) return bad(at + "sha256 must be 64 lowercase hex characters");
    k.sha256 = *sh;
    const psn::Json* sz = p.get("size");
    if (!sz || !sz->is(psn::Json::T::Num) || sz->n < 1 || sz->n > 68719476736.0 || sz->n != static_cast<double>(static_cast<long long>(sz->n))) return bad(at + "size must be a whole number of bytes, 1 to 64 GiB");
    k.size = static_cast<long long>(sz->n);
    const std::string* u = str_of(p, "url");
    std::string sc, host;
    if (!u || !clean_text(*u, 2048, false) || !psn::url_host(*u, &sc, &host) || sc != "https") return bad(at + "url must be https");
    std::string host_l = host; for (auto& c : host_l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (host_l != src.host) return bad(at + "download host differs from the source's index host (refused)");
    k.url = *u;
    const psn::Json* ex = p.get("experimental");
    k.experimental = ex && ex->is(psn::Json::T::Bool) && ex->b;
    idx.packages.push_back(std::move(k));
  }
  *out = std::move(idx);
  return Status::Ok();
}

std::string store_dir_for(const std::string& db_path) {
  if (const char* e = std::getenv("PNR_STORE_DIR")) if (*e) return e;
  return (fs::path(db_path).parent_path() / "store").string();
}

Status Registry::load() {
  sources_.clear();
  std::string t;
  const std::string p = dir_ + "/sources.json";
  std::error_code ec;
  if (!fs::exists(p, ec)) return Status::Ok();
  Status s = read_file(p, &t, 1u << 20);
  if (!s) return Status::Fail(kErrIo, "store", "sources.json unreadable");
  psn::Json j; std::string e;
  const psn::Json* arr = nullptr;
  if (!psn::json_parse(t, &j, &e) || !(arr = j.get("sources")) || !arr->is(psn::Json::T::Arr)) return Status::Fail(kErrIo, "store", "sources.json is damaged; remove it to start over");
  for (const auto& o : arr->a) {
    const std::string* id = str_of(o, "id"); const std::string* u = str_of(o, "index_url"); const std::string* r = str_of(o, "rights");
    std::string sc, host;
    if (!id || !u || !r || !valid_source_id(*id) || !psn::url_host(*u, &sc, &host) || sc != "https") return Status::Fail(kErrIo, "store", "sources.json has an invalid entry; remove it to start over");
    for (auto& c : host) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    sources_.push_back({*id, *u, *r, host});
  }
  return Status::Ok();
}

const Source* Registry::find(const std::string& id) const {
  for (auto& s : sources_) if (s.id == id) return &s;
  return nullptr;
}

Status Registry::save() {
  std::string o = "{\"sources\":[";
  for (size_t i = 0; i < sources_.size(); ++i)
    o += std::string(i ? "," : "") + "{\"id\":\"" + esc(sources_[i].id) + "\",\"index_url\":\"" + esc(sources_[i].index_url) + "\",\"rights\":\"" + esc(sources_[i].rights) + "\"}";
  o += "]}\n";
  return write_atomic(dir_ + "/sources.json", o);
}

Status Registry::add(const std::string& id, const std::string& url, const std::string& rights) {
  if (!valid_source_id(id)) return Status::Fail(kErrBadSource, "store", "source id must be 1..40 characters of a-z 0-9 . - _");
  std::string sc, host;
  if (!clean_text(url, 2048, false) || !psn::url_host(url, &sc, &host) || sc != "https") return Status::Fail(kErrBadSource, "store", "index URL must be https (no user-info, port 443 only)");
  if (!clean_text(rights, 2000, false) || rights.find_first_not_of(" ") == std::string::npos) return Status::Fail(kErrBadSource, "store", "a rights statement is required (what you know about the source's right to distribute its content)");
  if (find(id)) return Status::Fail(kErrDuplicate, "store", "a source with that id already exists");
  for (auto& c : host) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  sources_.push_back({id, url, rights, host});
  Status s = save();
  if (!s) sources_.pop_back();
  return s;
}

Status Registry::remove(const std::string& id, bool* existed) {
  auto it = std::find_if(sources_.begin(), sources_.end(), [&](const Source& s) { return s.id == id; });
  *existed = it != sources_.end();
  if (!*existed) return Status::Ok();
  Source keep = *it;
  sources_.erase(it);
  Status s = save();
  if (!s) { sources_.push_back(keep); return s; }
  std::error_code ec;
  fs::remove(index_path(id), ec);
  return Status::Ok();
}

Status Registry::save_index_text(const std::string& id, const std::string& text) { return write_atomic(index_path(id), text); }

Status Registry::load_index(const std::string& id, Index* out) {
  const Source* s = find(id);
  if (!s) return Status::Fail(kErrNoSource, "store", "no such source");
  std::string t;
  Status r = read_file(index_path(id), &t, psn::kJsonMaxInput);
  if (!r) return Status::Fail(kErrNoSource, "store", "no cached index for this source: run 'store refresh'");
  return parse_index(t, *s, out);
}

Status refresh_source(psn::HttpClient& raw, Registry& reg, const std::string& id, int* packages) {
  const Source* s = reg.find(id);
  if (!s) return Status::Fail(kErrNoSource, "store", "no such source");
  psn::GuardedHttp http(raw, {s->host});
  psn::HttpRequest rq; rq.url = s->index_url;
  rq.headers.push_back("Accept: application/json");
  psn::HttpResponse rs;
  Status st = http.perform(rq, &rs);
  if (!st) return st;
  if (rs.status != 200) return Status::Fail(kErrBadIndex, "store", "source answered HTTP " + std::to_string(rs.status) + " (redirects are not followed)");
  Index idx;
  st = parse_index(rs.body, *s, &idx);
  if (!st) return st;
  st = reg.save_index_text(id, rs.body);
  if (!st) return st;
  *packages = static_cast<int>(idx.packages.size());
  return Status::Ok();
}

}  // namespace pnr::store
