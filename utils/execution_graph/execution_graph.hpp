#pragma once

/**
 * @file execution_graph.hpp
 * @brief Reusable dependency-aware scheduling over a fixed JobQueue.
 */

#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "utils/execution_graph/dependency_graph.hpp"
#include "utils/multithreading/job_queue.hpp"

/** Generic dependency-graph construction and asynchronous execution. */
namespace puc::execution_graph {

namespace detail {

/** Log one graph failure without imposing formatting on the node type. */
void log_failure(std::string_view operation, Status status) noexcept;

/** Log a successful lifecycle transition and its graph size. */
void log_transition(std::string_view operation,
                    std::size_t node_count) noexcept;

}  // namespace detail

/**
 * Node identities accepted by ExecutionGraph.
 *
 * This compatibility concept is the topology-only DependencyGraphNode
 * contract under the execution scheduler's established public name.
 */
template <typename NodeType>
concept ExecutionGraphNode = DependencyGraphNode<NodeType>;

/** A concrete worker job accepted as a graph node's executable body. */
template <typename JobType>
concept ExecutionGraphJob =
    std::derived_from<std::remove_cvref_t<JobType>, multithreading::Job>;

/**
 * Execute a reusable directed acyclic graph on a caller-owned worker pool.
 *
 * @tparam NodeType Copyable, equality-comparable, hashable graph identity.
 *
 * Each registered node owns a shared multithreading::Job. At `start()`, every
 * zero-dependency node is wrapped in an internal completion job and submitted.
 * Completion decrements the remaining dependency count of each successor and
 * submits successors precisely when they become unblocked. Thus every ready
 * branch can run concurrently while no job starts before all prerequisites
 * finish.
 *
 * Topology is validated only after it changes; subsequent runs reuse the same
 * validated nodes, edges, and jobs. `wait()` consumes one run's result and
 * returns the graph to its reusable idle state. Construction methods and
 * lifecycle methods are thread-safe, although only one run may exist at once.
 * Node jobs themselves retain their ordinary caller-defined synchronization
 * responsibilities.
 */
template <ExecutionGraphNode NodeType>
class ExecutionGraph {
 private:
  class Impl;

  /** Internal job that observes completion of one caller-owned node job. */
  class CompletionJob final : public multithreading::Job {
   public:
    CompletionJob(std::shared_ptr<Impl> implementation, std::size_t node_index,
                  std::uint64_t generation)
        : implementation_(std::move(implementation)),
          node_index_(node_index),
          generation_(generation) {}

    void execute() noexcept override {
      implementation_->execute_node(node_index_, generation_);
    }

   private:
    std::shared_ptr<Impl> implementation_; /**< Graph kept alive by the job. */
    std::size_t node_index_;               /**< Dense registered-node index. */
    std::uint64_t generation_;             /**< Run generation to complete. */
  };

  /** Shared graph state retained independently by scheduled wrapper jobs. */
  class Impl : public std::enable_shared_from_this<Impl> {
   public:
    /** One immutable job plus copied topology and per-run readiness state. */
    struct Node {
      std::shared_ptr<multithreading::Job> job; /**< Work invoked per run. */
      std::vector<std::size_t> dependents;      /**< Outgoing directed edges. */
      std::size_t dependency_count = 0U; /**< Static incoming edge count. */
      std::size_t remaining_dependencies = 0U; /**< Current-run readiness. */
    };

    enum class RunState {
      IDLE,     /**< Topology may be changed and a run may start. */
      RUNNING,  /**< At least one run has uncompleted logical nodes. */
      COMPLETE, /**< Run result exists and must be consumed by wait(). */
    };

    explicit Impl(multithreading::JobQueue& configured_workers) noexcept
        : workers(configured_workers) {}

    /** Add one uniquely identified executable node while idle. */
    Status add(NodeType value, std::shared_ptr<multithreading::Job> job) {
      if (job == nullptr) {
        detail::log_failure("add node", Status::INVALID_ARGUMENT);
        return Status::INVALID_ARGUMENT;
      }
      const std::lock_guard lock(mutex);
      if (state != RunState::IDLE) {
        return Status::EXECUTION_IN_PROGRESS;
      }
      const Status topology_status = topology.add_node(value);
      if (!is_ok(topology_status)) {
        return topology_status;
      }
      nodes.push_back(Node{.job = std::move(job)});
      return Status::OK;
    }

