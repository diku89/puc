#pragma once

/**
 * @file lifecycle.hpp
 * @brief Application operating modes, lifecycle states, and result codes.
 */

#include <string_view>

namespace puc::app {

/** Runtime profile selected before subsystem initialization begins. */
enum class OperatingMode {
  TUI,  /**< Interactive terminal user interface. */
  TEST, /**< Deterministic test-oriented application composition. */
};

/** Complete state of the application subsystem lifecycle. */
enum class LifecycleState {
  UNINITIALIZED, /**< Subsystems may still be registered. */
  INITIALIZING,  /**< Dependency-first initialization is in progress. */
  INITIALIZED,   /**< Every subsystem is initialized but not started. */
  STARTING,      /**< Dependency-first startup is in progress. */
  RUNNING,       /**< Every subsystem has started successfully. */
  STOPPING,      /**< Dependent-first quiescence is in progress. */
  STOPPED,       /**< Subsystems remain initialized but are not running. */
  TERMINATING,   /**< Dependent-first resource release is in progress. */
  TERMINATED,    /**< Every initialized subsystem has been released. */
  CRASHED,       /**< A lifecycle hook or dependency contract failed. */
};

/** Result of subsystem registration and lifecycle orchestration. */
enum class Status {
  OK,                           /**< The operation completed successfully. */
  INVALID_ARGUMENT,             /**< A pointer, name, or mode is invalid. */
  DUPLICATE_SUBSYSTEM,          /**< The concrete subsystem type exists. */
  DUPLICATE_SUBSYSTEM_NAME,     /**< A diagnostic name already exists. */
  SUBSYSTEM_NOT_FOUND,          /**< No subsystem has the requested type. */
  MISSING_DEPENDENCY,           /**< A declared subsystem was not registered. */
  DUPLICATE_DEPENDENCY,         /**< One dependency was declared twice. */
  DEPENDENCY_CYCLE,             /**< Subsystem dependencies are cyclic. */
  REGISTRATION_FROZEN,          /**< Lifecycle processing has already begun. */
  INVALID_LIFECYCLE_TRANSITION, /**< The operation is invalid in this state. */
  SUBSYSTEM_FAILURE,            /**< A subsystem lifecycle hook failed. */
  INTERNAL_ERROR,               /**< Lifecycle infrastructure failed. */
};

/** Return whether an application lifecycle operation succeeded. */
constexpr bool is_ok(Status status) noexcept { return status == Status::OK; }

/** Return stable human-readable text for an application lifecycle result. */
constexpr std::string_view status_message(Status status) noexcept {
  switch (status) {
    case Status::OK:
      return "success";
    case Status::INVALID_ARGUMENT:
      return "invalid application lifecycle argument";
    case Status::DUPLICATE_SUBSYSTEM:
      return "subsystem type is already registered";
    case Status::DUPLICATE_SUBSYSTEM_NAME:
      return "subsystem name is already registered";
    case Status::SUBSYSTEM_NOT_FOUND:
      return "subsystem was not found";
    case Status::MISSING_DEPENDENCY:
      return "subsystem dependency was not registered";
    case Status::DUPLICATE_DEPENDENCY:
      return "subsystem dependency is declared more than once";
    case Status::DEPENDENCY_CYCLE:
      return "subsystem dependencies contain a cycle";
    case Status::REGISTRATION_FROZEN:
      return "subsystem registration is frozen";
    case Status::INVALID_LIFECYCLE_TRANSITION:
      return "application lifecycle transition is invalid";
    case Status::SUBSYSTEM_FAILURE:
      return "subsystem lifecycle hook failed";
    case Status::INTERNAL_ERROR:
      return "application lifecycle infrastructure failed";
  }
  return "unknown application lifecycle status";
}

}  // namespace puc::app
