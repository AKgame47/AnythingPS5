// See research.hpp.
#include "research/research.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <thread>

#include "loader/elf_parser.hpp"
#include "package/importer.hpp"
#include "package/sha256.hpp"
#include "psn/json.hpp"
#include "psn/psn.hpp"
#include "store/download.hpp"

namespace fs = std::filesystem;
namespace pnr::research {
namespace {
Status fail(int code, const std::string& m) { return Status::Fail(code, "research", m); }

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
std::string esc(const std::string& s) {
  std::string o;
  for (unsigned char c : s) {
    if (c == '"') o += "\\\""; else if (c == '\\') o += "\\\\";
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += static_cast<char>(c);
  }
  return o;
}
std::string utc_now() {
  std::time_t t = std::time(nullptr); std::tm g{}; gmtime_r(&t, &g);
  char b[32]; std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &g);
  return b;
}
const std::string kZero(64, '0');
const char kHashKey[] = ",\"hash\":\"";

// body = the line without its trailing hash field
std::string entry_body(const LogEntry& e) {
  return "{\"seq\":" + std::to_string(e.seq) + ",\"time\":\"" + esc(e.time) + "\",\"kind\":\"" + esc(e.kind) + "\",\"subject\":\"" + esc(e.subject) +
         "\",\"text\":\"" + esc(e.text) + "\",\"sha256\":\"" + e.sha256 + "\",\"prev\":\"" + e.prev + "\"";
}
bool parse_line(const std::string& line, LogEntry* e, std::string* body_out) {
  const size_t k = line.rfind(kHashKey);
  if (k == std::string::npos || line.size() != k + sizeof(kHashKey) - 1 + 64 + 2 || line.compare(line.size() - 2, 2, "\"}") != 0) return false;
  e->hash = line.substr(k + sizeof(kHashKey) - 1, 64);
  *body_out = line.substr(0, k);
  psn::Json j; std::string err;
  if (!psn::json_parse(line, &j, &err) || !j.is(psn::Json::T::Obj)) return false;
  auto str = [&](const char* key, std::string* o) { const psn::Json* v = j.get(key); if (!v || !v->is(psn::Json::T::Str)) return false; *o = v->s; return true; };
  const psn::Json* sq = j.get("seq");
  if (!sq || !sq->is(psn::Json::T::Num)) return false;
  e->seq = static_cast<long long>(sq->n);
  return str("time", &e->time) && str("kind", &e->kind) && str("subject", &e->subject) && str("text", &e->text) && str("sha256", &e->sha256) && str("prev", &e->prev);
}
long long file_bytes(const std::string& p) { std::error_code ec; auto n = fs::file_size(p, ec); return ec ? -1 : static_cast<long long>(n); }
void rm(const std::string& p) { std::error_code ec; fs::remove(p, ec); }

bool sony_host(const std::string& host) {
  std::string h; for (unsigned char c : host) h += static_cast<char>(std::tolower(c));
  for (const char* d : {"playstation.com", "playstation.net", "sony.com", "sonyentertainmentnetwork.com", "sie.com", "scei.co.jp"}) {
    const std::string s = d;
    if (h == s || (h.size() > s.size() && h.compare(h.size() - s.size() - 1, s.size() + 1, "." + s) == 0)) return true;
  }
  return false;
}
}  // namespace

// ---------------------------------------------------------------- log
Status Log::append(const std::string& kind, const std::string& subject, const std::string& text, const std::string& sha256, LogEntry* out) {
  if (kind != "note" && kind != "fetch" && kind != "fetch-failed" && kind != "inspect") return fail(kErrBadInput, "unknown log kind");
  if (!clean_text(text, 1000, kind != "note") || !clean_text(subject, 2048, true)) return fail(kErrBadInput, "log text must be 1-1000 printable characters (subject up to 2048), no control characters or newlines");
  if (!sha256.empty() && !is_hex64(sha256)) return fail(kErrBadInput, "sha256 must be 64 lowercase hex characters");
  std::error_code ec; fs::create_directories(dir_, ec);
  int fd = ::open(path().c_str(), O_RDWR | O_CREAT | O_APPEND, 0600);
  if (fd < 0) return fail(kErrLogIO, "cannot open the research log for appending");
  if (::flock(fd, LOCK_EX) != 0) { ::close(fd); return fail(kErrLogIO, "cannot lock the research log"); }
  struct Unlock { int fd; ~Unlock() { ::flock(fd, LOCK_UN); ::close(fd); } } unlock{fd};
  std::vector<LogEntry> have; Status r = read(&have);
  if (!r) return r;  // never append to a broken chain
  LogEntry e;
  e.seq = have.empty() ? 1 : have.back().seq + 1;
  e.time = clock ? clock() : utc_now(); e.kind = kind; e.subject = subject; e.text = text; e.sha256 = sha256;
  e.prev = have.empty() ? kZero : have.back().hash;
  const std::string body = entry_body(e);
  e.hash = pkg::sha256_hex(body);
  const std::string line = body + kHashKey + e.hash + "\"}\n";
  ssize_t n = ::write(fd, line.data(), line.size());
  if (n != static_cast<ssize_t>(line.size())) return fail(kErrLogIO, "short write to the research log");
  ::fsync(fd);
  if (out) *out = e;
  return Status::Ok();
}

