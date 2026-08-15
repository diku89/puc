/**
 * @file execution_graph_test.cpp
 * @brief Unit tests for reusable dependency-aware job execution.
 */

#include "utils/execution_graph/execution_graph.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "utils/execution_graph/execution_plan.hpp"

namespace puc::execution_graph {
namespace {

using namespace std::chrono_literals;

/** Job double whose behavior is supplied by one callback. */
class CallbackJob final : public multithreading::Job {
 public:
  explicit CallbackJob(std::function<void()> callback)
      : callback_(std::move(callback)) {}

  void execute() noexcept override { callback_(); }

 private:
  std::function<void()> callback_; /**< Work performed by execute(). */
};

/** A second concrete Job type used to verify heterogeneous graph nodes. */
class CountingJob final : public multithreading::Job {
 public:
  explicit CountingJob(std::atomic<std::size_t>& count) : count_(&count) {}

  void execute() noexcept override { count_->fetch_add(1U); }

 private:
  std::atomic<std::size_t>* count_; /**< Counter incremented per invocation. */
};

/** Condition-variable gate used to make scheduling assertions deterministic. */
class Gate {
 public:
  /** Block until open() is called. */
  void wait() {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] { return open_; });
  }

  /** Release every current and future waiter. */
  void open() {
    {
      const std::lock_guard lock(mutex_);
      open_ = true;
    }
    changed_.notify_all();
  }

 private:
  std::mutex mutex_;                /**< Protects open_. */
  std::condition_variable changed_; /**< Wakes blocked jobs. */
  bool open_ = false;               /**< Whether waiters may proceed. */
};

/** Synchronized counter with a bounded wait for test observations. */
class Counter {
 public:
  /** Increment the counter and wake observers. */
  void increment() {
    {
      const std::lock_guard lock(mutex_);
      ++value_;
    }
    changed_.notify_all();
  }

  /** Wait until at least expected increments have occurred. */
  bool wait_for(std::size_t expected, std::chrono::milliseconds timeout = 2s) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, timeout,
                             [this, expected] { return value_ >= expected; });
  }

  /** Return the current value. */
  std::size_t value() const {
    const std::lock_guard lock(mutex_);
    return value_;
  }

 private:
  mutable std::mutex mutex_;        /**< Protects value_. */
  std::condition_variable changed_; /**< Wakes observers. */
  std::size_t value_ = 0U;          /**< Number of observed increments. */
};

/** Shared completion state retained through an asynchronous plan callback. */
struct PlanCompletion final {
  std::mutex mutex;                /**< Protects complete and result. */
  std::condition_variable changed; /**< Signals callback completion. */
  bool complete = false;           /**< Whether the callback has run. */
  Status result = Status::INVALID_ARGUMENT; /**< Accepted run result. */
};

static_assert(ExecutionGraphNode<std::string>);
static_assert(ExecutionGraphJob<CallbackJob>);
static_assert(ExecutionGraphJob<CountingJob>);
static_assert(!ExecutionGraphNode<std::unique_ptr<int>>);
static_assert(!ExecutionGraphJob<int>);

TEST(ExecutionGraphStatusTest, ReportsStableHumanReadableResults) {
  EXPECT_TRUE(is_ok(Status::OK));
  EXPECT_FALSE(is_ok(Status::INVALID_ARGUMENT));
  EXPECT_EQ(status_message(Status::OK), "success");
  EXPECT_EQ(status_message(Status::INVALID_ARGUMENT),
            "worker pool must be active and node jobs must be present");
  EXPECT_EQ(status_message(Status::DUPLICATE_NODE),
            "execution graph node already exists");
  EXPECT_EQ(status_message(Status::NODE_NOT_FOUND),
            "execution graph node was not found");
  EXPECT_EQ(status_message(Status::DUPLICATE_DEPENDENCY),
            "execution graph dependency already exists");
  EXPECT_EQ(status_message(Status::DEPENDENCY_CYCLE),
            "execution graph contains a dependency cycle");
  EXPECT_EQ(status_message(Status::EXECUTION_IN_PROGRESS),
            "execution graph run is active or awaiting collection");
  EXPECT_EQ(status_message(Status::WORKER_SUBMISSION_FAILED),
            "worker pool rejected an execution graph node");
  EXPECT_EQ(status_message(static_cast<Status>(-1)),
            "unknown execution graph status");
}

