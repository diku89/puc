/**
 * @file job_queue.cpp
 * @brief Worker scheduling and deterministic queue teardown.
 */

#include "utils/multithreading/job_queue.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include "utils/logger/logger.hpp"

/** @cond JOB_QUEUE_LOGGER_MODULE */
LOGGER_MODULE("Job Queue");
/** @endcond */

namespace puc::multithreading {

namespace detail {

/** Cancellation bit shared by one handle and its queued/running entry. */
struct PeriodicJobState {
  std::atomic<bool> active =
      true; /**< Whether another invocation is allowed. */
};

}  // namespace detail

thread_local const JobQueue* JobQueue::current_queue_ = nullptr;

PeriodicJobHandle::PeriodicJobHandle(
    std::shared_ptr<detail::PeriodicJobState> state) noexcept
    : state_(std::move(state)) {}

PeriodicJobHandle::PeriodicJobHandle(PeriodicJobHandle&& other) noexcept
    : state_(std::move(other.state_)) {}

PeriodicJobHandle& PeriodicJobHandle::operator=(
    PeriodicJobHandle&& other) noexcept {
  if (this != &other) {
    cancel();
    state_ = std::move(other.state_);
  }
  return *this;
}

PeriodicJobHandle::~PeriodicJobHandle() { cancel(); }

void PeriodicJobHandle::cancel() noexcept {
  if (state_ != nullptr) {
    state_->active.store(false, std::memory_order_release);
    state_.reset();
  }
}

bool PeriodicJobHandle::active() const noexcept {
  return state_ != nullptr && state_->active.load(std::memory_order_acquire);
}

JobQueue::JobQueue(std::uint8_t worker_count) {
  if (worker_count == 0U) {
    worker_count = 1U;
    Logger<WARN> << "Corrected a zero-worker job queue to one worker";
  }
  workers_.reserve(worker_count);
  for (std::uint8_t index = 0U; index < worker_count; ++index) {
    workers_.emplace_back(&JobQueue::worker, this);
  }
  Logger<INFO> << "Started job queue with "
               << static_cast<unsigned int>(worker_count) << " worker(s)";
}

JobQueue::~JobQueue() { wait(); }

Status JobQueue::add_delayed_job(std::uint64_t delay_ms,
                                 std::shared_ptr<Job> job) {
  if (job == nullptr) {
    Logger<ERROR> << "Cannot schedule a null job";
    return Status::INVALID_ARGUMENT;
  }
  using MillisecondsRep = std::chrono::milliseconds::rep;
  if (delay_ms >
      static_cast<std::uint64_t>(std::numeric_limits<MillisecondsRep>::max())) {
    Logger<ERROR> << "Job delay exceeds steady-clock duration range";
    return Status::DELAY_OUT_OF_RANGE;
  }

  const auto delay =
      std::chrono::milliseconds{static_cast<MillisecondsRep>(delay_ms)};
  {
    const std::lock_guard lock(jobs_mutex_);
    if (!active_) {
      return Status::QUEUE_STOPPED;
    }
    jobs_.push(JobEntry{
        .next_run = Clock::now() + delay,
        .period   = std::chrono::milliseconds::zero(),
        .sequence = next_sequence_++,
        .job      = std::move(job),
    });
  }
  signal_.notify_one();
  return Status::OK;
}

Status JobQueue::add_periodic_job(std::uint64_t period_ms,
                                  std::shared_ptr<Job> job,
                                  PeriodicJobHandle& handle) {
  if (job == nullptr) {
    Logger<ERROR> << "Cannot schedule a null periodic job";
    return Status::INVALID_ARGUMENT;
  }
  if (period_ms == 0U) {
    Logger<ERROR> << "Cannot schedule a periodic job with a zero period";
    return Status::INVALID_PERIOD;
  }
  using MillisecondsRep = std::chrono::milliseconds::rep;
  if (period_ms >
      static_cast<std::uint64_t>(std::numeric_limits<MillisecondsRep>::max())) {
    Logger<ERROR> << "Periodic job period exceeds steady-clock duration range";
    return Status::DELAY_OUT_OF_RANGE;
  }

  const auto period =
      std::chrono::milliseconds{static_cast<MillisecondsRep>(period_ms)};
  auto state = std::make_shared<detail::PeriodicJobState>();
  {
    const std::lock_guard lock(jobs_mutex_);
    if (!active_) {
      return Status::QUEUE_STOPPED;
    }
    jobs_.push(JobEntry{
        .next_run       = Clock::now() + period,
        .period         = period,
        .sequence       = next_sequence_++,
        .job            = std::move(job),
        .periodic_state = state,
    });
  }
  handle = PeriodicJobHandle{std::move(state)};
  signal_.notify_one();
  return Status::OK;
}

Status JobQueue::add_urgent_job(std::shared_ptr<Job> job) {
  if (job == nullptr) {
    Logger<ERROR> << "Cannot enqueue a null urgent job";
    return Status::INVALID_ARGUMENT;
  }
  {
    const std::lock_guard lock(jobs_mutex_);
    if (!active_) {
      return Status::QUEUE_STOPPED;
    }
    urgent_jobs_.push_back(JobEntry{
        .next_run = Clock::now(),
        .period   = std::chrono::milliseconds::zero(),
        .sequence = next_sequence_++,
        .job      = std::move(job),
    });
  }
  signal_.notify_one();
  return Status::OK;
}

void JobQueue::shutdown() noexcept {
  {
    const std::lock_guard lock(jobs_mutex_);
    if (!active_) {
      return;
    }
    active_ = false;
    urgent_jobs_.clear();
    while (!jobs_.empty()) {
      if (jobs_.top().periodic_state != nullptr) {
        jobs_.top().periodic_state->active.store(false,
                                                 std::memory_order_release);
      }
      jobs_.pop();
    }
  }
  signal_.notify_all();
  Logger<INFO> << "Shut down job queue";
}

void JobQueue::wait() noexcept {
  if (is_worker_thread()) {
    Logger<ERROR> << "A job queue cannot wait for its own worker thread";
    std::terminate();
  }

  const std::lock_guard wait_lock(wait_mutex_);
  shutdown();
  for (std::thread& worker_thread : workers_) {
    if (worker_thread.joinable()) {
      worker_thread.join();
    }
  }
}

bool JobQueue::active() const {
  const std::lock_guard lock(jobs_mutex_);
  return active_;
}

std::size_t JobQueue::pending_jobs() const {
  const std::lock_guard lock(jobs_mutex_);
  return urgent_jobs_.size() + jobs_.size();
}

void JobQueue::worker() noexcept {
  current_queue_ = this;
  while (true) {
    JobEntry entry;
    {
      std::unique_lock lock(jobs_mutex_);
      while (active_) {
        if (!urgent_jobs_.empty()) {
          entry = std::move(urgent_jobs_.front());
          urgent_jobs_.pop_front();
          break;
        }
        if (jobs_.empty()) {
          signal_.wait(lock);
          continue;
        }

        const Clock::time_point next_run = jobs_.top().next_run;
        if (next_run > Clock::now()) {
          signal_.wait_until(lock, next_run);
          continue;
        }
        entry = jobs_.top();
        jobs_.pop();
        break;
      }
      if (!active_) {
        current_queue_ = nullptr;
        return;
      }
      signal_.notify_one();
    }

    const bool periodic_active =
        entry.periodic_state == nullptr ||
        entry.periodic_state->active.load(std::memory_order_acquire);
    if (periodic_active) {
      entry.job->execute();
    }
    if (entry.periodic_state != nullptr &&
        entry.periodic_state->active.load(std::memory_order_acquire)) {
      {
        const std::lock_guard lock(jobs_mutex_);
        if (!active_) {
          entry.periodic_state->active.store(false, std::memory_order_release);
          continue;
        }
        entry.next_run = Clock::now() + entry.period;
        entry.sequence = next_sequence_++;
        jobs_.push(std::move(entry));
      }
      signal_.notify_one();
    }
  }
}

bool JobQueue::is_worker_thread() const noexcept {
  return current_queue_ == this;
}

}  // namespace puc::multithreading
