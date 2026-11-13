// PNR Core Runtime - MODULE 01
// Central runtime: lifecycle, subsystem registry, configuration, events,
// capabilities, error reporting. No OS-specific code lives in this module.
// Status: Implemented (unit-tested). See docs/interface-contracts.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pnr {

// Honest feature-state labels, used repo-wide (prompt rule 12/15).
enum class FeatureStatus { Planned, Implemented, Tested, Experimental, Blocked };

const char* to_string(FeatureStatus s);

// Required lifecycle states from the master prompt.
enum class LifecycleState {
  Created,
  Validating,
  Initializing,
  Ready,
  Running,
  Suspended,
  Stopping,
  Terminated
};

const char* to_string(LifecycleState s);

struct Error {
  int code = 0;
  std::string subsystem;
  std::string message;
};

// Uniform success/failure result. No silent dummy successes (rule 15).
struct Status {
  bool ok = true;
  Error error{};
  static Status Ok() { return Status{}; }
  static Status Fail(int code, std::string subsystem, std::string message) {
    Status s;
    s.ok = false;
    s.error = Error{code, std::move(subsystem), std::move(message)};
    return s;
  }
  explicit operator bool() const { return ok; }
};

// --- RuntimeEventBus -------------------------------------------------------
struct Event {
  std::string type;     // e.g. "lifecycle.changed", "session.started"
  std::string payload;  // free-form, JSON-ish by convention
  uint64_t sequence = 0;
};

class RuntimeEventBus {
public:
  using Handler = std::function<void(const Event&)>;
  // Returns a subscription id usable with unsubscribe().
  uint64_t subscribe(std::string event_type, Handler handler);
  void unsubscribe(uint64_t subscription_id);
  void publish(Event e);  // synchronous, in publish order
  size_t subscriber_count(const std::string& event_type) const;

private:
  struct Sub { std::string type; Handler handler; };
  mutable std::mutex mutex_;
  uint64_t next_id_ = 1;
  uint64_t next_sequence_ = 1;
  std::map<uint64_t, Sub> subs_;
};

// --- SubsystemRegistry -----------------------------------------------------
class ISubsystem {
public:
  virtual ~ISubsystem() = default;
  virtual std::string name() const = 0;
  virtual Status initialize() = 0;
  virtual Status shutdown() = 0;
  virtual FeatureStatus status() const = 0;
};

class SubsystemRegistry {
public:
  Status add(std::shared_ptr<ISubsystem> subsystem);
  std::shared_ptr<ISubsystem> get(const std::string& name) const;
  std::vector<std::string> names() const;
  Status initialize_all();   // registration order; stops at first failure
  Status shutdown_all();     // reverse order; aggregates failures

private:
  mutable std::mutex mutex_;
  std::vector<std::shared_ptr<ISubsystem>> order_;
  std::map<std::string, std::shared_ptr<ISubsystem>> by_name_;
};

// --- ConfigurationManager --------------------------------------------------
// Flat key=value files. Versioned profiles build on top later.
class ConfigurationManager {
public:
  Status load_from_file(const std::string& path);  // missing file = failure, not silent default
  void set(const std::string& key, const std::string& value);
  std::optional<std::string> get(const std::string& key) const;
  std::string get_or(const std::string& key, const std::string& fallback) const;
  std::optional<int64_t> get_int(const std::string& key) const;

private:
  mutable std::mutex mutex_;
  std::map<std::string, std::string> values_;
};

// --- CapabilityRegistry ----------------------------------------------------
// Records what the host actually provides. Missing = reported missing.
struct Capability {
  bool supported = false;
  std::string note;  // why not, or evidence of support
};

class CapabilityRegistry {
public:
  void declare(const std::string& name, bool supported, std::string note);
  std::optional<Capability> query(const std::string& name) const;
  std::vector<std::pair<std::string, Capability>> all() const;

private:
  mutable std::mutex mutex_;
  std::map<std::string, Capability> caps_;
};

// --- ErrorReporter ---------------------------------------------------------
class ErrorReporter {
public:
  void report(Error e);
  const std::vector<Error>& errors() const { return errors_; }
  size_t count() const { return errors_.size(); }

private:
  std::vector<Error> errors_;
};

// --- LifecycleController ---------------------------------------------------
// Enforces the legal transition graph. Illegal transitions fail explicitly.
class LifecycleController {
public:
  Status transition(LifecycleState target);
  LifecycleState current() const { return state_; }
  const std::vector<LifecycleState>& history() const { return history_; }
  static bool is_legal(LifecycleState from, LifecycleState to);

private:
  LifecycleState state_ = LifecycleState::Created;
  std::vector<LifecycleState> history_{LifecycleState::Created};
};

// --- RuntimeManager --------------------------------------------------------
class RuntimeManager {
public:
  RuntimeManager();
  Status initialize();  // Created -> Validating -> Initializing -> Ready
  Status shutdown();    // any active state -> Stopping -> Terminated
  Status suspend();
  Status resume();
  Status mark_running();

  RuntimeEventBus& events() { return bus_; }
  SubsystemRegistry& subsystems() { return registry_; }
  ConfigurationManager& config() { return config_; }
  CapabilityRegistry& capabilities() { return caps_; }
  ErrorReporter& errors() { return errors_; }
  LifecycleController& lifecycle() { return lifecycle_; }

private:
  RuntimeEventBus bus_;
  SubsystemRegistry registry_;
  ConfigurationManager config_;
  CapabilityRegistry caps_;
  ErrorReporter errors_;
  LifecycleController lifecycle_;
  bool subsystems_ready_ = false;
};

// --- GameSession -----------------------------------------------------------
// Coordinates one game launch. The actual process mechanics are injected
// (launcher), so the core never depends on the loader module.
class GameSession {
public:
  using Launcher = std::function<int(const std::string& exe,
                                     const std::vector<std::string>& args,
                                     int* out_signal)>;
  // Launcher returns the child exit code, or -1 and sets *out_signal.
  GameSession(RuntimeManager& runtime, std::string id, Launcher launcher);

  Status start(const std::string& exe, const std::vector<std::string>& args);
  Status stop();
  int last_exit_code() const { return last_exit_code_; }
  const std::string& id() const { return id_; }

private:
  RuntimeManager& runtime_;
  std::string id_;
  Launcher launcher_;
  int last_exit_code_ = -1;
  bool active_ = false;
};

}  // namespace pnr