TEST(ExecutionGraphTest, RegistersTypedJobsAndValidatesEdges) {
  multithreading::JobQueue workers(2U);
  ExecutionGraph<std::string> graph(workers);
  std::atomic<std::size_t> count{0U};

  EXPECT_EQ(graph.worker_count(), 2U);
  EXPECT_EQ(graph.size(), 0U);
  EXPECT_EQ(graph.dependency_count(), 0U);
  EXPECT_EQ(graph.add_node("first", std::make_shared<CountingJob>(count)),
            Status::OK);
  EXPECT_EQ(graph.add_node("second", std::make_shared<CallbackJob>(
                                         [&] { count.fetch_add(1U); })),
            Status::OK);
  EXPECT_EQ(graph.add_node("first", std::make_shared<CountingJob>(count)),
            Status::DUPLICATE_NODE);
  std::shared_ptr<CountingJob> null_job;
  EXPECT_EQ(graph.add_node("null", null_job), Status::INVALID_ARGUMENT);
  EXPECT_EQ(graph.add_dependency("missing", "first"), Status::NODE_NOT_FOUND);
  EXPECT_EQ(graph.add_dependency("first", "missing"), Status::NODE_NOT_FOUND);
  EXPECT_EQ(graph.add_dependency("first", "second"), Status::OK);
  EXPECT_EQ(graph.add_dependency("first", "second"),
            Status::DUPLICATE_DEPENDENCY);
  EXPECT_EQ(graph.size(), 2U);
  EXPECT_EQ(graph.dependency_count(), 1U);

  ASSERT_EQ(graph.start(), Status::OK);
  EXPECT_EQ(graph.wait(), Status::OK);
  EXPECT_EQ(count.load(), 2U);
}

TEST(ExecutionGraphTest, NeverStopsItsBorrowedWorkerPool) {
  multithreading::JobQueue workers(2U);
  {
    ExecutionGraph<int> graph(workers);
    std::atomic<std::size_t> count{0U};
    ASSERT_EQ(graph.add_node(1, std::make_shared<CountingJob>(count)),
              Status::OK);
    ASSERT_EQ(graph.start(), Status::OK);
    ASSERT_EQ(graph.wait(), Status::OK);
    EXPECT_EQ(count.load(), 1U);
  }
  EXPECT_TRUE(workers.active());
}

