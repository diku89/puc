#pragma once

/**
 * @file execution_plan.hpp
 * @brief Immutable dependency topology supporting concurrent independent runs.
 */

#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "utils/execution_graph/dependency_graph.hpp"
#include "utils/multithreading/job_queue.hpp"

namespace puc::execution_graph {

/**
 * Execute many independent job sets against one validated immutable DAG.
 *
 * @tparam NodeType Copyable, equality-comparable, hashable graph identity.
 *
 * A plan contains topology only. Each `submit()` supplies one job per node and
 * creates private readiness state for that run. Runs may therefore overlap on
 * the same worker pool without sharing dependency counters or job payloads.
 * The completion callback runs exactly once after every node completes or the
 * worker pool rejects newly ready work.
 */
template <DependencyGraphNode NodeType>
class ExecutionPlan final {
 public:
  /** Callback invoked once with the final status of an accepted run. */
  using Completion = std::function<void(Status)>;

  /** Construct an empty, invalid plan suitable as compile() output. */
  ExecutionPlan() noexcept = default;

  /**
   * Validate and snapshot a topology into an immutable reusable plan.
   *
   * The output is cleared on failure.
   */
  static Status compile(const DependencyGraph<NodeType>& topology,
                        ExecutionPlan& output) {
    DependencyGraphSnapshot<NodeType> snapshot;
    const Status status = topology.snapshot(snapshot);
    if (!is_ok(status)) {
      output.data_.reset();
      return status;
    }
    output.data_ = std::make_shared<const Data>(std::move(snapshot));
    return Status::OK;
  }

  /**
   * Schedule one independent run using jobs in topology registration order.
   *
   * Returning OK transfers completion responsibility to the plan. A rejected
   * submission does not invoke `completion`.
   */
  Status submit(multithreading::JobQueue& workers,
                std::vector<std::shared_ptr<multithreading::Job>> jobs,
                Completion completion) const {
    if (data_ == nullptr || data_->nodes.empty() ||
        jobs.size() != data_->nodes.size() || !completion ||
        !workers.active()) {
      return Status::INVALID_ARGUMENT;
    }
    for (const std::shared_ptr<multithreading::Job>& job : jobs) {
      if (job == nullptr) {
        return Status::INVALID_ARGUMENT;
      }
    }

    auto run = std::make_shared<Run>(workers, data_, std::move(jobs),
                                     std::move(completion));
    run->start();
    return Status::OK;
  }

  /** Return whether this facade owns a compiled non-empty topology. */
  bool valid() const noexcept {
    return data_ != nullptr && !data_->nodes.empty();
  }

  /** Return the number of nodes in the immutable topology. */
  std::size_t size() const noexcept {
    return data_ == nullptr ? 0U : data_->nodes.size();
  }

  /** Return node identities in stable topology registration order. */
  const std::vector<NodeType>& nodes() const noexcept {
    static const std::vector<NodeType> empty;
    return data_ == nullptr ? empty : data_->nodes;
  }

 private:
  /** Immutable dense topology retained by every accepted run. */
  struct Data {
    explicit Data(DependencyGraphSnapshot<NodeType> snapshot)
        : nodes(std::move(snapshot.nodes)),
          dependents(std::move(snapshot.dependents)),
          dependency_counts(std::move(snapshot.dependency_counts)) {
      roots.reserve(dependency_counts.size());
      for (std::size_t index = 0U; index < dependency_counts.size(); ++index) {
        if (dependency_counts[index] == 0U) {
          roots.push_back(index);
        }
      }
    }

    std::vector<NodeType> nodes; /**< Stable node identities. */
    std::vector<std::vector<std::size_t>> dependents; /**< Outgoing edges. */
    std::vector<std::size_t> dependency_counts; /**< Static incoming counts. */
    std::vector<std::size_t> roots; /**< Zero-dependency nodes to publish. */
  };

  class Run;

  /** Worker wrapper retaining one independent run until node completion. */
  class RunJob final : public multithreading::Job {
   public:
    RunJob(std::shared_ptr<Run> run, std::size_t node_index)
        : run_(std::move(run)), node_index_(node_index) {}

    /** Execute one run-local node and release its newly ready dependents. */
    void execute() noexcept override { run_->execute(node_index_); }

