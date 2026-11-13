#include "store/download.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <thread>

#include "package/sha256.hpp"
#include "psn/json.hpp"
#include "psn/psn.hpp"

namespace fs = std::filesystem;
namespace pnr::store {
namespace {
std::string esc(const std::string& s) {
  std::string o;
  for (unsigned char c : s) {
    if (c == '"') o += "\\\""; else if (c == '\\') o += "\\\\";
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += static_cast<char>(c);
  }
  return o;
}
Status fail(int code, const std::string& m) { return Status::Fail(code, "store", m); }
long long file_bytes(const std::string& p) {
  std::error_code ec;
  if (!fs::is_regular_file(p, ec) || fs::is_symlink(p, ec)) return -1;
  return static_cast<long long>(fs::file_size(p, ec));
}
void rm(const std::string& p) { std::error_code ec; fs::remove(p, ec); }
}  // namespace

Status hash_file(const std::string& path, std::string* hex, long long* bytes) {
  if (file_bytes(path) < 0) return fail(kErrDisk, "file missing");
  std::ifstream f(path, std::ios::binary);
  if (!f) return fail(kErrDisk, "cannot read file");
  pkg::Sha256 h; std::vector<char> buf(1 << 20); long long n = 0;
  while (f) { f.read(buf.data(), static_cast<std::streamsize>(buf.size())); const auto g = f.gcount(); if (g > 0) { h.update(buf.data(), static_cast<size_t>(g)); n += g; } }
  *hex = h.finish_hex(); *bytes = n;
  return Status::Ok();
}

Status Queue::find_pkg(const std::string& s, const std::string& p, Package* out) {
  Index idx;
  Status l = reg_.load_index(s, &idx);
  if (!l) return l;
  for (auto& k : idx.packages) if (k.id == p) { *out = k; return Status::Ok(); }
  return fail(kErrNoSource, "no such package in that source");
}

Status Queue::load_state(const std::string& s, const std::string& p, DlState* st, bool* exists) {
  *st = DlState{}; st->source = s; st->pkg = p; *exists = false;
  std::string t;
  const std::string path = state_path(s, p);
  if (file_bytes(path) < 0) return Status::Ok();
  if (file_bytes(path) > (1 << 16)) return fail(kErrDisk, "damaged state file (remove with 'store queue cancel')");
  std::ifstream f(path, std::ios::binary); t.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  psn::Json j; std::string e;
  if (!psn::json_parse(t, &j, &e) || !j.is(psn::Json::T::Obj)) return fail(kErrDisk, "damaged state file (remove with 'store queue cancel')");
  auto sv = [&](const char* k) { const psn::Json* v = j.get(k); return (v && v->is(psn::Json::T::Str) && v->s.size() < 4096) ? v->s : std::string(); };
  st->status = sv("status"); st->sha256 = sv("sha256"); st->etag = sv("etag"); st->error = sv("error");
  static const char* ok[] = {"queued", "paused", "partial", "verified", "failed", "installed"};
  if (std::find(std::begin(ok), std::end(ok), st->status) == std::end(ok)) return fail(kErrDisk, "damaged state file (remove with 'store queue cancel')");
  *exists = true;
  return Status::Ok();
}

Status Queue::save_state(const DlState& st) {
  const std::string path = state_path(st.source, st.pkg);
  std::error_code ec; fs::create_directories(fs::path(path).parent_path(), ec);
  const std::string tmp = path + ".tmp"; ::unlink(tmp.c_str());
  const std::string d = "{\"status\":\"" + esc(st.status) + "\",\"sha256\":\"" + esc(st.sha256) + "\",\"etag\":\"" + esc(st.etag) + "\",\"error\":\"" + esc(st.error) + "\"}\n";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  if (fd < 0) return fail(kErrIo, "cannot write the store directory");
  bool ok = ::write(fd, d.data(), d.size()) == static_cast<ssize_t>(d.size()) && ::fsync(fd) == 0;
  ::close(fd);
  if (!ok || ::rename(tmp.c_str(), path.c_str()) != 0) { ::unlink(tmp.c_str()); return fail(kErrIo, "cannot write the store directory"); }
  return Status::Ok();
}

static void fill_have(const Queue& q, DlState* st) {
  long long f = file_bytes(q.final_path(st->source, st->pkg)), p = file_bytes(q.part_path(st->source, st->pkg));
  st->have = f >= 0 ? f : (p >= 0 ? p : 0);
}

Status Queue::enqueue(const std::string& s, const std::string& p, DlState* out) {
  Package k; Status r = find_pkg(s, p, &k);
  if (!r) return r;
  if (label_for(k) == "unsupported") return fail(kErrUnsupportedKind, "that package's kind (" + k.kind + ") is not a PNR package: PNR cannot install it, so it is not downloaded");
  DlState st; bool ex = false;
  r = load_state(s, p, &st, &ex);
  if (!r) return r;
  if (ex && st.sha256 != k.sha256) {  // the source's index changed the hash: old bytes are not trusted
    rm(part_path(s, p)); rm(final_path(s, p)); ex = false; st = DlState{}; st.source = s; st.pkg = p;
  }
  if (!ex) { st.status = "queued"; st.sha256 = k.sha256; r = save_state(st); if (!r) return r; }
  st.size = k.size; fill_have(*this, &st);
  *out = st;
  return Status::Ok();
}

Status Queue::list(std::vector<DlState>* out) {
  out->clear();
  std::error_code ec;
  const fs::path root = fs::path(dir_) / "downloads";
  if (!fs::is_directory(root, ec)) return Status::Ok();
  for (auto& sd : fs::directory_iterator(root, ec)) {
    if (!sd.is_directory(ec) || !valid_source_id(sd.path().filename().string())) continue;
    for (auto& f : fs::directory_iterator(sd.path(), ec)) {
      if (f.path().extension() != ".state") continue;
      DlState st; bool ex = false;
      const std::string s = sd.path().filename().string(), p = f.path().stem().string();
      if (!load_state(s, p, &st, &ex) || !ex) continue;
      Package k; if (find_pkg(s, p, &k)) st.size = k.size;
      fill_have(*this, &st);
      out->push_back(st);
    }
  }
  std::sort(out->begin(), out->end(), [](const DlState& a, const DlState& b) { return a.source + "/" + a.pkg < b.source + "/" + b.pkg; });
  return Status::Ok();
}

Status Queue::set_paused(const std::string& s, const std::string& p, bool paused) {
  DlState st; bool ex = false;
  Status r = load_state(s, p, &st, &ex);
  if (!r) return r;
  if (!ex) return fail(kErrNoSource, "not in the queue");
  if (st.status == "verified" || st.status == "installed") return fail(kErrDownload, "already complete");
  st.status = paused ? "paused" : (file_bytes(part_path(s, p)) > 0 ? "partial" : "queued");
  return save_state(st);
}

Status Queue::cancel(const std::string& s, const std::string& p) {
  DlState st; bool ex = false;
  Status r = load_state(s, p, &st, &ex);
  if (!r && file_bytes(state_path(s, p)) < 0) return r;
  if (!ex && file_bytes(part_path(s, p)) < 0 && file_bytes(final_path(s, p)) < 0) return fail(kErrNoSource, "not in the queue");
  rm(part_path(s, p)); rm(state_path(s, p));
  if (!ex || st.status != "installed") rm(final_path(s, p));  // an installed game's copy is only a cache; kept if installed
  return Status::Ok();
}

Status Queue::run(psn::HttpClient& raw, const std::string& s, const std::string& p, const DownloadOptions& opt, DlState* out) {
  Package k; Status r = find_pkg(s, p, &k);
  if (!r) return r;
  if (label_for(k) == "unsupported") return fail(kErrUnsupportedKind, "not a PNR package: not downloaded");
  const Source* src = reg_.find(s);
  DlState st; bool ex = false;
  r = load_state(s, p, &st, &ex);
  if (!r) return r;
  if (!ex || st.sha256 != k.sha256) { r = enqueue(s, p, &st); if (!r) return r; }
  if (st.status == "paused") return fail(kErrDownload, "this download is paused: resume it first");
  if (st.status == "installed") { st.size = k.size; fill_have(*this, &st); *out = st; return Status::Ok(); }
  const std::string part = part_path(s, p), fin = final_path(s, p);
  std::error_code ec; fs::create_directories(fs::path(part).parent_path(), ec);
  psn::GuardedHttp http(raw, {src->host});
  auto finish = [&](const std::string& status, const std::string& err) { st.status = status; st.error = err; st.size = k.size; fill_have(*this, &st); save_state(st); *out = st; };

  if (st.status == "verified" && file_bytes(fin) == k.size) { finish("verified", ""); return Status::Ok(); }
  rm(fin);
  int retries = 0, delay = opt.backoff_ms;
  std::string last = "download did not complete";
  while (true) {
    long long have = file_bytes(part);
    if (have < 0) have = 0;
    if (have > k.size) { rm(part); have = 0; }
    if (have == k.size) break;
    if (opt.cancel && opt.cancel()) { finish("partial", "interrupted"); return fail(kErrInterrupted, "stopped; the partial file is kept, run again to resume"); }
    std::error_code sp_ec;
    const auto space = fs::space(fs::path(part).parent_path(), sp_ec);
    if (sp_ec || static_cast<long long>(space.available) < (k.size - have) + opt.free_margin) {
      finish("partial", "not enough free disk space"); return fail(kErrDisk, "not enough free disk space for the remaining " + std::to_string(k.size - have) + " bytes plus a safety margin");
    }
    psn::HttpRequest rq; rq.url = k.url;
    if (have > 0) { rq.headers.push_back("Range: bytes=" + std::to_string(have) + "-"); if (!st.etag.empty()) rq.headers.push_back("If-Range: " + st.etag); }
    std::ofstream out_f; bool opened = false, bad_head = false; long long offset = have, written = 0; std::string head_err;
    psn::HttpResponse rs;
    auto sink = [&](const psn::HttpResponse& hd, const char* d, size_t n) -> bool {
      if (!opened) {
        if (hd.status == 206) {
          const std::string want = "bytes " + std::to_string(have) + "-" + std::to_string(k.size - 1) + "/" + std::to_string(k.size);
          if (have == 0 || hd.content_range != want) { bad_head = true; head_err = "server sent an unexpected byte range"; return false; }
          out_f.open(part, std::ios::binary | std::ios::app);
        } else {  // 200: the server sent the whole body (no Range support, or If-Range failed)
          if (hd.content_length >= 0 && hd.content_length != k.size) { bad_head = true; head_err = "server's file size differs from the index"; return false; }
          offset = 0; out_f.open(part, std::ios::binary | std::ios::trunc);
          st.etag = hd.etag;
        }
        if (!out_f) { bad_head = true; head_err = "cannot write the partial file"; return false; }
        opened = true;
      }
      if (offset + written + static_cast<long long>(n) > k.size) { bad_head = true; head_err = "server sent more bytes than the index declares"; return false; }
      out_f.write(d, static_cast<std::streamsize>(n)); written += static_cast<long long>(n);
      if (!out_f) { bad_head = true; head_err = "disk write failed"; return false; }
      if (opt.progress) opt.progress(offset + written, k.size);
      if (opt.cancel && opt.cancel()) return false;
      return true;
    };
    Status t = http.download(rq, sink, &rs);
    if (opened) { out_f.flush(); out_f.close(); }
    if (opt.cancel && opt.cancel() && !bad_head) { finish("partial", "interrupted"); return fail(kErrInterrupted, "stopped; the partial file is kept, run again to resume"); }
    if (bad_head) {
      if (head_err.find("disk") != std::string::npos || head_err.find("write") != std::string::npos) { finish("partial", head_err); return fail(kErrDisk, head_err); }
      finish("failed", head_err); rm(part); return fail(kErrDownload, head_err);
    }
    bool retry = false;
    if (!t) { last = t.error.message; retry = t.error.code != psn::kErrBlockedHost; if (!retry) { finish("failed", last); return t; } }
    else if (rs.status == 416) { rm(part); last = "server rejected the range; restarting"; retry = true; }
    else if (rs.status == 429 || (rs.status >= 500 && rs.status < 600)) { last = "server answered HTTP " + std::to_string(rs.status); retry = true; }
    else if (rs.status != 200 && rs.status != 206) {
      last = rs.status >= 300 && rs.status < 400 ? "the server redirected (redirects are not followed)" : "server answered HTTP " + std::to_string(rs.status);
      finish("failed", last); return fail(kErrDownload, last);
    } else if (file_bytes(part) != k.size) { last = "transfer ended early"; retry = true; }
    else break;
    if (!retry) break;
    if (++retries > opt.max_retries) { finish("partial", last); return fail(kErrDownload, "gave up after " + std::to_string(opt.max_retries) + " retries: " + last + " (the partial file is kept; run again to resume)"); }
    save_state(st);
    if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    delay *= 2;
  }
  // size is right; now the hash decides
  std::string hex; long long n = 0;
  r = hash_file(part, &hex, &n);
  if (!r) { finish("failed", "cannot read the downloaded file"); return r; }
  if (n != k.size || hex != k.sha256) {
    rm(part);
    finish("failed", "sha256 does not match the source's index; the file was deleted");
    return fail(kErrHashMismatch, "downloaded file does not match the index (expected sha256 " + k.sha256.substr(0, 12) + "..., got " + hex.substr(0, 12) + "...); it was deleted and nothing was installed");
  }
  if (::rename(part.c_str(), fin.c_str()) != 0) { finish("failed", "cannot finalise the file"); return fail(kErrDisk, "cannot finalise the file"); }
  finish("verified", "");
  return Status::Ok();
}

Status Queue::prepare_install(const std::string& s, const std::string& p, std::string* path, std::string* sha) {
  Package k; Status r = find_pkg(s, p, &k);
  if (!r) return r;
  if (label_for(k) == "unsupported") return fail(kErrUnsupportedKind, "not a PNR package: cannot be installed");
  { const Source* so = reg_.find(s); std::string mode; if (!so) return fail(kErrNoSource, "no such source"); Status sg = check_signature(*so, k, &mode); if (!sg) return sg; }
  DlState st; bool ex = false;
  r = load_state(s, p, &st, &ex);
  if (!r) return r;
  if (!ex || (st.status != "verified" && st.status != "installed") || st.sha256 != k.sha256) return fail(kErrNotVerified, "no verified download of that package: run 'store get' and 'store queue run' first");
  std::string hex; long long n = 0;
  r = hash_file(final_path(s, p), &hex, &n);
  if (!r) return fail(kErrNotVerified, "the verified file is missing: download it again");
  if (n != k.size || hex != k.sha256) return fail(kErrNotVerified, "the file on disk no longer matches the index hash (changed after download?): refusing to install it");
  *path = final_path(s, p); *sha = hex;
  return Status::Ok();
}

Status Queue::mark_installed(const std::string& s, const std::string& p, const std::string& game_id) {
  DlState st; bool ex = false;
  Status r = load_state(s, p, &st, &ex);
  if (!r || !ex) return r;
  st.status = "installed"; st.error = "game id: " + game_id;
  Status sv = save_state(st);
  if (!sv) return sv;
  // provenance record for the launch-time re-check
  Package k; const Source* so = reg_.find(s);
  if (!find_pkg(s, p, &k) || !so) return Status::Ok();
  std::error_code ec; fs::create_directories(dir_ + "/provenance", ec);
  std::ofstream f(dir_ + "/provenance/" + game_id + ".prov", std::ios::trunc);
  f << "source=" << s << "\npkg=" << p << "\nversion=" << k.version << "\nsha256=" << k.sha256 << "\nsize=" << k.size
    << "\nsignature=" << k.signature << "\npubkey=" << so->pubkey << "\n";
  return f ? Status::Ok() : Status::Fail(kErrIo, "store", "could not write the provenance record");
}

Status check_provenance(const std::string& store_dir, Registry& reg, const std::string& game_id, std::string* note) {
  if (game_id.empty() || game_id.find_first_of("/\\") != std::string::npos || game_id.find("..") != std::string::npos) { *note = ""; return Status::Ok(); }
  std::ifstream f(store_dir + "/provenance/" + game_id + ".prov");
  if (!f) { *note = "not installed from a store source: no signature check applies"; return Status::Ok(); }
  std::map<std::string, std::string> kv; std::string line;
  while (std::getline(f, line)) { auto e = line.find('='); if (e != std::string::npos) kv[line.substr(0, e)] = line.substr(e + 1); }
  auto bad = [&](const std::string& why) { return Status::Fail(kErrSignature, "store", "refusing to launch: " + why); };
  if (kv["source"].empty() || kv["pkg"].empty()) return bad("provenance record is damaged");
  const Source* so = reg.find(kv["source"]);
  const std::string& pk = kv["pubkey"];
  if (pk.empty()) {
    if (so && !so->pubkey.empty()) return bad("source '" + so->id + "' now pins a key but this game was installed unsigned: reinstall it from the source");
    *note = "installed from a source without a pinned key: hash only, no signature";
    return Status::Ok();
  }
  if (!so) return bad("the source it was installed from was removed: its pinned key is no longer trusted");
  if (so->pubkey != pk) return bad("the source's pinned key changed since install: reinstall from the source");
  Package k; k.id = kv["pkg"]; k.version = kv["version"]; k.sha256 = kv["sha256"]; k.size = std::atoll(kv["size"].c_str()); k.signature = kv["signature"];
  std::string mode; Status sg = check_signature(*so, k, &mode);
  if (!sg) return bad(sg.error.message);
  // the retained package file must still match the signed hash
  std::string hex; long long n = 0;
  Status h = hash_file(store_dir + "/downloads/" + kv["source"] + "/" + k.id + ".pnrgame", &hex, &n);
  if (!h) return bad("the retained package file is missing, so the signed hash cannot be re-checked");
  if (hex != k.sha256 || n != k.size) return bad("the retained package file no longer matches the signed hash");
  *note = "signature verified again under the pinned key; retained package hash matches";
  return Status::Ok();
}

}  // namespace pnr::store
