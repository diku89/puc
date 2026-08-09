#pragma once

/**
 * @file job_queue.hpp
 * @brief Fixed worker pool for immediate, delayed, and periodic jobs.
 */

#include <atomic>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "utils/multithreading/status.hpp"

namespace puc::multithreading {

namespace detail {
struct PeriodicJobState;
}  // namespace detail

/** Unit of no-throw work owned by JobQueue through shared ownership. */
class Job {
 public:
  /** Construct a polymorphic job. */
  Job() = default;

  Job(const Job&)            = delete;
  Job& operator=(const Job&) = delete;

  /** Destroy a job after it is no longer queued or executing. */
  virtual ~Job() = default;

  /** Execute one invocation; escaping failures terminate the process. */
  virtual void execute() noexcept = 0;
};

/**
 * Move-only cancellation authority for one periodic JobQueue submission.
 *
 * Destruction and `cancel()` prevent later invocations and rescheduling. A job
 * that already began may finish concurrently, so resources captured by the
 * job must remain valid through that invocation. Cancellation is idempotent
 * and never stops the caller-owned worker pool.
 */
class PeriodicJobHandle {
 public:
  /** Construct an inactive handle. */
  PeriodicJobHandle() noexcept = default;

  PeriodicJobHandle(const PeriodicJobHandle&)            = delete;
  PeriodicJobHandle& operator=(const PeriodicJobHandle&) = delete;

  /** Transfer cancellation authority without changing job state. */
  PeriodicJobHandle(PeriodicJobHandle&& other) noexcept;

  /** Cancel any current job, then take authority from `other`. */
  PeriodicJobHandle& operator=(PeriodicJobHandle&& other) noexcept;

  /** Cancel the periodic job if it remains active. */
  ~PeriodicJobHandle();

  /** Prevent future invocations and make this handle inactive. */
  void cancel() noexcept;

  /** Return whether this handle owns a periodic job that may run again. */
  bool active() const noexcept;

 private:
  friend class JobQueue;

  /** Adopt one newly scheduled periodic job. */
  explicit PeriodicJobHandle(
      std::shared_ptr<detail::PeriodicJobState> state) noexcept;

  std::shared_ptr<detail::PeriodicJobState>
      state_; /**< Shared cancellation state observed by workers. */
};

/**
 * Own a fixed set of worker threads and a synchronized job scheduler.
 *
 * Immediate jobs are FIFO. One-shot delayed jobs execute no earlier than their
 * delay; equal deadlines retain enqueue order. Periodic jobs first execute
 * after one full period and are rescheduled one period after the preceding
 * invocation finishes, so one periodic Job never overlaps itself even when
 * several workers are available. Each periodic submission returns an
 * independent cancellation handle and never owns the worker pool.
 *
 * `shutdown()` discards work that has not started and allows running jobs to
 * finish. `wait()` performs shutdown itself and joins every worker, making it
 * the safe one-call teardown operation. Destroying or waiting for a queue from
 * one of its own jobs is an invariant violation and terminates the process.
 */
class JobQueue {
 public:
  /** Construct and start the requested number of workers. Zero becomes one. */
  explicit JobQueue(std::uint8_t worker_count = 1U);

  JobQueue(const JobQueue&)            = delete;
  JobQueue& operator=(const JobQueue&) = delete;
  JobQueue(JobQueue&&)                 = delete;
  JobQueue& operator=(JobQueue&&)      = delete;

  /** Shut down and join every worker. */
  ~JobQueue();

  /** Schedule one job no earlier than `delay_ms` from now. */
  template <std::derived_from<Job> JobType>
  Status add_delayed(std::uint64_t delay_ms, std::shared_ptr<JobType> job) {
    return add_delayed_job(delay_ms,
                           std::static_pointer_cast<Job>(std::move(job)));
  }

  /**
   * Schedule a non-overlapping fixed-delay periodic job.
   *
   * The first invocation occurs after `period_ms`. On success, `handle`
   * replaces and cancels any periodic submission it previously controlled.
   * A zero period is rejected.
   */
  template <std::derived_from<Job> JobType>
  Status add_periodic(std::uint64_t period_ms, std::shared_ptr<JobType> job,
                      PeriodicJobHandle& handle) {
    return add_periodic_job(
        period_ms, std::static_pointer_cast<Job>(std::move(job)), handle);
  }

  /** Enqueue a one-shot FIFO job that is ready immediately. */
  template <std::derived_from<Job> JobType>
  Status add_urgent(std::shared_ptr<JobType> job) {
    return add_urgent_job(std::static_pointer_cast<Job>(std::move(job)));
  }

  /** Stop accepting work, discard queued jobs, and wake every worker. */
  void shutdown() noexcept;

  /** Shut down and join every worker; safe to call repeatedly. */
  void wait() noexcept;

  /** Return whether the queue still accepts jobs. */
  bool active() const;

  /** Return the number of queued jobs that have not begun execution. */
  std::size_t pending_jobs() const;

  /** Return the fixed number of worker threads owned by this queue. */
  std::size_t worker_count() const noexcept { return workers_.size(); }

 private:
  using Clock = std::chrono::steady_clock;

  /** Scheduled job and deterministic tie-breaking sequence. */
  struct JobEntry {
    Clock::time_point next_run;
    std::chrono::milliseconds period;
    std::uint64_t sequence = 0U;
    std::shared_ptr<Job> job;
    std::shared_ptr<detail::PeriodicJobState>
        periodic_state; /**< Present only for cancellable periodic work. */

    /** Order the earliest deadline and oldest sequence at the queue top. */
    bool operator>(const JobEntry& other) const noexcept {
      if (next_run != other.next_run) {
        return next_run > other.next_run;
      }
      return sequence > other.sequence;
    }
  };

  /** Type-erased implementation of add_delayed(). */
  Status add_delayed_job(std::uint64_t delay_ms, std::shared_ptr<Job> job);

  /** Type-erased implementation of add_periodic(). */
  Status add_periodic_job(std::uint64_t period_ms, std::shared_ptr<Job> job,
                          PeriodicJobHandle& handle);

  /** Type-erased implementation of add_urgent(). */
  Status add_urgent_job(std::shared_ptr<Job> job);

  /** Take ready work, execute it, and reschedule periodic invocations. */
  void worker() noexcept;

  /** Return whether the calling thread is one of this queue's workers. */
  bool is_worker_thread() const noexcept;

  /** Queue currently executing on this thread, if any. */
  static thread_local const JobQueue* current_queue_;

  std::priority_queue<JobEntry, std::vector<JobEntry>, std::greater<JobEntry>>
      jobs_; /**< Delayed and periodic work ordered by deadline. */
  std::deque<JobEntry> urgent_jobs_; /**< Immediate FIFO work. */
  std::vector<std::thread> workers_; /**< Fixed worker set. */
  mutable std::mutex jobs_mutex_; /**< Protects queues, active_, sequence_. */
  std::mutex
      wait_mutex_; /**< Ensures workers are joined only once at a time. */
  std::condition_variable signal_; /**< Wakes workers for jobs or shutdown. */
  bool active_                 = true; /**< Whether new work is accepted. */
  std::uint64_t next_sequence_ = 0U;   /**< Next deterministic enqueue order. */
};

}  // namespace puc::multithreading