   private:
    std::shared_ptr<Run> run_; /**< Run retained through this invocation. */
    std::size_t node_index_;   /**< Dense job/topology index. */
  };

  /** Private readiness counters and completion state for one submission. */
  class Run final : public std::enable_shared_from_this<Run> {
   public:
    Run(multithreading::JobQueue& configured_workers,
        std::shared_ptr<const Data> configured_data,
        std::vector<std::shared_ptr<multithreading::Job>> configured_jobs,
        Completion configured_completion)
        : workers_(configured_workers),
          data_(std::move(configured_data)),
          jobs_(std::move(configured_jobs)),
          completion_(std::move(configured_completion)),
          remaining_dependencies_(data_->dependency_counts),
          remaining_nodes_(data_->nodes.size()) {}

    /** Schedule every root in this run's immutable topology. */
    void start() noexcept {
      // The first submission publishes this Run to worker threads, so startup
      // reads only immutable compiled topology and never run-local readiness.
      for (const std::size_t root : data_->roots) {
        schedule(root);
      }
    }

    /** Execute one caller job, then complete its logical graph node. */
    void execute(std::size_t node_index) noexcept {
      jobs_[node_index]->execute();
      complete(node_index);
    }

   private:
    /** Submit one ready node or fail after every accepted wrapper exits. */
    void schedule(std::size_t node_index) noexcept {
      {
        const std::lock_guard lock(mutex_);
        if (finished_ || !is_ok(status_)) {
          return;
        }
        ++running_jobs_;
      }
      const multithreading::Status submitted = workers_.add_urgent(
          std::make_shared<RunJob>(this->shared_from_this(), node_index));
      if (multithreading::is_ok(submitted)) {
        return;
      }

      Completion completion;
      Status status = Status::OK;
      {
        const std::lock_guard lock(mutex_);
        --running_jobs_;
        if (is_ok(status_)) {
          status_ = Status::WORKER_SUBMISSION_FAILED;
        }
        take_completion_locked(completion, status);
      }
      invoke(std::move(completion), status);
    }

    /** Complete one node and submit every successor it newly unblocks. */
    void complete(std::size_t node_index) noexcept {
      std::vector<std::size_t> ready;
      Completion completion;
      Status status = Status::OK;
      {
        const std::lock_guard lock(mutex_);
        --running_jobs_;
        if (is_ok(status_)) {
          --remaining_nodes_;
          for (const std::size_t dependent : data_->dependents[node_index]) {
            --remaining_dependencies_[dependent];
            if (remaining_dependencies_[dependent] == 0U) {
              ready.push_back(dependent);
            }
          }
        }
      }

      for (const std::size_t dependent : ready) {
        schedule(dependent);
      }
      {
        const std::lock_guard lock(mutex_);
        take_completion_locked(completion, status);
      }
      invoke(std::move(completion), status);
    }

    /** Move out completion when success or failure has fully quiesced. */
    void take_completion_locked(Completion& completion, Status& status) {
      if (finished_ || running_jobs_ != 0U ||
          (is_ok(status_) && remaining_nodes_ != 0U)) {
        return;
      }
      finished_  = true;
      status     = status_;
      completion = std::move(completion_);
    }

    /** Invoke one no-throw completion boundary outside the run lock. */
    static void invoke(Completion completion, Status status) noexcept {
      if (!completion) {
        return;
      }
      try {
        completion(status);
      } catch (...) {
      }
    }

    multithreading::JobQueue& workers_; /**< Borrowed active executor. */
    std::shared_ptr<const Data> data_;  /**< Retained immutable topology. */
    std::vector<std::shared_ptr<multithreading::Job>> jobs_; /**< Run jobs. */
    Completion completion_; /**< Run result callback moved on completion. */
    std::mutex mutex_;      /**< Protects all readiness and result state. */
    std::vector<std::size_t> remaining_dependencies_; /**< Run counters. */
    std::size_t remaining_nodes_;   /**< Logical nodes not yet completed. */
    std::size_t running_jobs_ = 0U; /**< Accepted wrappers not yet exited. */
    Status status_            = Status::OK; /**< First scheduling failure. */
    bool finished_            = false; /**< Whether callback was consumed. */
  };

  std::shared_ptr<const Data> data_; /**< Compiled immutable topology. */
};

}  // namespace puc::execution_graph
