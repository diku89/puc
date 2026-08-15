#pragma once

/**
 * @file turn_pipeline.hpp
 * @brief Runtime-extensible Turn-processing execution graph.
 */

#include <any>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
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

  /**
   * Return the process-local FIFO ticket assigned at pipeline submission.
   *
   * This transient scheduling value is neither persisted nor part of Turn
   * identity. Resource owners may use it to make an otherwise-unfair mutex
   * honor ingress order.
   */
  std::uint64_t ingress_ticket() const noexcept { return ingress_ticket_; }

  /** Retain run-local state for a later node without sharing it across Turns.
   */
  template <typename Value>
  void store(std::string key, Value value) {
    const std::lock_guard lock(state_mutex_);
    state_.insert_or_assign(std::move(key), std::any{std::move(value)});
  }

  /** Move and remove typed run-local state retained by an earlier node. */
  template <typename Value>
  std::optional<Value> take(std::string_view key) {
    const std::lock_guard lock(state_mutex_);
    const auto found = state_.find(std::string{key});
    if (found == state_.end()) return std::nullopt;
    Value* value = std::any_cast<Value>(&found->second);
    if (value == nullptr) return std::nullopt;
    std::optional<Value> result{std::move(*value)};
    state_.erase(found);
    return result;
  }

 private:
  friend class TurnPipeline;

  /** Reset this context for one independent graph run. */
  void reset(const proto::Turn& submitted, std::uint64_t ingress_ticket);

  proto::Turn submitted_; /**< Immutable input copied from IPC. */
  proto::Turn turn_;      /**< Incrementally constructed committed Turn. */
  std::uint64_t ingress_ticket_ = 0U; /**< Transient FIFO submission order. */
  std::atomic<datastore::Status> status_{
      datastore::Status::OK}; /**< First run failure, if any. */
  std::mutex state_mutex_; /**< Protects cross-node run-local scratch state. */
  std::unordered_map<std::string, std::any>
      state_; /**< Typed values retained only for this Turn run. */
};

/**
 * Own a runtime-extensible graph of named Turn-processing callbacks.
 *
 * TurnPipeline defines no stages or topology. Subsystems register uniquely
 * named nodes and their prerequisite node names. Every registration change
 * publishes a newly validated immutable topology; in-flight Turns retain the
 * prior topology while later Turns use the replacement.
 *
 * Runs are independent and may overlap. The pipeline never serializes a Turn
 * around the entire graph. Each registered callback is responsible for
 * synchronizing only the resource it owns, such as a numbering allocator,
 * database transaction, Trie, or Presentation root. Model inference, tool
 * execution, and other potentially long-lived workflows should trigger their
 * own execution graphs rather than extend this authoritative commit path.
 */
class TurnPipeline final {
 public:
  /** Stable name used to identify a graph node and declare dependencies. */
  using NodeId = std::string;

  /** Callback invoked once for its node during a graph run. */
  using Handler = std::function<void(TurnContext&)>;

  /** Callback invoked once after one accepted Turn run finishes. */
  using Completion =
      std::function<void(datastore::Status, proto::Turn committed_turn)>;

  /** Construct an unattached pipeline with no registered nodes. */
  TurnPipeline();

  /** Stop accepting work and wait for every accepted Turn run. */
  ~TurnPipeline();

  TurnPipeline(const TurnPipeline&)            = delete;
  TurnPipeline& operator=(const TurnPipeline&) = delete;

  /** Attach the active worker pool used by subsequent graph runs. */
  execution_graph::Status attach(multithreading::JobQueue& workers) noexcept;

  /** Stop accepting work, drain accepted runs, and detach the worker pool. */
  void detach() noexcept;

  /**
   * Register a named callback and its already-registered prerequisites.
   *
   * Node names are the dependency API; no pipeline-owned enum or fixed
   * sequence exists. Accepted Turns retain the topology they were submitted
   * against, so registration does not wait for an in-flight run.
   */
  execution_graph::Status register_node(NodeId node, Handler handler,
                                        std::vector<NodeId> dependencies = {});

  /**
   * Remove a leaf node from future submissions.
   *
   * In-flight plans retain their copied Handler. Resources captured by a
   * Handler must therefore use shared ownership or live until detach() drains
   * accepted runs.
   */
  execution_graph::Status unregister_node(std::string_view node);

  /**
   * Submit one Turn without occupying an ingestion or worker thread waiting.
   *
   * Returning OK guarantees one later completion callback. The callback must
   * not detach or destroy this pipeline from inside itself.
   */
  execution_graph::Status submit(const proto::Turn& submitted,
                                 Completion completion);

  /**
   * Submit and synchronously collect one Turn for tests and non-worker callers.
   *
   * Runtime ingestion should use submit(). Calling this from the attached
   * worker pool is rejected to prevent pool starvation.
   */
  datastore::Status process(const proto::Turn& submitted,
                            proto::Turn& committed_turn);

  /** Return the number of currently registered graph nodes. */
  std::size_t size() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_; /**< Registration, plan, and run lifecycle. */
};

}  // namespace puc::canvas