Status Log::read(std::vector<LogEntry>* out) {
  out->clear();
  std::ifstream f(path(), std::ios::binary);
  if (!f) return Status::Ok();  // no log yet
  std::string line, prev = kZero; long long expect = 1, ln = 0;
  while (std::getline(f, line)) {
    ++ln;
    LogEntry e; std::string body;
    if (!parse_line(line, &e, &body)) return fail(kErrLogCorrupt, "research log line " + std::to_string(ln) + " is not a valid entry");
    if (pkg::sha256_hex(body) != e.hash) return fail(kErrLogCorrupt, "research log line " + std::to_string(ln) + ": hash does not match its content (edited?)");
    if (e.prev != prev) return fail(kErrLogCorrupt, "research log line " + std::to_string(ln) + ": does not follow the previous entry (line removed or reordered?)");
    if (e.seq != expect) return fail(kErrLogCorrupt, "research log line " + std::to_string(ln) + ": sequence number is " + std::to_string(e.seq) + ", expected " + std::to_string(expect));
    prev = e.hash; ++expect; out->push_back(e);
  }
  return Status::Ok();
}

Status Log::verify(size_t* entries, std::string* detail) {
  std::vector<LogEntry> v; Status r = read(&v);
  *entries = v.size();
  if (!r) { *detail = r.error.message; return r; }
  *detail = v.empty() ? "no log yet" : "chain intact: " + std::to_string(v.size()) + " entries. This detects edits, deletions in the middle and reordering; it cannot detect removal of the newest entries, or a rewrite of the whole file.";
  return Status::Ok();
}

// ---------------------------------------------------------------- fetch
namespace {
bool parse_total(const std::string& content_range, long long* start, long long* total) {
  // "bytes a-b/total"
  long long a = 0, b = 0, t = 0;
  if (std::sscanf(content_range.c_str(), "bytes %lld-%lld/%lld", &a, &b, &t) != 3) return false;
  *start = a; *total = t; return true;
}
}  // namespace

