/**
 * @file job_queue_test.cpp
 * @brief Unit tests for asynchronous job scheduling and teardown.
 */

#include "utils/multithreading/job_queue.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

namespace puc::multithreading {
namespace {

using namespace std::chrono_literals;

class CallbackJob final : public Job {
 public:
  explicit CallbackJob(std::function<void()> callback)
      : callback_(std::move(callback)) {}

  void execute() noexcept override { callback_(); }

 private:
  std::function<void()> callback_;
};

class Counter {
 public:
  void increment() {
    {
      const std::lock_guard lock(mutex_);
      ++value_;
    }
    changed_.notify_all();
  }

  bool wait_for(std::size_t expected, std::chrono::milliseconds timeout = 2s) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, timeout,
                             [this, expected] { return value_ >= expected; });
  }

  std::size_t value() const {
    const std::lock_guard lock(mutex_);
    return value_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::size_t value_ = 0U;
};

TEST(StatusTest, ReportsStableHumanReadableResults) {
  EXPECT_TRUE(is_ok(Status::OK));
  EXPECT_FALSE(is_ok(Status::INVALID_ARGUMENT));
  EXPECT_FALSE(is_ok(Status::INVALID_PERIOD));
  EXPECT_FALSE(is_ok(Status::DELAY_OUT_OF_RANGE));
  EXPECT_FALSE(is_ok(Status::QUEUE_STOPPED));
  EXPECT_EQ(status_message(Status::OK), "success");
  EXPECT_EQ(status_message(Status::INVALID_ARGUMENT),
            "job scheduling argument is invalid");
  EXPECT_EQ(status_message(Status::INVALID_PERIOD),
            "periodic job period must be positive");
  EXPECT_EQ(status_message(Status::DELAY_OUT_OF_RANGE),
            "job delay is outside the supported range");
  EXPECT_EQ(status_message(Status::QUEUE_STOPPED), "job queue is stopped");
  EXPECT_EQ(status_message(static_cast<Status>(-1)),
            "unknown job queue status");
}

TEST(JobQueueTest, CorrectsZeroWorkersToOne) {
  JobQueue queue(0U);
  EXPECT_EQ(queue.worker_count(), 1U);
  EXPECT_TRUE(queue.active());
}

TEST(JobQueueTest, RunsImmediateJobsInFifoOrderWithOneWorker) {
  JobQueue queue;
  Counter completed;
  std::mutex order_mutex;
  std::vector<int> order;

  for (int value = 0; value < 4; ++value) {
    const auto job = std::make_shared<CallbackJob>([&, value] {
      {
        const std::lock_guard lock(order_mutex);
        order.push_back(value);
      }
      completed.increment();
    });
    ASSERT_EQ(queue.add_urgent(job), Status::OK);
  }

  ASSERT_TRUE(completed.wait_for(4U));
  queue.wait();
  EXPECT_EQ(order, (std::vector<int>{0, 1, 2, 3}));
}

TEST(JobQueueTest, ZeroDelayScheduledJobRunsExactlyOnce) {
  JobQueue queue;
  Counter completed;
  ASSERT_EQ(queue.add_delayed(0U, std::make_shared<CallbackJob>(
                                      [&] { completed.increment(); })),
            Status::OK);

  ASSERT_TRUE(completed.wait_for(1U));
  std::this_thread::sleep_for(30ms);
  queue.wait();
  EXPECT_EQ(completed.value(), 1U);
}

TEST(JobQueueTest, DelayedJobDoesNotRunEarly) {
  JobQueue queue;
  Counter completed;
  const auto started = std::chrono::steady_clock::now();
  std::chrono::steady_clock::duration elapsed{};

  ASSERT_EQ(queue.add_delayed(50U, std::make_shared<CallbackJob>([&] {
                                elapsed =
                                    std::chrono::steady_clock::now() - started;
                                completed.increment();
                              })),
            Status::OK);

  ASSERT_TRUE(completed.wait_for(1U));
  queue.shutdown();
  queue.wait();
  EXPECT_GE(elapsed, 40ms);
}

TEST(JobQueueTest, ReadyImmediateJobPrecedesNotReadyDelayedJob) {
  JobQueue queue;
  Counter completed;
  std::atomic<int> observed{0};

  ASSERT_EQ(queue.add_delayed(1000U, std::make_shared<CallbackJob>(
                                         [&] { observed.store(2); })),
            Status::OK);
  ASSERT_EQ(queue.add_urgent(std::make_shared<CallbackJob>([&] {
    observed.store(1);
    completed.increment();
  })),
            Status::OK);

  ASSERT_TRUE(completed.wait_for(1U));
  queue.wait();
  EXPECT_EQ(observed.load(), 1);
}

TEST(JobQueueTest, PeriodicJobReschedulesAfterItsInvocationFinishes) {
  JobQueue queue(4U);
  Counter completed;
  std::atomic<int> executing{0};
  std::atomic<int> maximum_executing{0};

  const auto periodic = std::make_shared<CallbackJob>([&] {
    const int current = executing.fetch_add(1) + 1;
    int maximum       = maximum_executing.load();
    while (current > maximum &&
           !maximum_executing.compare_exchange_weak(maximum, current)) {
    }
    std::this_thread::sleep_for(15ms);
    executing.fetch_sub(1);
    completed.increment();
  });
  PeriodicJobHandle handle;
  ASSERT_EQ(queue.add_periodic(5U, periodic, handle), Status::OK);
  EXPECT_TRUE(handle.active());

  ASSERT_TRUE(completed.wait_for(3U));
  handle.cancel();
  EXPECT_FALSE(handle.active());
  queue.wait();
  EXPECT_EQ(maximum_executing.load(), 1);
  EXPECT_GE(completed.value(), 3U);
}

TEST(JobQueueTest, DelayedJobWithPositiveDelayRunsOnlyOnce) {
  JobQueue queue;
  Counter completed;
  ASSERT_EQ(queue.add_delayed(5U, std::make_shared<CallbackJob>(
                                      [&] { completed.increment(); })),
            Status::OK);

  ASSERT_TRUE(completed.wait_for(1U));
  std::this_thread::sleep_for(30ms);
  queue.wait();
  EXPECT_EQ(completed.value(), 1U);
}

TEST(JobQueueTest, CancellingPeriodicJobBeforeDeadlinePreventsInvocation) {
  JobQueue queue;
  Counter completed;
  PeriodicJobHandle handle;
  ASSERT_EQ(queue.add_periodic(100U, std::make_shared<CallbackJob>([&] {
                                 completed.increment();
                               }),
                               handle),
            Status::OK);

  handle.cancel();
  std::this_thread::sleep_for(130ms);
  queue.wait();
  EXPECT_EQ(completed.value(), 0U);
}

TEST(JobQueueTest, CancellationStopsPeriodicRescheduling) {
  JobQueue queue(2U);
  Counter completed;
  PeriodicJobHandle handle;
  ASSERT_EQ(queue.add_periodic(5U, std::make_shared<CallbackJob>([&] {
                                 completed.increment();
                               }),
                               handle),
            Status::OK);
  ASSERT_TRUE(completed.wait_for(2U));

  handle.cancel();
  std::this_thread::sleep_for(30ms);
  const std::size_t settled = completed.value();
  std::this_thread::sleep_for(30ms);
  queue.wait();
  EXPECT_EQ(completed.value(), settled);
}

TEST(JobQueueTest, ReplacingHandleCancelsItsPreviousPeriodicJob) {
  JobQueue queue(2U);
  Counter first;
  Counter second;
  PeriodicJobHandle handle;
  ASSERT_EQ(queue.add_periodic(
                5U, std::make_shared<CallbackJob>([&] { first.increment(); }),
                handle),
            Status::OK);
  ASSERT_TRUE(first.wait_for(1U));

  ASSERT_EQ(queue.add_periodic(
                5U, std::make_shared<CallbackJob>([&] { second.increment(); }),
                handle),
            Status::OK);
  ASSERT_TRUE(second.wait_for(2U));
  std::this_thread::sleep_for(30ms);
  const std::size_t first_settled = first.value();
  std::this_thread::sleep_for(30ms);
  handle.cancel();
  queue.wait();
  EXPECT_EQ(first.value(), first_settled);
}

TEST(JobQueueTest, ShutdownDeactivatesPeriodicHandles) {
  JobQueue queue;
  PeriodicJobHandle handle;
  ASSERT_EQ(
      queue.add_periodic(1000U, std::make_shared<CallbackJob>([] {}), handle),
      Status::OK);
  ASSERT_TRUE(handle.active());

  queue.wait();
  EXPECT_FALSE(handle.active());
}

TEST(JobQueueTest, AcceptsJobsFromConcurrentProducers) {
  constexpr std::size_t producer_count    = 4U;
  constexpr std::size_t jobs_per_producer = 50U;
  JobQueue queue(4U);
  Counter completed;
  std::atomic<bool> submission_failed{false};
  std::vector<std::thread> producers;

  for (std::size_t producer = 0U; producer < producer_count; ++producer) {
    producers.emplace_back([&] {
      for (std::size_t job_index = 0U; job_index < jobs_per_producer;
           ++job_index) {
        if (queue.add_urgent(std::make_shared<CallbackJob>(
                [&] { completed.increment(); })) != Status::OK) {
          submission_failed.store(true);
        }
      }
    });
  }
  for (std::thread& producer : producers) {
    producer.join();
  }

  ASSERT_TRUE(completed.wait_for(producer_count * jobs_per_producer));
  queue.wait();
  EXPECT_FALSE(submission_failed.load());
  EXPECT_EQ(completed.value(), producer_count * jobs_per_producer);
}

TEST(JobQueueTest, ShutdownDiscardsQueuedWorkAndLetsRunningWorkFinish) {
  JobQueue queue;
  std::mutex gate_mutex;
  std::condition_variable gate_changed;
  bool started = false;
  bool release = false;
  Counter completed;
  std::atomic<bool> queued_job_ran{false};

  ASSERT_EQ(queue.add_urgent(std::make_shared<CallbackJob>([&] {
    {
      std::unique_lock lock(gate_mutex);
      started = true;
      gate_changed.notify_all();
      gate_changed.wait(lock, [&] { return release; });
    }
    completed.increment();
  })),
            Status::OK);
  {
    std::unique_lock lock(gate_mutex);
    ASSERT_TRUE(gate_changed.wait_for(lock, 2s, [&] { return started; }));
  }
  ASSERT_EQ(queue.add_urgent(std::make_shared<CallbackJob>(
                [&] { queued_job_ran.store(true); })),
            Status::OK);

  queue.shutdown();
  {
    const std::lock_guard lock(gate_mutex);
    release = true;
  }
  gate_changed.notify_all();
  ASSERT_TRUE(completed.wait_for(1U));
  queue.wait();
  EXPECT_FALSE(queued_job_ran.load());
  EXPECT_FALSE(queue.active());
  EXPECT_EQ(queue.pending_jobs(), 0U);
}

TEST(JobQueueTest, RejectsInvalidAndPostShutdownSubmissions) {
  JobQueue queue;
  std::shared_ptr<CallbackJob> null_job;
  EXPECT_EQ(queue.add_urgent(null_job), Status::INVALID_ARGUMENT);
  EXPECT_EQ(queue.add_delayed(0U, null_job), Status::INVALID_ARGUMENT);
  PeriodicJobHandle handle;
  EXPECT_EQ(queue.add_periodic(1U, null_job, handle), Status::INVALID_ARGUMENT);
  EXPECT_EQ(
      queue.add_periodic(0U, std::make_shared<CallbackJob>([] {}), handle),
      Status::INVALID_PERIOD);

  queue.shutdown();
  const auto job = std::make_shared<CallbackJob>([] {});
  EXPECT_EQ(queue.add_urgent(job), Status::QUEUE_STOPPED);
  EXPECT_EQ(queue.add_delayed(0U, job), Status::QUEUE_STOPPED);
  EXPECT_EQ(queue.add_periodic(1U, job, handle), Status::QUEUE_STOPPED);
}

TEST(JobQueueTest, RejectsUnrepresentableDelay) {
  if constexpr (sizeof(std::chrono::milliseconds::rep) <
                sizeof(std::uint64_t)) {
    JobQueue queue;
    const auto job = std::make_shared<CallbackJob>([] {});
    EXPECT_EQ(queue.add_delayed(std::numeric_limits<std::uint64_t>::max(), job),
              Status::DELAY_OUT_OF_RANGE);
    PeriodicJobHandle handle;
    EXPECT_EQ(queue.add_periodic(std::numeric_limits<std::uint64_t>::max(), job,
                                 handle),
              Status::DELAY_OUT_OF_RANGE);
  }
}

TEST(JobQueueTest, WaitShutsDownJoinsAndIsIdempotent) {
  JobQueue queue(2U);
  queue.wait();
  queue.wait();
  EXPECT_FALSE(queue.active());
  EXPECT_EQ(queue.pending_jobs(), 0U);
  EXPECT_EQ(queue.add_urgent(std::make_shared<CallbackJob>([] {})),
            Status::QUEUE_STOPPED);
}

}  // namespace
}  // namespace puc::multithreading
