#pragma once

/**
 * @file scheduler.hpp
 * @brief Chrono-based timed-work facade over a caller-owned worker pool.
 */

#include <chrono>
#include <concepts>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

#include "utils/multithreading/job_queue.hpp"
#include "utils/timer/status.hpp"

namespace puc::timer {

namespace detail {

/** Type-erased base used by Scheduler's implementation boundary. */
using ScheduledJob = multithreading::Job;

/** Own one statically no-throw callback as a worker Job. */
template <typename Callback>
class CallbackJob final : public ScheduledJob {
 public:
  explicit CallbackJob(Callback callback) : callback_(std::move(callback)) {}

  void execute() noexcept override { callback_(); }

 private:
  Callback callback_;
};

}  // namespace detail

/** Move-only cancellation authority for one periodic timer. */
class PeriodicHandle {
 public:
  PeriodicHandle() noexcept                            = default;
  PeriodicHandle(const PeriodicHandle&)                = delete;
  PeriodicHandle& operator=(const PeriodicHandle&)     = delete;
  PeriodicHandle(PeriodicHandle&&) noexcept            = default;
  PeriodicHandle& operator=(PeriodicHandle&&) noexcept = default;
  ~PeriodicHandle()                                    = default;

  /** Prevent later invocations and release cancellation authority. */
  void cancel() noexcept { handle_.cancel(); }

  /** Return whether another invocation may occur. */
  bool active() const noexcept { return handle_.active(); }

 private:
  friend class Scheduler;
  multithreading::PeriodicJobHandle handle_;
};

/**
 * Schedule chrono-duration timeouts on one running JobQueue generation.
 *
 * Scheduler borrows the queue and does not own its threads. Its lifecycle owner
 * must destroy or stop all dependents before the borrowed queue is stopped.
 */
class Scheduler final {
 public:
  /** Borrow a worker queue for the scheduler's complete lifetime. */
  explicit Scheduler(multithreading::JobQueue& workers) noexcept
      : workers_(&workers) {}

  /** Schedule one no-throw callback after a nonnegative delay. */
  template <typename Callback>
    requires std::is_nothrow_invocable_r_v<void, Callback&>
  Status after(std::chrono::milliseconds delay, Callback&& callback) {
    using Stored = std::remove_cvref_t<Callback>;
    return schedule_after(delay, std::make_shared<detail::CallbackJob<Stored>>(
                                     std::forward<Callback>(callback)));
  }

  /** Schedule one non-overlapping no-throw callback at a positive period. */
  template <typename Callback>
    requires std::is_nothrow_invocable_r_v<void, Callback&>
  Status every(std::chrono::milliseconds period, Callback&& callback,
               PeriodicHandle& handle) {
    using Stored = std::remove_cvref_t<Callback>;
    return schedule_every(period,
                          std::make_shared<detail::CallbackJob<Stored>>(
                              std::forward<Callback>(callback)),
                          handle);
  }

 private:
  Status schedule_after(std::chrono::milliseconds delay,
                        std::shared_ptr<detail::ScheduledJob> job);
  Status schedule_every(std::chrono::milliseconds period,
                        std::shared_ptr<detail::ScheduledJob> job,
                        PeriodicHandle& handle);

  multithreading::JobQueue* workers_; /**< Borrowed running worker queue. */
};

}  // namespace puc::timer