Status fetch(psn::HttpClient& raw, Log& log, const std::string& url, const std::string& out_path, const FetchOptions& opt, FetchResult* res) {
  *res = FetchResult{};
  auto refuse = [&](int code, const std::string& m) { log.append("fetch-failed", url.size() <= 2048 ? url : "", "refused or failed: " + m.substr(0, 600), ""); return fail(code, m); };
  std::string scheme, host;
  if (!clean_text(url, 2048, false) || !psn::url_host(url, &scheme, &host) || scheme != "https") return fail(kErrBadInput, "not a plain https URL (no userinfo, port 443 only)");
  if (url.find('#') != std::string::npos) return fail(kErrBadInput, "URLs with a #fragment are not accepted");
  if (sony_host(host)) return refuse(kErrRefusedHost, "PNR does not fetch from Sony hosts (no authorized download mechanism exists)");
  if (!clean_text(opt.rights, 500, false)) return fail(kErrBadInput, "a rights statement is required (--rights \"...\"): say why you may download this. It is recorded, not verified");
  if (!opt.expect_sha256.empty() && !is_hex64(opt.expect_sha256)) return fail(kErrBadInput, "--sha256 must be 64 lowercase hex characters");
  if (out_path.empty() || out_path.size() > 4000) return fail(kErrBadInput, "output path missing");
  std::error_code ec;
  if (fs::is_directory(out_path, ec)) return fail(kErrBadInput, "the output path is a directory");
  if (fs::exists(out_path, ec) && !opt.force) return fail(kErrBadInput, "the output file already exists (use --force to replace it)");
  const std::string part = out_path + ".part", etag_file = out_path + ".part.etag";
  fs::path parent = fs::path(out_path).parent_path(); if (parent.empty()) parent = ".";
  if (!fs::is_directory(parent, ec)) return fail(kErrBadInput, "the output directory does not exist");

  psn::GuardedHttp http(raw, {host});
  std::string etag; { std::ifstream ef(etag_file); std::getline(ef, etag); }
  int retries = 0, delay = opt.backoff_ms; std::string last = "download did not complete";
  long long total = -1;  // declared by the server
  bool resumed = false;
  while (true) {
    long long have = file_bytes(part); if (have < 0) have = 0;
    if (have > opt.max_bytes) { rm(part); have = 0; }
    psn::HttpRequest rq; rq.url = url;
    if (have > 0) { rq.headers.push_back("Range: bytes=" + std::to_string(have) + "-"); if (!etag.empty()) rq.headers.push_back("If-Range: " + etag); }
    std::ofstream of; bool opened = false, bad = false; std::string bad_msg; long long offset = have, written = 0; bool disk = false;
    psn::HttpResponse rs;
    auto sink = [&](const psn::HttpResponse& hd, const char* d, size_t n) -> bool {
      if (!opened) {
        if (hd.status == 206) {
          long long st = 0, tt = 0;
          if (have == 0 || !parse_total(hd.content_range, &st, &tt) || st != have || tt <= have) { bad = true; bad_msg = "server sent an unexpected byte range"; return false; }
          total = tt; resumed = true; of.open(part, std::ios::binary | std::ios::app);
        } else {
          total = hd.content_length; offset = 0; resumed = false; of.open(part, std::ios::binary | std::ios::trunc);
          etag = hd.etag; { std::ofstream ef(etag_file, std::ios::trunc); ef << etag << "\n"; }
        }
        if (total > opt.max_bytes) { bad = true; bad_msg = "the file is larger than the --max-bytes limit"; return false; }
        if (total >= 0) {
          std::error_code sec; auto sp = fs::space(parent, sec);
          if (sec || static_cast<long long>(sp.available) < (total - offset) + opt.free_margin) { bad = true; disk = true; bad_msg = "not enough free disk space"; return false; }
        }
        if (!of) { bad = true; disk = true; bad_msg = "cannot write the partial file"; return false; }
        opened = true;
      }
      if (offset + written + static_cast<long long>(n) > opt.max_bytes) { bad = true; bad_msg = "the download exceeded the --max-bytes limit"; return false; }
      if (total >= 0 && offset + written + static_cast<long long>(n) > total) { bad = true; bad_msg = "server sent more bytes than it declared"; return false; }
      of.write(d, static_cast<std::streamsize>(n)); written += static_cast<long long>(n);
      if (!of) { bad = true; disk = true; bad_msg = "disk write failed"; return false; }
      return true;
    };
    Status t = http.download(rq, sink, &rs);
    if (opened) { of.flush(); of.close(); }
    if (bad) {
      if (disk) return refuse(kErrFetch, bad_msg + " (partial file kept)");
      rm(part); rm(etag_file); return refuse(kErrFetch, bad_msg + " (partial file deleted)");
    }
    bool retry = false;
    if (!t) { last = t.error.message; if (t.error.code == psn::kErrBlockedHost) return refuse(kErrFetch, last); retry = true; }
    else if (rs.status == 416) { rm(part); rm(etag_file); last = "server rejected the range; restarting"; retry = true; }
    else if (rs.status == 429 || (rs.status >= 500 && rs.status < 600)) { last = "server answered HTTP " + std::to_string(rs.status); retry = true; }
    else if (rs.status != 200 && rs.status != 206) {
      return refuse(kErrFetch, rs.status >= 300 && rs.status < 400 ? "the server redirected (redirects are not followed)" : "server answered HTTP " + std::to_string(rs.status));
    } else if (total >= 0 && file_bytes(part) != total) { last = "transfer ended early"; retry = true; }
    else break;
    if (!retry) break;
    if (++retries > opt.max_retries) return refuse(kErrFetch, "gave up after " + std::to_string(opt.max_retries) + " retries: " + last + " (the partial file is kept; run again to resume)");
    if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    delay *= 2;
  }
  std::string hex; long long n = 0;
  Status hs = store::hash_file(part, &hex, &n);
  if (!hs) return refuse(kErrFetch, "cannot read the downloaded file");
  if (!opt.expect_sha256.empty() && hex != opt.expect_sha256) {
    rm(part); rm(etag_file);
    return refuse(kErrHash, "sha256 does not match the one you supplied (expected " + opt.expect_sha256.substr(0, 12) + "..., got " + hex.substr(0, 12) + "...); the file was deleted");
  }
  if (opt.force) rm(out_path);
  if (::rename(part.c_str(), out_path.c_str()) != 0) return refuse(kErrFetch, "cannot move the finished file into place");
  rm(etag_file);
  res->path = out_path; res->sha256 = hex; res->bytes = n; res->hash_verified = !opt.expect_sha256.empty(); res->size_declared = total >= 0; res->resumed = resumed;
  std::string text = "fetched " + std::to_string(n) + " bytes to " + out_path + "; rights (declared, unverified): " + opt.rights + "; ";
  text += res->hash_verified ? "sha256 matched the hash you supplied" : "NO expected hash supplied, so integrity against the source is unverified (hash recorded)";
  if (!res->size_declared) text += "; server declared no size";
  if (text.size() > 1000) text = text.substr(0, 1000);
  Status ls = log.append("fetch", url, text, hex);
  if (!ls) return fail(kErrLogIO, "the file was fetched, but the research log could not be written: " + ls.error.message);
  return Status::Ok();
}

