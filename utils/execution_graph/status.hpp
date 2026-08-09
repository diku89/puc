#pragma once

/**
 * @file status.hpp
 * @brief Human-readable execution-graph construction and run results.
 */

#include <string_view>

namespace puc::execution_graph {

/** Expected outcomes from graph construction, scheduling, and completion. */
enum class Status {
  OK,                       /**< The operation completed successfully. */
  INVALID_ARGUMENT,         /**< Pool is stopped or a job pointer is absent. */
  DUPLICATE_NODE,           /**< The graph already contains the node value. */
  NODE_NOT_FOUND,           /**< A dependency endpoint is not registered. */
  DUPLICATE_DEPENDENCY,     /**< The directed edge already exists. */
  DEPENDENCY_CYCLE,         /**< The configured graph is not acyclic. */
  EXECUTION_IN_PROGRESS,    /**< A run is active or awaiting wait(). */
  WORKER_SUBMISSION_FAILED, /**< The worker pool rejected a ready node. */
};

/** Return whether an execution-graph operation succeeded. */
constexpr bool is_ok(Status status) noexcept { return status == Status::OK; }

/** Return stable, human-readable text for every execution-graph status. */
constexpr std::string_view status_message(Status status) noexcept {
  switch (status) {
    case Status::OK:
      return "success";
    case Status::INVALID_ARGUMENT:
      return "worker pool must be active and node jobs must be present";
    case Status::DUPLICATE_NODE:
      return "execution graph node already exists";
    case Status::NODE_NOT_FOUND:
      return "execution graph node was not found";
    case Status::DUPLICATE_DEPENDENCY:
      return "execution graph dependency already exists";
    case Status::DEPENDENCY_CYCLE:
      return "execution graph contains a dependency cycle";
    case Status::EXECUTION_IN_PROGRESS:
      return "execution graph run is active or awaiting collection";
    case Status::WORKER_SUBMISSION_FAILED:
      return "worker pool rejected an execution graph node";
  }
  return "unknown execution graph status";
}

}  // namespace puc::execution_graph
