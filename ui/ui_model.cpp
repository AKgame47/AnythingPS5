#include "ui/ui_model.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "launcher/launch_guard.hpp"
#include "plugin/host.hpp"
#include "scan/scan.hpp"
#include "store/store_cli.hpp"

namespace pnr::ui {
namespace {
std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}
// Redirects fds 0/1/2 for the duration of a launch so the game's output can be
// shown in the window. Process-global: the UI prints nothing to stdout/stderr
// while a launch runs.
class FdCapture {
public:
  FdCapture() {
    char tmpl[] = "/tmp/pnr_ui_launch_XXXXXX";
    fd_ = ::mkstemp(tmpl);
    if (fd_ < 0) return;
    path_ = tmpl;
    ::unlink(tmpl);  // the open fd keeps the file; nothing is left behind
    std::fflush(stdout);
    std::fflush(stderr);
    saved_[0] = ::dup(0);
    saved_[1] = ::dup(1);
    saved_[2] = ::dup(2);
    int nul = ::open("/dev/null", O_RDONLY);
    if (nul >= 0) { ::dup2(nul, 0); ::close(nul); }
    ::dup2(fd_, 1);
    ::dup2(fd_, 2);
    active_ = true;
  }
  std::string finish() {
    if (!active_) return "";
    std::fflush(stdout);
    std::fflush(stderr);
    for (int i = 0; i < 3; ++i) { if (saved_[i] >= 0) { ::dup2(saved_[i], i); ::close(saved_[i]); } }
    active_ = false;
    off_t end = ::lseek(fd_, 0, SEEK_END);
    off_t start = end > 8192 ? end - 8192 : 0;
    std::string out(static_cast<size_t>(end - start), '\0');
    ssize_t n = ::pread(fd_, out.data(), out.size(), start);
    out.resize(n > 0 ? static_cast<size_t>(n) : 0);
    ::close(fd_);
    fd_ = -1;
    return out;
  }
  ~FdCapture() { if (active_) finish(); if (fd_ >= 0) ::close(fd_); }
private:
  int fd_ = -1;
  int saved_[3] = {-1, -1, -1};
  bool active_ = false;
  std::string path_;
};
}  // namespace

UiModel::UiModel(std::string db_path) : db_path_(std::move(db_path)) {}
UiModel::~UiModel() { wait_idle(); if (sth_.joinable()) sth_.join(); core_.close(); }

bool UiModel::open(std::string* error) {
  auto st = core_.open(db_path_);
  if (!st) { if (error) *error = st.error.message; return false; }
  opened_ = true;
  refresh();
  store_reload();
  return true;
}

void UiModel::refresh() {
  if (!opened_ || launching()) return;
  games_ = core_.list();
}

std::vector<size_t> UiModel::filter(const std::string& query) const {
  std::vector<size_t> r;
  const std::string q = lower(query);
  for (size_t i = 0; i < games_.size(); ++i) {
    const auto& g = games_[i];
    if (q.empty() || lower(g.title).find(q) != std::string::npos || lower(g.game_id).find(q) != std::string::npos ||
        lower(g.version).find(q) != std::string::npos || lower(g.compatibility).find(q) != std::string::npos)
      r.push_back(i);
  }
  return r;
}

std::string UiModel::launch_blocker(const launcher::GameEntry& g) const {
  if (launching()) return "another launch is running";
  if (!g.installed) return "not installed";
  if (!g.runnable) return g.not_runnable_reason.empty() ? "not runnable" : g.not_runnable_reason;
  return "";
}

bool UiModel::launching() const { std::lock_guard<std::mutex> l(mu_); return last_.running; }

LaunchResult UiModel::last_launch() const { std::lock_guard<std::mutex> l(mu_); return last_; }

bool UiModel::start_launch(const std::string& game_id, bool sandbox, std::string* why_not) {
  if (!opened_) { if (why_not) *why_not = "library is not open"; return false; }
  {
    std::lock_guard<std::mutex> l(mu_);
    if (last_.running) { if (why_not) *why_not = "another launch is running"; return false; }
  }
  launcher::GameEntry g;
  if (!core_.get(game_id, &g)) { if (why_not) *why_not = "no game with id '" + game_id + "'"; return false; }
  std::string b = launch_blocker(g);
  if (!b.empty()) { if (why_not) *why_not = b; return false; }
  if (th_.joinable()) th_.join();
  {
    std::lock_guard<std::mutex> l(mu_);
    last_ = LaunchResult{};
    last_.game_id = game_id;
    last_.sandbox = sandbox;
    last_.running = true;
    last_.summary = "running...";
    worker_done_ = false;
  }
  th_ = std::thread([this, game_id, sandbox] { worker(game_id, sandbox); });
  return true;
}

