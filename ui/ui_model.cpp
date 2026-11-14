#include "ui/ui_model.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "launcher/launch_guard.hpp"

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
UiModel::~UiModel() { wait_idle(); core_.close(); }

bool UiModel::open(std::string* error) {
  auto st = core_.open(db_path_);
  if (!st) { if (error) *error = st.error.message; return false; }
  opened_ = true;
  refresh();
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

}  // namespace pnr::ui
