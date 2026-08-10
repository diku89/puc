#pragma once

/**
 * @file state.hpp
 * @brief Typed subsystem registry and dependency-aware application lifecycle.
 */

#include <concepts>
#include <cstddef>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "puc-cli/state/lifecycle.hpp"
#include "utils/execution_graph/dependency_graph.hpp"

namespace puc::app {

class AppState;

/** Concrete C++ type used as one stable subsystem identity. */
using SubsystemId = std::type_index;

/**
 * Application-owned lifecycle adapter around one cohesive mechanism.
 *
 * Reusable mechanisms such as JobQueue, Directory, Decoder, and Logger remain
 * independent of the application layer. Concrete AppSubsystem adapters own or
 * borrow those mechanisms and declare dependencies on other adapter types.
 * Hook failure must leave the failing hook's own partial work quiescent;
 * AppState rolls back hooks that completed successfully before it.
 */
class AppSubsystem {
 public:
  /** Construct a named subsystem with concrete-type dependency identities. */
  explicit AppSubsystem(std::string name,
                        std::vector<SubsystemId> dependencies = {});

  AppSubsystem(const AppSubsystem&)            = delete;
  AppSubsystem& operator=(const AppSubsystem&) = delete;
  AppSubsystem(AppSubsystem&&)                 = delete;
  AppSubsystem& operator=(AppSubsystem&&)      = delete;

  /** Destroy one adapter after AppState has requested termination. */
  virtual ~AppSubsystem();

  /** Return the unique human-readable diagnostic name. */
  std::string_view name() const noexcept { return name_; }

  /** Return concrete subsystem types that must precede this subsystem. */
  std::span<const SubsystemId> dependencies() const noexcept {
    return dependencies_;
  }

  /** Configure resources without beginning externally observable work. */
  virtual Status initialize(AppState& app) = 0;

  /** Begin work after every declared dependency has started. */
  virtual Status start(AppState& app) = 0;

  /** Quiesce work before any declared dependency is stopped. */
  virtual Status stop(AppState& app) noexcept = 0;

  /** Release resources before any declared dependency is terminated. */
  virtual Status terminate(AppState& app) noexcept = 0;

 private:
  std::string name_; /**< Unique diagnostic name, not registry identity. */
  std::vector<SubsystemId>
      dependencies_; /**< Concrete prerequisite subsystem types. */
};

/** Types accepted as concrete application subsystem adapters. */
template <typename SubsystemType>
concept ApplicationSubsystem = std::derived_from<SubsystemType, AppSubsystem> &&
                               !std::same_as<SubsystemType, AppSubsystem>;

/** Build a concrete dependency list without spelling std::type_index. */
template <ApplicationSubsystem... SubsystemTypes>
std::vector<SubsystemId> subsystem_dependencies() {
  return {SubsystemId{typeid(SubsystemTypes)}...};
}

/**
 * Own, resolve, and orchestrate the application's subsystem adapters.
 *
 * Registration is keyed by concrete C++ type and freezes permanently when
 * initialize() begins. Dependency topology is validated by
 * execution_graph::DependencyGraph. Initialize and start traverse its forward
 * layers; stop and terminate traverse its reverse layers. Hooks within a layer
 * currently run synchronously in registration order so bootstrap facilities,
 * including the worker pool itself, do not depend on an already-running pool.
 *
 * Lifecycle calls are serialized and may safely resolve other subsystems from
 * inside a hook. Lookup pointers remain valid through AppState destruction;
 * callers must not retain them beyond that boundary.
 */
class AppState final {
 public:
  /** Construct an empty, open subsystem registry in TUI mode. */
  AppState() = default;

  AppState(const AppState&)            = delete;
  AppState& operator=(const AppState&) = delete;
  AppState(AppState&&)                 = delete;
  AppState& operator=(AppState&&)      = delete;

  /** Perform best-effort dependent-first teardown. */
  ~AppState();

  /**
   * Register one exclusively owned adapter under its concrete C++ type.
   *
   * A diagnostic name must also be unique. No registration is accepted after
   * the first initialize() attempt, including an attempt that fails.
   */
  template <ApplicationSubsystem SubsystemType>
  Status register_subsystem(std::unique_ptr<SubsystemType> subsystem) {
    return register_subsystem_erased(
        SubsystemId{typeid(SubsystemType)},
        std::unique_ptr<AppSubsystem>{std::move(subsystem)});
  }