void UiModel::worker(std::string game_id, bool sandbox) {
  LaunchResult r;
  r.game_id = game_id;
  r.sandbox = sandbox;
  launcher::LauncherCore core;  // its own SQLite connection: the UI thread never shares one
  auto st = core.open(db_path_);
  if (!st) {
    r.finished = true;
    r.summary = "LAUNCH NOT STARTED: library open failed: " + st.error.message;
  } else {
    std::string refuse = launcher::prepare_launch(core, db_path_, game_id, sandbox);
    if (!refuse.empty()) {
      r.finished = true;
      r.summary = refuse;
    } else {
      FdCapture cap;
      auto o = core.launch(game_id);
      r.output = cap.finish();
      r.finished = true;
      r.started = o.attempted;
      r.seconds = o.wall_seconds;
      r.compat_advanced = o.compat_advanced;
      r.clean = o.error.empty() && o.attempted;
      char b[160];
      if (!o.error.empty()) r.summary = std::string("LAUNCH ") + (o.attempted ? "RAN, NOT CLEAN" : "NOT STARTED") + ": " + o.error;
      else {
        std::snprintf(b, sizeof b, "LAUNCH OK: exit 0 in %.3fs%s", o.wall_seconds, o.compat_advanced ? " (compatibility Untested -> Launches Only)" : "");
        r.summary = b;
      }
    }
    core.close();
  }
  std::lock_guard<std::mutex> l(mu_);
  r.running = false;
  last_ = r;
  worker_done_ = true;
}

void UiModel::poll() {
  bool done = false;
  {
    std::lock_guard<std::mutex> l(mu_);
    done = worker_done_;
    if (done) worker_done_ = false;
  }
  if (done) {
    if (th_.joinable()) th_.join();
    refresh();
  }
}

void UiModel::wait_idle() {
  if (sth_.joinable()) sth_.join();
  if (th_.joinable()) th_.join();
}

bool UiModel::game(const std::string& id, launcher::GameEntry* out) { return opened_ && core_.get(id, out); }

std::vector<library::LaunchRow> UiModel::history(const std::string& id, int limit) {
  std::vector<library::LaunchRow> rows;
  if (opened_) core_.history(id, limit, &rows);
  return rows;
}

Status UiModel::set_compat(const std::string& id, const std::string& state, const std::string& note) {
  if (!opened_) return Status::Fail(2, "ui", "library is not open");
  if (launching()) return Status::Fail(2, "ui", "a launch is running; try again when it finishes");
  auto st = core_.set_compat(id, state, note);
  if (st.ok) refresh();
  return st;
}

launcher::VerifyOutcome UiModel::verify(const std::string& id) {
  launcher::VerifyOutcome o;
  if (!opened_ || launching()) { o.problems.push_back(opened_ ? "a launch is running; try again when it finishes" : "library is not open"); return o; }
  o = core_.verify(id);
  refresh();
  return o;
}

launcher::SavesOutcome UiModel::export_saves(const std::string& id, const std::string& out_path) {
  if (!opened_) { launcher::SavesOutcome o; o.error = "library is not open"; return o; }
  return core_.export_saves(id, out_path);
}

launcher::RemoveOutcome UiModel::remove(const std::string& id, bool keep_saves, bool confirmed) {
  if (!opened_ || launching()) {
    launcher::RemoveOutcome o;
    o.error = opened_ ? "a launch is running; try again when it finishes" : "library is not open";
    return o;
  }
  auto o = core_.remove(id, keep_saves, confirmed);
  if (o.removed) refresh();
  return o;
}

Status UiModel::scan(const std::string& dir, launcher::ScanReport* out) { return core_.scan(dir, out); }

launcher::ImportOutcome UiModel::import_package(const std::string& path) {
  auto o = core_.import_package(path);
  if (o.ok) refresh();
  return o;
}

std::string UiModel::store_dir() const { return store::store_dir_for(db_path_); }

void UiModel::store_reload() {
  store::Registry reg(store_dir());
  sources_.clear();
  if (reg.load().ok) sources_ = reg.sources();
}

std::string UiModel::store_add_source(const std::string& id, const std::string& url, const std::string& rights, bool* ok) {
  store::Registry reg(store_dir());
  std::string out;
  auto ls = reg.load();
  if (!ls.ok) { if (ok) *ok = false; return "store error: " + ls.error.message; }
  int rc = store::cmd_source_add(reg, id, url, rights, false, &out);
  if (ok) *ok = rc == 0;
  store_reload();
  return out;
}

