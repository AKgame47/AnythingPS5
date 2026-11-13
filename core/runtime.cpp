// PNR Core Runtime - MODULE 01 implementation.
#include "core/runtime.hpp"

#include <fstream>
#include <sstream>

namespace pnr {

const char* to_string(FeatureStatus s) {
  switch (s) {
    case FeatureStatus::Planned: return "Planned";
    case FeatureStatus::Implemented: return "Implemented";
    case FeatureStatus::Tested: return "Tested";
    case FeatureStatus::Experimental: return "Experimental";
    case FeatureStatus::Blocked: return "Blocked";
  }
  return "Unknown";
}

const char* to_string(LifecycleState s) {
  switch (s) {
    case LifecycleState::Created: return "CREATED";
    case LifecycleState::Validating: return "VALIDATING";
    case LifecycleState::Initializing: return "INITIALIZING";
    case LifecycleState::Ready: return "READY";
    case LifecycleState::Running: return "RUNNING";
    case LifecycleState::Suspended: return "SUSPENDED";
    case LifecycleState::Stopping: return "STOPPING";
    case LifecycleState::Terminated: return "TERMINATED";
  }
  return "UNKNOWN";
}

// --- RuntimeEventBus -------------------------------------------------------
uint64_t RuntimeEventBus::subscribe(std::string event_type, Handler handler) {
  std::lock_guard<std::mutex> lock(mutex_);
  const uint64_t id = next_id_++;
  subs_.emplace(id, Sub{std::move(event_type), std::move(handler)});
  return id;
}

void RuntimeEventBus::unsubscribe(uint64_t subscription_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  subs_.erase(subscription_id);
}

void RuntimeEventBus::publish(Event e) {
  std::vector<Handler> handlers;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    e.sequence = next_sequence_++;
    for (const auto& [id, sub] : subs_) {
      if (sub.type == e.type || sub.type == "*") handlers.push_back(sub.handler);
    }
  }
  // Handlers run outside the lock so a handler may publish or unsubscribe.
  for (auto& h : handlers) h(e);
}

size_t RuntimeEventBus::subscriber_count(const std::string& event_type) const {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t n = 0;
  for (const auto& [id, sub] : subs_)
    if (sub.type == event_type || sub.type == "*") ++n;
  return n;
}

// --- SubsystemRegistry -----------------------------------------------------
Status SubsystemRegistry::add(std::shared_ptr<ISubsystem> subsystem) {
  if (!subsystem) return Status::Fail(1, "core", "null subsystem rejected");
  std::lock_guard<std::mutex> lock(mutex_);
  const std::string n = subsystem->name();
  if (by_name_.count(n))
    return Status::Fail(2, "core", "duplicate subsystem name: " + n);
  by_name_[n] = subsystem;
  order_.push_back(std::move(subsystem));
  return Status::Ok();
}

std::shared_ptr<ISubsystem> SubsystemRegistry::get(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = by_name_.find(name);
  return it == by_name_.end() ? nullptr : it->second;
}

std::vector<std::string> SubsystemRegistry::names() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> out;
  for (auto& s : order_) out.push_back(s->name());
  return out;
}

Status SubsystemRegistry::initialize_all() {
  std::vector<std::shared_ptr<ISubsystem>> snapshot;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot = order_;
  }
  for (auto& s : snapshot) {
    Status st = s->initialize();
    if (!st) return st;  // fail fast; caller decides on rollback
  }
  return Status::Ok();
}

Status SubsystemRegistry::shutdown_all() {
  std::vector<std::shared_ptr<ISubsystem>> snapshot;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot.assign(order_.rbegin(), order_.rend());  // reverse order
  }
  Status first_failure = Status::Ok();
  for (auto& s : snapshot) {
    Status st = s->shutdown();
    if (!st && first_failure) first_failure = st;
  }
  return first_failure;
}