    /** Add one prerequisite-to-dependent edge while idle. */
    Status depend(const NodeType& prerequisite, const NodeType& dependent) {
      const std::lock_guard lock(mutex);
      if (state != RunState::IDLE) {
        return Status::EXECUTION_IN_PROGRESS;
      }
      return topology.add_dependency(prerequisite, dependent);
    }

    /** Validate the topology, initialize readiness, and schedule every root. */
    Status start() {
      std::vector<std::size_t> roots;
      std::uint64_t run_generation = 0U;
      std::size_t node_count       = 0U;
      {
        const std::lock_guard lock(mutex);
        if (state != RunState::IDLE) {
          return Status::EXECUTION_IN_PROGRESS;
        }
        if (!workers.active()) {
          detail::log_failure("start", Status::INVALID_ARGUMENT);
          return Status::INVALID_ARGUMENT;
        }
        DependencyGraphSnapshot<NodeType> topology_snapshot;
        const Status validation = topology.snapshot(topology_snapshot);
        if (!is_ok(validation)) {
          detail::log_failure("validate topology", validation);
          return validation;
        }

        ++generation;
        run_generation  = generation;
        run_status      = Status::OK;
        remaining_nodes = nodes.size();
        node_count      = nodes.size();
        running_jobs    = 0U;
        state           = RunState::RUNNING;
        for (std::size_t index = 0U; index < nodes.size(); ++index) {
          Node& node      = nodes[index];
          node.dependents = std::move(topology_snapshot.dependents[index]);
          node.dependency_count = topology_snapshot.dependency_counts[index];
          node.remaining_dependencies = node.dependency_count;
          if (node.remaining_dependencies == 0U) {
            roots.push_back(index);
          }
        }
        if (nodes.empty()) {
          state = RunState::COMPLETE;
          finished.notify_all();
        }
      }

      for (const std::size_t root : roots) {
        schedule(root, run_generation);
      }
      detail::log_transition("started", node_count);
      return Status::OK;
    }

    /** Wait for the active run, consume its status, and restore idle state. */
    Status wait() noexcept {
      if (current_graph() == this) {
        detail::log_failure("wait from graph job",
                            Status::EXECUTION_IN_PROGRESS);
        std::terminate();
      }
      std::unique_lock lock(mutex);
      if (state == RunState::IDLE) {
        return Status::OK;
      }
      finished.wait(lock, [this] { return state == RunState::COMPLETE; });
      const Status result = run_status;
      state               = RunState::IDLE;
      detail::log_transition("completed", nodes.size());
      return result;
    }

    /** Execute the caller's job and unlock newly ready successors. */
    void execute_node(std::size_t node_index,
                      std::uint64_t run_generation) noexcept {
      {
        const std::lock_guard lock(mutex);
        if (state != RunState::RUNNING || generation != run_generation ||
            node_index >= nodes.size()) {
          return;
        }
      }

      current_graph() = this;
      nodes[node_index].job->execute();
      current_graph() = nullptr;
      complete(node_index, run_generation);
    }

    /** Return whether a run is active or awaiting result collection. */
    bool active() const noexcept {
      const std::lock_guard lock(mutex);
      return state != RunState::IDLE;
    }

    /** Return the number of registered nodes. */
    std::size_t size() const noexcept {
      const std::lock_guard lock(mutex);
      return topology.size();
    }

    /** Return the number of registered directed edges. */
    std::size_t dependencies() const noexcept {
      const std::lock_guard lock(mutex);
      return topology.dependency_count();
    }

    /** Return the configured worker count. */
    std::size_t worker_count() const noexcept { return workers.worker_count(); }

   private:
    /** Submit one ready node, converting queue rejection into run failure. */
    void schedule(std::size_t node_index,
                  std::uint64_t run_generation) noexcept {
      {
        const std::lock_guard lock(mutex);
        if (state != RunState::RUNNING || generation != run_generation ||
            !is_ok(run_status)) {
          return;
        }
        ++running_jobs;
      }

      const multithreading::Status submission =
          workers.add_urgent(std::make_shared<CompletionJob>(
              this->shared_from_this(), node_index, run_generation));
      if (multithreading::is_ok(submission)) {
        return;
      }

      const std::lock_guard lock(mutex);
      --running_jobs;
      fail_run_locked(Status::WORKER_SUBMISSION_FAILED);
    }