std::string UiModel::store_remove_source(const std::string& id, bool confirmed, bool* ok) {
  store::Registry reg(store_dir());
  std::string out;
  auto ls = reg.load();
  if (!ls.ok) { if (ok) *ok = false; return "store error: " + ls.error.message; }
  int rc = store::cmd_source_remove(reg, id, confirmed, false, &out);
  if (ok) *ok = rc == 0;
  store_reload();
  return out;
}

std::vector<StoreRow> UiModel::store_rows(const std::string& source, const std::string& query) {
  std::vector<StoreRow> rows;
  store::Registry reg(store_dir());
  if (!reg.load().ok) return rows;
  store::Queue q(reg, reg.dir());
  std::vector<store::DlState> dl;
  q.list(&dl);
  const std::string qq = lower(query);
  for (const auto& s : reg.sources()) {
    if (!source.empty() && s.id != source) continue;
    store::Index ix;
    if (!reg.load_index(s.id, &ix).ok) continue;
    for (const auto& p : ix.packages) {
      if (!qq.empty() && lower(p.name).find(qq) == std::string::npos && lower(p.id).find(qq) == std::string::npos &&
          lower(p.description).find(qq) == std::string::npos && lower(p.category).find(qq) == std::string::npos) continue;
      StoreRow r;
      r.source = s.id; r.id = p.id; r.name = p.name; r.version = p.version; r.description = p.description;
      r.license = p.license; r.rights = p.rights.empty() ? ix.rights : p.rights; r.category = p.category; r.kind = p.kind;
      r.label = store::label_for(p); r.size = p.size;
      for (const auto& d : dl) if (d.source == s.id && d.pkg == p.id) { r.status = d.status; r.have = d.have; r.error = d.error; }
      rows.push_back(r);
    }
  }
  return rows;
}

StoreJob UiModel::store_job() const { std::lock_guard<std::mutex> l(smu_); return sjob_; }

bool UiModel::store_start_refresh(const std::string& source, psn::HttpClient* http) {
  { std::lock_guard<std::mutex> l(smu_); if (sjob_.running) return false; }
  if (sth_.joinable()) sth_.join();
  {
    std::lock_guard<std::mutex> l(smu_);
    sjob_ = StoreJob{}; sjob_.running = true; sjob_.what = "refresh " + (source.empty() ? std::string("all sources") : source);
    sjob_.summary = "contacting the source...";
    sworker_done_ = false;
  }
  const std::string dir = store_dir();
  sth_ = std::thread([this, dir, source, http] {
    store::Registry reg(dir);
    std::string out; int rc = 1;
    if (reg.load().ok) rc = store::cmd_refresh(reg, source, false, &out, http);
    std::lock_guard<std::mutex> l(smu_);
    sjob_.running = false; sjob_.finished = true; sjob_.ok = rc == 0; sjob_.summary = out; sworker_done_ = true;
  });
  return true;
}

bool UiModel::store_start_get(const std::string& source, const std::string& pkg, psn::HttpClient* http) {
  { std::lock_guard<std::mutex> l(smu_); if (sjob_.running) return false; }
  if (sth_.joinable()) sth_.join();
  {
    std::lock_guard<std::mutex> l(smu_);
    sjob_ = StoreJob{}; sjob_.running = true; sjob_.what = "download " + source + "/" + pkg;
    sjob_.summary = "downloading...";
    sworker_done_ = false;
  }
  const std::string dir = store_dir();
  sth_ = std::thread([this, dir, source, pkg, http] {
    store::Registry reg(dir);
    std::string out; int rc = 1;
    if (reg.load().ok) {
      store::Queue q(reg, reg.dir());
      std::string o1;
      rc = store::cmd_get(q, source, pkg, false, &o1);
      out = o1;
      if (rc == 0) { std::string o2; rc = store::cmd_queue_run(q, source, pkg, false, &o2, http); out += o2; }
    }
    std::lock_guard<std::mutex> l(smu_);
    sjob_.running = false; sjob_.finished = true; sjob_.ok = rc == 0; sjob_.summary = out; sworker_done_ = true;
  });
  return true;
}

void UiModel::store_poll() {
  bool done;
  { std::lock_guard<std::mutex> l(smu_); done = sworker_done_; sworker_done_ = false; }
  if (done) { if (sth_.joinable()) sth_.join(); store_reload(); }
}