// --- ConfigurationManager --------------------------------------------------
Status ConfigurationManager::load_from_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) return Status::Fail(3, "config", "cannot open config file: " + path);
  std::map<std::string, std::string> parsed;
  std::string line;
  int line_no = 0;
  while (std::getline(in, line)) {
    ++line_no;
    auto hash = line.find('#');
    if (hash != std::string::npos) line.erase(hash);
    auto eq = line.find('=');
    if (eq == std::string::npos) {
      // tolerate blank lines only
      if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
      std::ostringstream msg;
      msg << "config parse error at line " << line_no << ": " << line;
      return Status::Fail(4, "config", msg.str());
    }
    std::string key = line.substr(0, eq);
    std::string value = line.substr(eq + 1);
    auto trim = [](std::string& s) {
      const char* ws = " \t\r";
      s.erase(0, s.find_first_not_of(ws));
      s.erase(s.find_last_not_of(ws) + 1);
    };
    trim(key);
    trim(value);
    if (key.empty()) {
      std::ostringstream msg;
      msg << "empty config key at line " << line_no;
      return Status::Fail(5, "config", msg.str());
    }
    parsed[key] = value;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  values_.merge(parsed);
  return Status::Ok();
}

void ConfigurationManager::set(const std::string& key, const std::string& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  values_[key] = value;
}

std::optional<std::string> ConfigurationManager::get(const std::string& key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = values_.find(key);
  if (it == values_.end()) return std::nullopt;
  return it->second;
}

std::string ConfigurationManager::get_or(const std::string& key,
                                         const std::string& fallback) const {
  return get(key).value_or(fallback);
}

std::optional<int64_t> ConfigurationManager::get_int(const std::string& key) const {
  auto v = get(key);
  if (!v) return std::nullopt;
  try {
    size_t used = 0;
    int64_t out = std::stoll(*v, &used);
    if (used != v->size()) return std::nullopt;
    return out;
  } catch (...) {
    return std::nullopt;
  }
}

// --- CapabilityRegistry ----------------------------------------------------
void CapabilityRegistry::declare(const std::string& name, bool supported,
                                 std::string note) {
  std::lock_guard<std::mutex> lock(mutex_);
  caps_[name] = Capability{supported, std::move(note)};
}

std::optional<Capability> CapabilityRegistry::query(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = caps_.find(name);
  if (it == caps_.end()) return std::nullopt;
  return it->second;
}

std::vector<std::pair<std::string, Capability>> CapabilityRegistry::all() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return {caps_.begin(), caps_.end()};
}

// --- ErrorReporter ---------------------------------------------------------
void ErrorReporter::report(Error e) { errors_.push_back(std::move(e)); }

// --- LifecycleController ---------------------------------------------------
bool LifecycleController::is_legal(LifecycleState from, LifecycleState to) {
  using S = LifecycleState;
  switch (from) {
    case S::Created: return to == S::Validating || to == S::Stopping;
    case S::Validating: return to == S::Initializing || to == S::Stopping;
    case S::Initializing: return to == S::Ready || to == S::Stopping;
    case S::Ready: return to == S::Running || to == S::Stopping;
    case S::Running: return to == S::Suspended || to == S::Stopping;
    case S::Suspended: return to == S::Running || to == S::Stopping;
    case S::Stopping: return to == S::Terminated;
    case S::Terminated: return false;
  }
  return false;
}

Status LifecycleController::transition(LifecycleState target) {
  if (!is_legal(state_, target)) {
    return Status::Fail(6, "lifecycle",
                        std::string("illegal transition ") + to_string(state_) +
                            " -> " + to_string(target));
  }
  state_ = target;
  history_.push_back(target);
  return Status::Ok();
}

// --- RuntimeManager --------------------------------------------------------
RuntimeManager::RuntimeManager() {
  caps_.declare("core.lifecycle", true, "implemented, unit-tested");
  caps_.declare("core.event_bus", true, "implemented, unit-tested");
  caps_.declare("core.configuration", true, "implemented, unit-tested");
  caps_.declare("graphics.vulkan", false,
                "no Vulkan device in the CI/dev sandbox; Phase 2 work");
  caps_.declare("loader.protected_ps5_format", false,
                "proprietary formats are undocumented and out of scope until "
                "authorized interfaces exist");
}