TEST(ExecutionGraphTest, RunsEveryReadyBranchAndWaitsForAllPrerequisites) {
  multithreading::JobQueue workers(4U);
  ExecutionGraph<std::string> graph(workers);
  Gate root_gate;
  Gate branch_gate;
  Counter root_started;
  Counter branches_started;
  std::atomic<std::size_t> active_branches{0U};
  std::atomic<std::size_t> maximum_active_branches{0U};
  std::atomic<bool> leaf_started{false};

  ASSERT_EQ(graph.add_node("root", std::make_shared<CallbackJob>([&] {
                             root_started.increment();
                             root_gate.wait();
                           })),
            Status::OK);
  for (const std::string_view branch : {"left", "right"}) {
    ASSERT_EQ(
        graph.add_node(std::string{branch}, std::make_shared<CallbackJob>([&] {
                         const std::size_t active =
                             active_branches.fetch_add(1U) + 1U;
                         std::size_t maximum = maximum_active_branches.load();
                         while (active > maximum &&
                                !maximum_active_branches.compare_exchange_weak(
                                    maximum, active)) {
                         }
                         branches_started.increment();
                         branch_gate.wait();
                         active_branches.fetch_sub(1U);
                       })),
        Status::OK);
  }
  ASSERT_EQ(graph.add_node("leaf", std::make_shared<CallbackJob>(
                                       [&] { leaf_started.store(true); })),
            Status::OK);
  ASSERT_EQ(graph.add_dependency("root", "left"), Status::OK);
  ASSERT_EQ(graph.add_dependency("root", "right"), Status::OK);
  ASSERT_EQ(graph.add_dependency("left", "leaf"), Status::OK);
  ASSERT_EQ(graph.add_dependency("right", "leaf"), Status::OK);

  ASSERT_EQ(graph.start(), Status::OK);
  ASSERT_TRUE(root_started.wait_for(1U));
  EXPECT_EQ(branches_started.value(), 0U);
  EXPECT_FALSE(leaf_started.load());

  root_gate.open();
  ASSERT_TRUE(branches_started.wait_for(2U));
  EXPECT_EQ(maximum_active_branches.load(), 2U);
  EXPECT_FALSE(leaf_started.load());

  branch_gate.open();
  EXPECT_EQ(graph.wait(), Status::OK);
  EXPECT_TRUE(leaf_started.load());
  EXPECT_FALSE(graph.active());
}

TEST(ExecutionGraphTest, RejectsCyclesBeforeExecutingAnyJob) {
  multithreading::JobQueue workers(2U);
  ExecutionGraph<int> graph(workers);
  std::atomic<std::size_t> count{0U};
  const auto job = std::make_shared<CountingJob>(count);

  ASSERT_EQ(graph.add_node(1, job), Status::OK);
  ASSERT_EQ(graph.add_node(2, job), Status::OK);
  ASSERT_EQ(graph.add_node(3, job), Status::OK);
  ASSERT_EQ(graph.add_dependency(1, 2), Status::OK);
  ASSERT_EQ(graph.add_dependency(2, 3), Status::OK);
  ASSERT_EQ(graph.add_dependency(3, 1), Status::OK);

  EXPECT_EQ(graph.start(), Status::DEPENDENCY_CYCLE);
  EXPECT_FALSE(graph.active());
  EXPECT_EQ(graph.wait(), Status::OK);
  EXPECT_EQ(count.load(), 0U);
}

TEST(ExecutionGraphTest, ReusesValidatedTopologyAndJobsAcrossRuns) {
  multithreading::JobQueue workers(2U);
  ExecutionGraph<int> graph(workers);
  std::atomic<std::size_t> count{0U};

  ASSERT_EQ(graph.add_node(1, std::make_shared<CountingJob>(count)),
            Status::OK);
  ASSERT_EQ(graph.add_node(2, std::make_shared<CountingJob>(count)),
            Status::OK);
  ASSERT_EQ(graph.add_dependency(1, 2), Status::OK);

  ASSERT_EQ(graph.start(), Status::OK);
  ASSERT_EQ(graph.wait(), Status::OK);
  ASSERT_EQ(graph.start(), Status::OK);
  ASSERT_EQ(graph.wait(), Status::OK);
  EXPECT_EQ(count.load(), 4U);
}

TEST(ExecutionGraphTest, ReportsAReadyNodeRejectedAfterWorkersStop) {
  multithreading::JobQueue workers(1U);
  ExecutionGraph<int> graph(workers);
  std::atomic<std::size_t> dependent_runs{0U};

  ASSERT_EQ(graph.add_node(1, std::make_shared<CallbackJob>(
                                  [&workers] { workers.shutdown(); })),
            Status::OK);
  ASSERT_EQ(graph.add_node(2, std::make_shared<CountingJob>(dependent_runs)),
            Status::OK);
  ASSERT_EQ(graph.add_dependency(1, 2), Status::OK);

  ASSERT_EQ(graph.start(), Status::OK);
  EXPECT_EQ(graph.wait(), Status::WORKER_SUBMISSION_FAILED);
  EXPECT_EQ(dependent_runs.load(), 0U);
}

