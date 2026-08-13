/**
 * @file state.cpp
 * @brief Dependency-aware application subsystem lifecycle implementation.
 */

#include "state/state.hpp"

#include <exception>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <typeindex>
#include <utility>
#include <vector>

namespace puc::app {
namespace {

/** Return whether one OperatingMode enumerator is part of the public contract.
 */
constexpr bool valid_operating_mode(OperatingMode mode) noexcept {
  return mode == OperatingMode::TUI || mode == OperatingMode::TEST;
}

/** Translate topology construction results into application lifecycle results.
 */
constexpr Status topology_status(execution_graph::Status status) noexcept {
  switch (status) {
    case execution_graph::Status::OK:
      return Status::OK;
    case execution_graph::Status::DUPLICATE_NODE:
      return Status::DUPLICATE_SUBSYSTEM;
    case execution_graph::Status::NODE_NOT_FOUND:
      return Status::MISSING_DEPENDENCY;
    case execution_graph::Status::DUPLICATE_DEPENDENCY:
      return Status::DUPLICATE_DEPENDENCY;
    case execution_graph::Status::DEPENDENCY_CYCLE:
      return Status::DEPENDENCY_CYCLE;
    case execution_graph::Status::INVALID_ARGUMENT:
    case execution_graph::Status::EXECUTION_IN_PROGRESS:
    case execution_graph::Status::WORKER_SUBMISSION_FAILED:
      return Status::INTERNAL_ERROR;
  }
  return Status::INTERNAL_ERROR;
}

/** Keep the first lifecycle failure while continuing best-effort teardown. */
void record_failure(Status candidate, Status& first_failure) noexcept {
  if (is_ok(first_failure) && !is_ok(candidate)) {
    first_failure = candidate;
  }
}

}  // namespace

AppSubsystem::AppSubsystem(std::string name,
                           std::vector<SubsystemId> dependencies)
    : name_(std::move(name)), dependencies_(std::move(dependencies)) {}

AppSubsystem::~AppSubsystem() = default;

AppState::~AppState() { static_cast<void>(terminate()); }

Status AppState::register_subsystem_erased(
    SubsystemId id, std::unique_ptr<AppSubsystem> subsystem) {
  if (subsystem == nullptr || subsystem->name().empty()) {
    return Status::INVALID_ARGUMENT;
  }

  const std::lock_guard lifecycle_lock(lifecycle_mutex_);
  const std::unique_lock registry_lock(registry_mutex_);
  if (registration_frozen_) {
    return Status::REGISTRATION_FROZEN;
  }
  if (subsystems_.contains(id)) {
    return Status::DUPLICATE_SUBSYSTEM;
  }
  const std::string name{subsystem->name()};
  if (subsystem_names_.contains(name)) {
    return Status::DUPLICATE_SUBSYSTEM_NAME;
  }

  subsystem_names_.emplace(name, id);
  subsystems_.emplace(id, Entry{.subsystem   = std::move(subsystem),
                                .initialized = false,
                                .started     = false});
  registration_order_.push_back(id);
  return Status::OK;
}

AppSubsystem* AppState::find_subsystem(SubsystemId id) noexcept {
  const std::shared_lock lock(registry_mutex_);
  const auto entry = subsystems_.find(id);
  return entry == subsystems_.end() ? nullptr : entry->second.subsystem.get();
}

const AppSubsystem* AppState::find_subsystem(SubsystemId id) const noexcept {
  const std::shared_lock lock(registry_mutex_);
  const auto entry = subsystems_.find(id);
  return entry == subsystems_.end() ? nullptr : entry->second.subsystem.get();
}

Status AppState::initialize(OperatingMode mode) {
  const std::lock_guard lock(lifecycle_mutex_);
  if (state_ != LifecycleState::UNINITIALIZED) {
    return Status::INVALID_LIFECYCLE_TRANSITION;
  }
  if (!valid_operating_mode(mode)) {
    return Status::INVALID_ARGUMENT;
  }
  {
    const std::unique_lock registry_lock(registry_mutex_);
    registration_frozen_ = true;
  }

  mode_                        = mode;
  state_                       = LifecycleState::INITIALIZING;
  const Status topology_result = build_topology();
  if (!is_ok(topology_result)) {
    state_ = LifecycleState::CRASHED;
    return topology_result;
  }

  std::vector<SubsystemId> initialized;
  initialized.reserve(subsystems_.size());
  for (const Topology::Layer& layer : forward_layers_) {
    for (const SubsystemId id : layer) {
      Entry* entry = find_entry(id);
      if (entry == nullptr) {
        static_cast<void>(rollback_initialized(initialized));
        state_ = LifecycleState::CRASHED;
        return Status::INTERNAL_ERROR;
      }

      Status hook_status = Status::SUBSYSTEM_FAILURE;
      try {
        hook_status = entry->subsystem->initialize(*this);
      } catch (...) {
        hook_status = Status::SUBSYSTEM_FAILURE;
      }
      if (!is_ok(hook_status)) {
        static_cast<void>(rollback_initialized(initialized));
        state_ = LifecycleState::CRASHED;
        return hook_status;
      }
      entry->initialized = true;
      initialized.push_back(id);
    }
  }

  state_ = LifecycleState::INITIALIZED;
  return Status::OK;
}

Status AppState::start() {
  const std::lock_guard lock(lifecycle_mutex_);
  if (state_ != LifecycleState::INITIALIZED &&
      state_ != LifecycleState::STOPPED) {
    return Status::INVALID_LIFECYCLE_TRANSITION;
  }

  const LifecycleState previous_state = state_;
  state_                              = LifecycleState::STARTING;
  std::vector<SubsystemId> started;
  started.reserve(subsystems_.size());
  for (const Topology::Layer& layer : forward_layers_) {
    for (const SubsystemId id : layer) {
      Entry* entry = find_entry(id);
      if (entry == nullptr || !entry->initialized) {
        static_cast<void>(rollback_started(started));
        state_ = LifecycleState::CRASHED;
        return Status::INTERNAL_ERROR;
      }

      Status hook_status = Status::SUBSYSTEM_FAILURE;
      try {
        hook_status = entry->subsystem->start(*this);
      } catch (...) {
        hook_status = Status::SUBSYSTEM_FAILURE;
      }
      if (!is_ok(hook_status)) {
        const Status rollback_status = rollback_started(started);
        state_ =
            is_ok(rollback_status) ? previous_state : LifecycleState::CRASHED;
        return hook_status;
      }
      entry->started = true;
      started.push_back(id);
    }
  }

  state_ = LifecycleState::RUNNING;
  return Status::OK;
}

Status AppState::stop() noexcept {
  const std::lock_guard lock(lifecycle_mutex_);
  if (state_ == LifecycleState::UNINITIALIZED ||
      state_ == LifecycleState::INITIALIZED ||
      state_ == LifecycleState::STOPPED ||
      state_ == LifecycleState::TERMINATED) {
    return Status::OK;
  }
  if (state_ != LifecycleState::RUNNING && state_ != LifecycleState::CRASHED) {
    return Status::INVALID_LIFECYCLE_TRANSITION;
  }

  const bool was_crashed = state_ == LifecycleState::CRASHED;
  state_                 = LifecycleState::STOPPING;
  const Status status    = stop_started(false);
  state_ = is_ok(status) && !was_crashed ? LifecycleState::STOPPED
                                         : LifecycleState::CRASHED;
  return status;
}

Status AppState::terminate() noexcept {
  const std::lock_guard lock(lifecycle_mutex_);
  if (state_ == LifecycleState::TERMINATED) {
    return Status::OK;
  }
  if (state_ == LifecycleState::INITIALIZING ||
      state_ == LifecycleState::STARTING ||
      state_ == LifecycleState::STOPPING ||
      state_ == LifecycleState::TERMINATING) {
    return Status::INVALID_LIFECYCLE_TRANSITION;
  }
  if (state_ == LifecycleState::UNINITIALIZED) {
    {
      const std::unique_lock registry_lock(registry_mutex_);
      registration_frozen_ = true;
    }
    state_ = LifecycleState::TERMINATED;
    return Status::OK;
  }

  state_               = LifecycleState::TERMINATING;
  Status first_failure = stop_started(true);
  record_failure(terminate_initialized(), first_failure);
  state_ = is_ok(first_failure) ? LifecycleState::TERMINATED
                                : LifecycleState::CRASHED;
  return first_failure;
}

std::size_t AppState::size() const noexcept {
  const std::shared_lock lock(registry_mutex_);
  return subsystems_.size();
}

bool AppState::registration_frozen() const noexcept {
  const std::shared_lock lock(registry_mutex_);
  return registration_frozen_;
}

LifecycleState AppState::lifecycle_state() const noexcept {
  const std::lock_guard lock(lifecycle_mutex_);
  return state_;
}

OperatingMode AppState::operating_mode() const noexcept {
  const std::lock_guard lock(lifecycle_mutex_);
  return mode_;
}

Status AppState::build_topology() {
  auto topology = std::make_unique<Topology>();
  {
    const std::shared_lock lock(registry_mutex_);
    for (const SubsystemId id : registration_order_) {
      const execution_graph::Status status = topology->add_node(id);
      if (!execution_graph::is_ok(status)) {
        return topology_status(status);
      }
    }
    for (const SubsystemId id : registration_order_) {
      const auto entry = subsystems_.find(id);
      if (entry == subsystems_.end()) {
        return Status::INTERNAL_ERROR;
      }
      for (const SubsystemId dependency :
           entry->second.subsystem->dependencies()) {
        if (!subsystems_.contains(dependency)) {
          return Status::MISSING_DEPENDENCY;
        }
        const execution_graph::Status status =
            topology->add_dependency(dependency, id);
        if (!execution_graph::is_ok(status)) {
          return topology_status(status);
        }
      }
    }
  }

  Layers forward;
  execution_graph::Status graph_status = topology->forward_layers(forward);
  if (!execution_graph::is_ok(graph_status)) {
    return topology_status(graph_status);
  }
  Layers reverse;
  graph_status = topology->reverse_layers(reverse);
  if (!execution_graph::is_ok(graph_status)) {
    return topology_status(graph_status);
  }

  topology_       = std::move(topology);
  forward_layers_ = std::move(forward);
  reverse_layers_ = std::move(reverse);
  return Status::OK;
}

AppState::Entry* AppState::find_entry(SubsystemId id) noexcept {
  const std::shared_lock lock(registry_mutex_);
  const auto entry = subsystems_.find(id);
  return entry == subsystems_.end() ? nullptr : &entry->second;
}

Status AppState::stop_started(bool best_effort) noexcept {
  Status first_failure = Status::OK;
  for (const Topology::Layer& layer : reverse_layers_) {
    Status layer_failure = Status::OK;
    for (const SubsystemId id : layer) {
      Entry* entry = find_entry(id);
      if (entry == nullptr) {
        record_failure(Status::INTERNAL_ERROR, first_failure);
        record_failure(Status::INTERNAL_ERROR, layer_failure);
        continue;
      }
      if (!entry->started) {
        continue;
      }
      const Status status = entry->subsystem->stop(*this);
      record_failure(status, first_failure);
      record_failure(status, layer_failure);
      if (is_ok(status)) {
        entry->started = false;
      }
    }
    if (!best_effort && !is_ok(layer_failure)) {
      break;
    }
  }
  return first_failure;
}

Status AppState::terminate_initialized() noexcept {
  Status first_failure = Status::OK;
  for (const Topology::Layer& layer : reverse_layers_) {
    for (const SubsystemId id : layer) {
      Entry* entry = find_entry(id);
      if (entry == nullptr) {
        record_failure(Status::INTERNAL_ERROR, first_failure);
        continue;
      }
      if (!entry->initialized) {
        continue;
      }
      const Status status = entry->subsystem->terminate(*this);
      record_failure(status, first_failure);
      if (is_ok(status)) {
        entry->started     = false;
        entry->initialized = false;
      }
    }
  }
  return first_failure;
}

Status AppState::rollback_started(
    const std::vector<SubsystemId>& started) noexcept {
  Status first_failure = Status::OK;
  for (auto id = started.rbegin(); id != started.rend(); ++id) {
    Entry* entry = find_entry(*id);
    if (entry == nullptr) {
      record_failure(Status::INTERNAL_ERROR, first_failure);
      continue;
    }
    const Status status = entry->subsystem->stop(*this);
    record_failure(status, first_failure);
    if (is_ok(status)) {
      entry->started = false;
    } else {
      break;
    }
  }
  return first_failure;
}

Status AppState::rollback_initialized(
    const std::vector<SubsystemId>& initialized) noexcept {
  Status first_failure = Status::OK;
  for (auto id = initialized.rbegin(); id != initialized.rend(); ++id) {
    Entry* entry = find_entry(*id);
    if (entry == nullptr) {
      record_failure(Status::INTERNAL_ERROR, first_failure);
      continue;
    }
    const Status status = entry->subsystem->terminate(*this);
    record_failure(status, first_failure);
    if (is_ok(status)) {
      entry->started     = false;
      entry->initialized = false;
    } else {
      break;
    }
  }
  return first_failure;
}

}  // namespace puc::app