Status RuntimeManager::initialize() {
  Status st = lifecycle_.transition(LifecycleState::Validating);
  if (!st) return st;
  bus_.publish({"lifecycle.changed", "VALIDATING", 0});
  st = lifecycle_.transition(LifecycleState::Initializing);
  if (!st) return st;
  bus_.publish({"lifecycle.changed", "INITIALIZING", 0});
  st = registry_.initialize_all();
  if (!st) {
    errors_.report(st.error);
    return st;
  }
  subsystems_ready_ = true;
  st = lifecycle_.transition(LifecycleState::Ready);
  if (!st) return st;
  bus_.publish({"lifecycle.changed", "READY", 0});
  return Status::Ok();
}

Status RuntimeManager::mark_running() {
  Status st = lifecycle_.transition(LifecycleState::Running);
  if (st) bus_.publish({"lifecycle.changed", "RUNNING", 0});
  return st;
}

Status RuntimeManager::suspend() {
  Status st = lifecycle_.transition(LifecycleState::Suspended);
  if (st) bus_.publish({"lifecycle.changed", "SUSPENDED", 0});
  return st;
}

Status RuntimeManager::resume() {
  Status st = lifecycle_.transition(LifecycleState::Running);
  if (st) bus_.publish({"lifecycle.changed", "RUNNING", 0});
  return st;
}

Status RuntimeManager::shutdown() {
  if (lifecycle_.current() == LifecycleState::Terminated)
    return Status::Fail(7, "core", "already terminated");
  Status st = lifecycle_.transition(LifecycleState::Stopping);
  if (!st) return st;
  bus_.publish({"lifecycle.changed", "STOPPING", 0});
  if (subsystems_ready_) {
    Status sst = registry_.shutdown_all();
    if (!sst) errors_.report(sst.error);
    subsystems_ready_ = false;
  }
  st = lifecycle_.transition(LifecycleState::Terminated);
  if (!st) return st;
  bus_.publish({"lifecycle.changed", "TERMINATED", 0});
  return Status::Ok();
}

// --- GameSession -----------------------------------------------------------
GameSession::GameSession(RuntimeManager& runtime, std::string id,
                         Launcher launcher)
    : runtime_(runtime), id_(std::move(id)), launcher_(std::move(launcher)) {}

Status GameSession::start(const std::string& exe,
                          const std::vector<std::string>& args) {
  if (active_) return Status::Fail(8, "session", "session already running");
  if (runtime_.lifecycle().current() != LifecycleState::Ready &&
      runtime_.lifecycle().current() != LifecycleState::Running)
    return Status::Fail(9, "session", "runtime not ready for a session");
  if (!launcher_) return Status::Fail(10, "session", "no launcher configured");

  Status st = runtime_.mark_running();
  if (!st) return st;
  runtime_.events().publish({"session.started", id_, 0});

  int sig = 0;
  last_exit_code_ = launcher_(exe, args, &sig);
  active_ = false;

  if (sig != 0) {
    Error e{20, "session",
            "game process terminated by signal " + std::to_string(sig)};
    runtime_.errors().report(e);
    runtime_.events().publish(
        {"session.crashed", id_ + " signal=" + std::to_string(sig), 0});
    // Never report a crashed game as a successful run (rule 15).
    st = Status::Fail(20, "session", e.message);
  } else {
    runtime_.events().publish(
        {"session.exited", id_ + " exit=" + std::to_string(last_exit_code_), 0});
    if (last_exit_code_ != 0)
      st = Status::Fail(21, "session",
                        "game exited with code " +
                            std::to_string(last_exit_code_));
  }
  return st;
}

Status GameSession::stop() {
  // Cooperative stop lands with the process supervisor in the loader
  // module; core only records intent for now.
  runtime_.events().publish({"session.stop_requested", id_, 0});
  return Status::Ok();
}

}  // namespace pnr