// ---------------------------------------------------------------- inspect
namespace {
std::string hexbytes(const std::string& s) {
  static const char* d = "0123456789abcdef"; std::string o;
  for (unsigned char c : s) { o += d[c >> 4]; o += d[c & 15]; }
  return o;
}
}  // namespace

Status inspect_file(const std::string& path, Inspection* in) {
  *in = Inspection{}; in->path = path;
  std::error_code ec;
  if (!fs::is_regular_file(path, ec)) return fail(kErrInspect, "not a readable regular file");
  Status hs = store::hash_file(path, &in->sha256, &in->size);
  if (!hs) return fail(kErrInspect, "cannot read the file");
  std::ifstream f(path, std::ios::binary);
  std::string head(1 << 20, '\0'); f.read(&head[0], static_cast<std::streamsize>(head.size())); head.resize(static_cast<size_t>(f.gcount()));
  in->magic_hex = hexbytes(head.substr(0, 8));
  if (!head.empty()) {
    long long cnt[256] = {0}; for (unsigned char c : head) cnt[c]++;
    double h = 0; for (int i = 0; i < 256; i++) if (cnt[i]) { double p = static_cast<double>(cnt[i]) / head.size(); h -= p * std::log2(p); }
    in->entropy = h;
  }
  auto fact = [&](const std::string& k, const std::string& v) { in->facts.emplace_back(k, v); };
  auto starts = [&](const char* m, size_t n) { return head.size() >= n && head.compare(0, n, m, n) == 0; };
  if (starts("\x7f" "CNT", 4)) {
    in->kind = "sony-package-container"; in->sony_container = true;
    in->notes.push_back("This looks like a Sony package container (magic 7f 43 4e 54). PNR does not parse, decrypt, install or run Sony packages. Only its magic, size, SHA-256 and entropy were computed; the container was not parsed.");
  } else if (starts("\x7f" "ELF", 4)) {
    auto pr = loader::parse_elf(path);
    in->kind = "elf";
    if (!pr.ok) { in->kind = "elf (invalid)"; fact("parse error", pr.error); }
    else {
      fact("class", pr.info.is_64bit ? "64-bit" : "32-bit"); fact("byte order", pr.info.little_endian ? "little" : "big");
      fact("type", pr.info.elf_type == 2 ? "ET_EXEC" : pr.info.elf_type == 3 ? "ET_DYN" : std::to_string(pr.info.elf_type));
      fact("machine", std::to_string(pr.info.machine) + (pr.info.machine == 62 ? " (x86-64)" : ""));
      char b[32]; std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(pr.info.entry)); fact("entry", b);
      fact("load segments", std::to_string(pr.info.segments.size()));
      fact("PNR can launch natively", loader::is_x86_64(pr.info) && loader::is_executable_type(pr.info) ? "yes (x86-64 executable)" : "no (not an x86-64 executable)");
    }
  } else if (head.size() >= 262 && head.compare(257, 5, "ustar") == 0) {
    auto ir = pkg::inspect_package(path);
    if (ir.status.ok) {
      in->kind = "pnrgame";
      fact("id", ir.manifest.id); fact("title", ir.manifest.title); fact("version", ir.manifest.version);
      fact("executable", ir.manifest.executable); fact("arch", ir.manifest.arch);
      fact("validation", "structure, manifest, every SHA-256 and the x86-64 ELF entry point passed (read-only; nothing extracted)");
    } else {
      in->kind = "tar (not a valid .pnrgame)"; fact("reason", ir.status.error.message);
    }
  } else if (starts("{", 1) || starts("[", 1)) {
    in->kind = "json";
    psn::Json j; std::string err; std::string all;
    if (in->size <= static_cast<long long>(psn::kJsonMaxInput)) { std::ifstream g(path, std::ios::binary); all.assign((std::istreambuf_iterator<char>(g)), std::istreambuf_iterator<char>()); }
    if (all.empty() || !psn::json_parse(all, &j, &err)) fact("parse", "not valid JSON (or larger than the parser limit)");
    else if (j.is(psn::Json::T::Obj)) {
      fact("top-level keys", std::to_string(j.o.size()));
      const psn::Json* pk = j.get("packages");
      if (pk && pk->is(psn::Json::T::Arr)) { in->kind = "json (looks like a PNR store index)"; fact("packages", std::to_string(pk->a.size())); in->notes.push_back("Structure and signatures are NOT validated here; use `store refresh` for that."); }
    } else if (j.is(psn::Json::T::Arr)) fact("array items", std::to_string(j.a.size()));
  } else if (starts("\x1f\x8b", 2)) { in->kind = "gzip (recognised, not opened)"; }
  else if (starts("PK\x03\x04", 4)) { in->kind = "zip (recognised, not opened)"; }
  else if (starts("SQLite format 3", 15)) { in->kind = "sqlite database (recognised, not opened)"; }
  else if (starts("\x89PNG", 4)) { in->kind = "png image (recognised, not opened)"; }
  else { in->kind = "unknown"; in->notes.push_back("Format not recognised by PNR."); }
  if (in->entropy > 7.5 && in->size > 4096) in->notes.push_back("High entropy (above 7.5 bits per byte) suggests compressed or encrypted data. That is an indication, not proof, and PNR does not try to decrypt anything.");
  return Status::Ok();
}

