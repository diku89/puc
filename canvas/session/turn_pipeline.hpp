#pragma once

/**
 * @file turn_pipeline.hpp
 * @brief Runtime-extensible Turn-processing execution graph.
 */

#include <atomic>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "canvas/protos/datastore/status.hpp"
#include "canvas/protos/turn.pb.h"
#include "utils/execution_graph/status.hpp"

namespace puc::multithreading {
class JobQueue;
}

namespace puc::canvas {

/** Mutable data shared by the registered nodes of one Turn graph run. */
class TurnContext final {
 public:
  /** Return the uncommitted Turn received by the pipeline. */
  const proto::Turn& submitted() const noexcept { return submitted_; }

  /** Return the Turn being constructed by registered processing nodes. */
  proto::Turn& turn() noexcept { return turn_; }

  /** Return the Turn being constructed by registered processing nodes. */
  const proto::Turn& turn() const noexcept { return turn_; }

  /** Record the first processing failure while preserving earlier failures. */
  void fail(datastore::Status status) noexcept;

  /** Return the first failure, or OK while processing may continue. */
  datastore::Status status() const noexcept { return status_.load(); }

 private:
  friend class TurnPipeline;

  /** Reset this context for one serialized graph run. */
  void reset(const proto::Turn& submitted);

  proto::Turn submitted_; /**< Immutable input copied from IPC. */
  proto::Turn turn_;      /**< Incrementally constructed committed Turn. */
  std::atomic<datastore::Status> status_{
      datastore::Status::OK}; /**< First run failure, if any. */
};

/**
 * Own a runtime-extensible graph of named Turn-processing callbacks.
 *
 * TurnPipeline defines no stages or topology. Subsystems register uniquely
 * named nodes and their prerequisite node names. Registrations may change
 * between runs; each process() call snapshots them into the repository's
 * ExecutionGraph and executes that validated snapshot on the attached worker
 * pool.
 */
class TurnPipeline final {
 public:
  /** Stable name used to identify a graph node and declare dependencies. */
  using NodeId = std::string;

  /** Callback invoked once for its node during a graph run. */
  using Handler = std::function<void(TurnContext&)>;

  /** Construct an unattached pipeline with no registered nodes. */
  TurnPipeline() = default;

  TurnPipeline(const TurnPipeline&)            = delete;
  TurnPipeline& operator=(const TurnPipeline&) = delete;

  /** Attach the active worker pool used by subsequent graph runs. */
  execution_graph::Status attach(multithreading::JobQueue& workers) noexcept;

  /** Detach the stopped worker pool after any active process() returns. */
  void detach() noexcept;

  /**
   * Register a named callback and its already-registered prerequisites.
   *
   * Registration is accepted only between process() calls. Node names are the
   * dependency API; no pipeline-owned enum or fixed sequence exists.
   */
  execution_graph::Status register_node(NodeId node, Handler handler,
                                        std::vector<NodeId> dependencies = {});

  /** Remove an idle leaf node that no remaining registration depends upon. */
  execution_graph::Status unregister_node(std::string_view node);

  /** Execute one snapshot of the registered graph for a submitted Turn. */
  datastore::Status process(const proto::Turn& submitted,
                            proto::Turn& committed_turn);

  /** Return the number of currently registered graph nodes. */
  std::size_t size() const noexcept;

 private:
  /** One registered node independent of an individual graph run. */
  struct Registration {
    NodeId node;                      /**< Stable graph identity. */
    Handler handler;                  /**< Work invoked for each Turn. */
    std::vector<NodeId> dependencies; /**< Required predecessor nodes. */
  };

  mutable std::mutex mutex_; /**< Serializes registration and graph runs. */
  multithreading::JobQueue* workers_ =
      nullptr;                              /**< Borrowed active executor. */
  std::vector<Registration> registrations_; /**< Stable registration order. */
};

}  // namespace puc::canvas