  /** Return a borrowed adapter of exactly `SubsystemType`, or nullptr. */
  template <ApplicationSubsystem SubsystemType>
  SubsystemType* get_subsystem() noexcept {
    return static_cast<SubsystemType*>(
        find_subsystem(SubsystemId{typeid(SubsystemType)}));
  }

  /** Return a borrowed const adapter of exactly `SubsystemType`, or nullptr. */
  template <ApplicationSubsystem SubsystemType>
  const SubsystemType* get_subsystem() const noexcept {
    return static_cast<const SubsystemType*>(
        find_subsystem(SubsystemId{typeid(SubsystemType)}));
  }

  /** Return a borrowed type-erased adapter, or nullptr when absent. */
  AppSubsystem* find_subsystem(SubsystemId id) noexcept;

  /** Return a borrowed const type-erased adapter, or nullptr when absent. */
  const AppSubsystem* find_subsystem(SubsystemId id) const noexcept;

  /** Validate dependencies and initialize every subsystem dependency-first. */
  Status initialize(OperatingMode mode = OperatingMode::TUI);

  /** Start every initialized subsystem dependency-first. */
  Status start();

  /** Stop every started subsystem dependent-first. */
  Status stop() noexcept;

  /** Stop active work and terminate initialized subsystems dependent-first. */
  Status terminate() noexcept;

  /** Return the number of exclusively owned subsystem adapters. */
  std::size_t size() const noexcept;

  /** Return whether no later subsystem registration can succeed. */
  bool registration_frozen() const noexcept;

  /** Return the current serialized lifecycle state. */
  LifecycleState lifecycle_state() const noexcept;

  /** Return the operating mode selected by initialize(). */
  OperatingMode operating_mode() const noexcept;

 private:
  /** Per-adapter ownership and successfully completed lifecycle phases. */
  struct Entry {
    std::unique_ptr<AppSubsystem> subsystem; /**< Exclusively owned adapter. */
    bool initialized = false; /**< Whether initialize() completed. */
    bool started     = false; /**< Whether start() completed without stop(). */
  };

  using Topology = execution_graph::DependencyGraph<SubsystemId>;
  using Layers   = Topology::Layers;

  /** Register one adapter after its concrete type has been erased. */
  Status register_subsystem_erased(SubsystemId id,
                                   std::unique_ptr<AppSubsystem> subsystem);

  /** Construct and cache validated forward and reverse lifecycle layers. */
  Status build_topology();

  /** Return stable entry storage after registration has frozen. */
  Entry* find_entry(SubsystemId id) noexcept;

  /** Stop started entries without crossing a failed dependency layer. */
  Status stop_started(bool best_effort) noexcept;

  /** Terminate every initialized entry in cached reverse layer order. */
  Status terminate_initialized() noexcept;

  /** Roll back a successful prefix of a failed start operation. */
  Status rollback_started(const std::vector<SubsystemId>& started) noexcept;

  /** Roll back a successful prefix of a failed initialize operation. */
  Status rollback_initialized(
      const std::vector<SubsystemId>& initialized) noexcept;

  mutable std::recursive_mutex
      lifecycle_mutex_; /**< Serializes transitions while permitting queries. */
  mutable std::shared_mutex
      registry_mutex_; /**< Protects registration and type lookup. */
  std::unordered_map<SubsystemId, Entry>
      subsystems_; /**< Concrete-type registry and exclusive ownership. */
  std::unordered_map<std::string, SubsystemId>
      subsystem_names_; /**< Diagnostic-name uniqueness index. */
  std::vector<SubsystemId>
      registration_order_; /**< Stable order within independent layers. */
  std::unique_ptr<Topology> topology_; /**< Validated shared DAG mechanism. */
  Layers forward_layers_; /**< Dependency-first initialization/start order. */
  Layers reverse_layers_; /**< Dependent-first stop/termination order. */
  LifecycleState state_ = LifecycleState::UNINITIALIZED; /**< Current phase. */
  OperatingMode mode_   = OperatingMode::TUI; /**< Selected runtime profile. */
  bool registration_frozen_ = false; /**< Whether graph construction began. */
};

}  // namespace puc::app