std::string render_inspection(const Inspection& i, bool json) {
  char eb[16]; std::snprintf(eb, sizeof eb, "%.2f", i.entropy);
  if (json) {
    std::string o = "{\"path\":\"" + esc(i.path) + "\",\"kind\":\"" + esc(i.kind) + "\",\"size\":" + std::to_string(i.size) + ",\"sha256\":\"" + i.sha256 + "\",\"entropy\":" + eb + ",\"magic\":\"" + i.magic_hex + "\",\"sony_container\":" + (i.sony_container ? "true" : "false") + ",\"facts\":{";
    for (size_t k = 0; k < i.facts.size(); ++k) o += (k ? "," : "") + std::string("\"") + esc(i.facts[k].first) + "\":\"" + esc(i.facts[k].second) + "\"";
    o += "},\"notes\":[";
    for (size_t k = 0; k < i.notes.size(); ++k) o += (k ? "," : "") + std::string("\"") + esc(i.notes[k]) + "\"";
    return o + "]}";
  }
  std::string o = "file:    " + i.path + "\nkind:    " + i.kind + "\nsize:    " + std::to_string(i.size) + " bytes\nsha256:  " + i.sha256 + "\nmagic:   " + i.magic_hex + "\nentropy: " + eb + " bits/byte (first 1 MiB)\n";
  for (auto& f : i.facts) o += "  " + f.first + ": " + f.second + "\n";
  for (auto& n : i.notes) o += "note: " + n + "\n";
  return o;
}