TEST(ExecutionGraphTest, PreventsMutationAndRestartUntilWaitConsumesRun) {
  multithreading::JobQueue workers(2U);
  ExecutionGraph<int> graph(workers);
  Gate gate;
  Counter started;

  ASSERT_EQ(graph.add_node(1, std::make_shared<CallbackJob>([&] {
                             started.increment();
                             gate.wait();
                           })),
            Status::OK);
  ASSERT_EQ(graph.start(), Status::OK);
  ASSERT_TRUE(started.wait_for(1U));
  EXPECT_TRUE(graph.active());
  EXPECT_EQ(graph.start(), Status::EXECUTION_IN_PROGRESS);
  EXPECT_EQ(graph.add_node(2, std::make_shared<CallbackJob>([] {})),
            Status::EXECUTION_IN_PROGRESS);
  EXPECT_EQ(graph.add_dependency(1, 1), Status::EXECUTION_IN_PROGRESS);

  gate.open();
  EXPECT_EQ(graph.wait(), Status::OK);
  EXPECT_EQ(graph.add_node(2, std::make_shared<CallbackJob>([] {})),
            Status::OK);
}

TEST(ExecutionGraphTest, HandlesAnEmptyGraphAndStoppedWorkers) {
  multithreading::JobQueue stopped_workers(1U);
  stopped_workers.shutdown();
  ExecutionGraph<int> stopped_graph(stopped_workers);
  EXPECT_EQ(stopped_graph.start(), Status::INVALID_ARGUMENT);

  multithreading::JobQueue workers(1U);
  ExecutionGraph<int> empty_graph(workers);
  EXPECT_EQ(empty_graph.start(), Status::OK);
  EXPECT_TRUE(empty_graph.active());
  EXPECT_EQ(empty_graph.wait(), Status::OK);
  EXPECT_FALSE(empty_graph.active());
}

TEST(ExecutionPlanTest, PublishesAllReadinessBeforeFastJobsCanComplete) {
  constexpr std::size_t kNodes = 64U;
  constexpr std::size_t kRuns  = 32U;
  DependencyGraph<std::size_t> topology;
  for (std::size_t index = 0U; index < kNodes; ++index) {
    ASSERT_EQ(topology.add_node(index), Status::OK);
    if (index > 0U) {
      ASSERT_EQ(topology.add_dependency(index - 1U, index), Status::OK);
    }
  }
  ExecutionPlan<std::size_t> plan;
  ASSERT_EQ(ExecutionPlan<std::size_t>::compile(topology, plan), Status::OK);

  multithreading::JobQueue workers(4U);
  std::atomic<std::size_t> count{0U};
  for (std::size_t run = 0U; run < kRuns; ++run) {
    std::vector<std::shared_ptr<multithreading::Job>> jobs;
    jobs.reserve(kNodes);
    for (std::size_t index = 0U; index < kNodes; ++index) {
      jobs.push_back(std::make_shared<CountingJob>(count));
    }

    auto completion = std::make_shared<PlanCompletion>();
    const Status accepted =
        plan.submit(workers, std::move(jobs), [completion](Status status) {
          {
            const std::lock_guard lock(completion->mutex);
            completion->result   = status;
            completion->complete = true;
          }
          completion->changed.notify_all();
        });
    ASSERT_EQ(accepted, Status::OK);
    std::unique_lock lock(completion->mutex);
    ASSERT_TRUE(completion->changed.wait_for(
        lock, 2s, [&completion] { return completion->complete; }));
    EXPECT_EQ(completion->result, Status::OK);
  }
  EXPECT_EQ(count.load(), kNodes * kRuns);
}

}  // namespace
}  // namespace puc::execution_graph