std::string UiModel::store_install(const std::string& source, const std::string& pkg, bool* ok, std::string* scan_text) {
  if (ok) *ok = false;
  if (!opened_ || launching()) return "a launch is running; try again when it finishes";
  store::Registry reg(store_dir());
  auto ls = reg.load();
  if (!ls.ok) return "store error: " + ls.error.message;
  store::Queue q(reg, reg.dir());
  std::string path, sha;
  auto ps = q.prepare_install(source, pkg, &path, &sha);
  if (!ps.ok) return "store error: " + ps.error.message;
  store::Package spk;
  { store::Index sx; if (reg.load_index(source, &sx).ok) for (auto& k : sx.packages) if (k.id == pkg) spk = k; }
  auto rep = scan::scan_package(path, sha, spk.size, scan::config_from_env());
  if (scan_text) *scan_text = rep.text();
  if (rep.flagged()) return "Install refused: a scan stage flagged this package. A clean scan never proves safety either.";
  auto o = core_.import_package(path);
  if (!o.ok) return "Import failed: " + o.error;
  q.mark_installed(source, pkg, o.game_id);
  refresh();
  if (ok) *ok = true;
  return "Installed " + o.title + " " + o.version + " from source '" + source + "' (sha256 checked against the source's index; the importer then ran its own checks). New games start as Untested.";
}

std::string UiModel::plugin_home() const {
  if (const char* e = std::getenv("PNR_PLUGIN_HOME")) if (*e) return e;
  auto pos = db_path_.rfind('/');
  return pos == std::string::npos ? std::string(".") : db_path_.substr(0, pos);
}

std::vector<plugin::InstalledPlugin> UiModel::plugin_list() const {
  plugin::PluginManager pm(plugin_home());
  return pm.list();
}

plugin::InstallResult UiModel::plugin_install(const std::string& dir) {
  plugin::PluginManager pm(plugin_home());
  pm.require_signed = plugin_require_signed;
  return pm.install_from_dir(dir);
}

std::string UiModel::plugin_enable(const std::string& id, const std::vector<std::string>& grant, bool* ok) {
  plugin::PluginManager pm(plugin_home());
  auto st = pm.enable(id, grant);
  if (ok) *ok = st.ok;
  return st.ok ? "Enabled " + id + " with " + std::to_string(grant.size()) + " granted permission(s)." : "Not enabled: " + st.error.message;
}

std::string UiModel::plugin_disable(const std::string& id, bool* ok) {
  plugin::PluginManager pm(plugin_home());
  auto st = pm.disable(id);
  if (ok) *ok = st.ok;
  return st.ok ? "Disabled " + id + "." : "Not disabled: " + st.error.message;
}

std::string UiModel::plugin_remove(const std::string& id, bool* ok) {
  plugin::PluginManager pm(plugin_home());
  auto st = pm.remove(id);
  if (ok) *ok = st.ok;
  return st.ok ? "Removed " + id + "." : "Not removed: " + st.error.message;
}

std::vector<plugin::PinnedKey> UiModel::plugin_keys() const {
  plugin::PluginManager pm(plugin_home());
  return pm.pinned_keys();
}

std::string UiModel::plugin_pin(const std::string& name, const std::string& hex, bool* ok) {
  plugin::PluginManager pm(plugin_home());
  auto st = pm.pin_key(name, hex);
  if (ok) *ok = st.ok;
  return st.ok ? "Pinned key " + name + ". A valid signature proves the key holder made the plugin, not that it is safe." : "Not pinned: " + st.error.message;
}

std::string UiModel::plugin_unpin(const std::string& name, bool* ok) {
  plugin::PluginManager pm(plugin_home());
  auto st = pm.unpin_key(name);
  if (ok) *ok = st.ok;
  return st.ok ? "Unpinned " + name + "." : "Not unpinned: " + st.error.message;
}

plugin::RunResult UiModel::plugin_run(const std::string& id, std::string* refuse) {
  plugin::RunResult rr;
  plugin::PluginManager pm(plugin_home());
  plugin::InstalledPlugin p;
  if (!pm.get(id, &p)) { if (refuse) *refuse = "no installed plugin '" + id + "'"; return rr; }
  if (!p.enabled) { if (refuse) *refuse = "plugin is not enabled"; return rr; }
  if (p.manifest.entry_type != "process") { if (refuse) *refuse = "this is a data plugin: it has nothing to run"; return rr; }
  plugin::LiveBrokerHost host(db_path_, p.manifest.id);
  plugin::RunOptions ro; ro.host = &host;
  rr = plugin::run_plugin(p, pm.root(), ro);
  if (!rr.started && refuse) *refuse = rr.error;
  return rr;
}

}  // namespace pnr::ui