// ---------------------------------------------------------------- CLI
int cmd_research(const std::vector<std::string>& pos, const CliOptions& o, const std::string& dir, std::string* out, psn::HttpClient* client, const FetchOptions* fo) {
  Log log(dir);
  auto err = [&](const Status& s) { *out += "research: " + s.error.message + "\n"; return 1; };
  if (pos.size() >= 2 && pos[0] == "log") {
    if (pos[1] == "add" && pos.size() == 3) {
      LogEntry e; Status s = log.append("note", "", pos[2], "", &e);
      if (!s) return err(s);
      *out += o.json ? "{\"seq\":" + std::to_string(e.seq) + ",\"hash\":\"" + e.hash + "\"}\n" : "logged #" + std::to_string(e.seq) + " (" + e.time + ")\n";
      return 0;
    }
    if (pos[1] == "list" && pos.size() == 2) {
      std::vector<LogEntry> v; Status s = log.read(&v);
      if (!s) return err(s);
      long long lim = std::atoll(o.limit.c_str()); if (lim <= 0) lim = 20;
      size_t from = v.size() > static_cast<size_t>(lim) ? v.size() - static_cast<size_t>(lim) : 0;
      if (o.json) {
        *out += "[";
        for (size_t k = from; k < v.size(); ++k) *out += (k > from ? "," : "") + entry_body(v[k]) + kHashKey + v[k].hash + "\"}";
        *out += "]\n";
      } else if (v.empty()) *out += "research log is empty\n";
      else for (size_t k = from; k < v.size(); ++k) {
        *out += "#" + std::to_string(v[k].seq) + " " + v[k].time + " " + v[k].kind + (v[k].subject.empty() ? "" : " " + v[k].subject) + "\n    " + v[k].text + "\n";
        if (!v[k].sha256.empty()) *out += "    sha256 " + v[k].sha256 + "\n";
      }
      return 0;
    }
    if (pos[1] == "verify" && pos.size() == 2) {
      size_t n = 0; std::string d; Status s = log.verify(&n, &d);
      if (!s) { *out += "research log BROKEN: " + d + "\n"; return 1; }
      *out += "research log OK: " + d + "\n"; return 0;
    }
  }
  if (pos.size() == 3 && pos[0] == "fetch") {
    FetchOptions f = fo ? *fo : FetchOptions{};
    f.rights = o.rights; f.expect_sha256 = o.expect_sha256; f.force = o.force;
    std::unique_ptr<psn::HttpClient> owned; psn::HttpClient* http = client;
    if (!http) {
      Status cs; psn::CurlOptions co; co.timeout_seconds = 60; owned = psn::make_curl_client(co, &cs);
      if (!owned) { *out += "research: " + cs.error.message + "\n"; return 1; }
      http = owned.get();
    }
    FetchResult r; Status s = fetch(*http, log, pos[1], pos[2], f, &r);
    if (!s) return err(s);
    if (o.json) *out += "{\"path\":\"" + esc(r.path) + "\",\"bytes\":" + std::to_string(r.bytes) + ",\"sha256\":\"" + r.sha256 + "\",\"hash_verified\":" + (r.hash_verified ? "true" : "false") + ",\"resumed\":" + (r.resumed ? "true" : "false") + "}\n";
    else {
      *out += "fetched " + std::to_string(r.bytes) + " bytes to " + r.path + (r.resumed ? " (resumed)" : "") + "\nsha256: " + r.sha256 + "\n";
      *out += r.hash_verified ? "integrity: matched the sha256 you supplied\n" : "integrity: UNVERIFIED (no --sha256 given); the hash above is only what arrived\n";
      *out += "logged in the research log\n";
    }
    return 0;
  }
  if (pos.size() == 2 && pos[0] == "inspect") {
    Inspection in; Status s = inspect_file(pos[1], &in);
    if (!s) return err(s);
    *out += render_inspection(in, o.json) + (o.json ? "\n" : "");
    if (o.log_it) {
      Status ls = log.append("inspect", in.path.substr(0, 2048), "kind: " + in.kind + "; size " + std::to_string(in.size), in.sha256);
      if (!ls) return err(ls);
      if (!o.json) *out += "logged in the research log\n";
    }
    return 0;
  }
  *out += "usage: research log add <text> | log list [--limit N] | log verify | fetch <https-url> <out-file> --rights <statement> [--sha256 HEX] [--force] | inspect <file> [--log]\n";
  return 64;
}

}  // namespace pnr::research