    /** Complete one node and submit every successor it newly unblocks. */
    void complete(std::size_t node_index,
                  std::uint64_t run_generation) noexcept {
      std::vector<std::size_t> ready;
      bool notify = false;
      {
        const std::lock_guard lock(mutex);
        if (state != RunState::RUNNING || generation != run_generation) {
          return;
        }
        --running_jobs;
        if (!is_ok(run_status)) {
          if (running_jobs == 0U) {
            state  = RunState::COMPLETE;
            notify = true;
          }
        } else {
          --remaining_nodes;
          for (const std::size_t dependent : nodes[node_index].dependents) {
            Node& next = nodes[dependent];
            --next.remaining_dependencies;
            if (next.remaining_dependencies == 0U) {
              ready.push_back(dependent);
            }
          }
          if (remaining_nodes == 0U) {
            state  = RunState::COMPLETE;
            notify = true;
          }
        }
      }

      for (const std::size_t dependent : ready) {
        schedule(dependent, run_generation);
      }
      if (notify) {
        finished.notify_all();
      }
    }

    /** Stop successor submission and finish after already-running jobs exit. */
    void fail_run_locked(Status status) noexcept {
      if (is_ok(run_status)) {
        run_status = status;
        detail::log_failure("submit ready node", status);
      }
      if (running_jobs == 0U) {
        state = RunState::COMPLETE;
        finished.notify_all();
      }
    }

    /** Identify a graph currently executing on this thread. */
    static Impl*& current_graph() noexcept {
      static thread_local Impl* graph = nullptr;
      return graph;
    }

   private:
    multithreading::JobQueue& workers; /**< Borrowed caller-owned executor. */
    mutable std::mutex mutex; /**< Protects topology and all run state. */
    std::condition_variable finished; /**< Signals a collectable run result. */
    DependencyGraph<NodeType> topology; /**< Shared validated DAG mechanism. */
    std::vector<Node> nodes;            /**< Dense stable node storage. */
    RunState state    = RunState::IDLE; /**< Current lifecycle state. */
    Status run_status = Status::OK;     /**< Result of the active run. */
    std::size_t remaining_nodes = 0U;   /**< Nodes not completed this run. */
    std::size_t running_jobs    = 0U;   /**< Wrappers executing or queued. */
    std::uint64_t generation    = 0U; /**< Rejects stale wrapper completion. */
  };

 public:
  /**
   * Construct an empty reusable graph over one caller-owned worker pool.
   *
   * The pool must remain alive and accepting work until this graph has no
   * active run and has been destroyed. ExecutionGraph never stops or joins it.
   */
  explicit ExecutionGraph(multithreading::JobQueue& workers)
      : impl_(std::make_shared<Impl>(workers)) {}

  ExecutionGraph(const ExecutionGraph&)            = delete;
  ExecutionGraph& operator=(const ExecutionGraph&) = delete;
  ExecutionGraph(ExecutionGraph&&)                 = delete;
  ExecutionGraph& operator=(ExecutionGraph&&)      = delete;

  /** Wait for any active run before releasing the graph facade. */
  ~ExecutionGraph() { static_cast<void>(impl_->wait()); }

  /**
   * Register one uniquely identified job node.
   *
   * Heterogeneous concrete Job types may coexist in one graph because the
   * graph stores the multithreading::Job interface after compile-time
   * derivation checking.
   */
  template <ExecutionGraphJob JobType>
  Status add_node(NodeType value, std::shared_ptr<JobType> job) {
    return impl_->add(
        std::move(value),
        std::static_pointer_cast<multithreading::Job>(std::move(job)));
  }

  /** Declare that `dependent` may start only after `prerequisite` completes. */
  Status add_dependency(const NodeType& prerequisite,
                        const NodeType& dependent) {
    return impl_->depend(prerequisite, dependent);
  }

  /** Validate the current DAG and asynchronously schedule all ready roots. */
  Status start() { return impl_->start(); }

  /** Wait for and consume the active run result, restoring reusable idle state.
   */
  Status wait() noexcept { return impl_->wait(); }

  /** Return whether a run is active or has an unconsumed result. */
  bool active() const noexcept { return impl_->active(); }

  /** Return the number of registered nodes. */
  std::size_t size() const noexcept { return impl_->size(); }

  /** Return the number of registered directed dependency edges. */
  std::size_t dependency_count() const noexcept {
    return impl_->dependencies();
  }

  /** Return the fixed size of the borrowed worker pool. */
  std::size_t worker_count() const noexcept { return impl_->worker_count(); }

 private:
  std::shared_ptr<Impl> impl_; /**< State shared with scheduled wrappers. */
};

}  // namespace puc::execution_graph
