/**
 * @file scheduler.cpp
 * @brief Timed-work facade implementation.
 */

#include "utils/timer/scheduler.hpp"

#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>

#include "utils/multithreading/status.hpp"

namespace puc::timer {
namespace {

/** Convert a public duration to JobQueue's checked millisecond input. */
bool milliseconds_value(std::chrono::milliseconds duration,
                        std::uint64_t& output) noexcept {
  if (duration.count() < 0) {
    return false;
  }
  output = static_cast<std::uint64_t>(duration.count());
  return true;
}

/** Preserve the useful distinction between a stopped queue and rejection. */
Status scheduling_status(multithreading::Status status) noexcept {
  if (multithreading::is_ok(status)) {
    return Status::OK;
  }
  return status == multithreading::Status::QUEUE_STOPPED
             ? Status::SCHEDULER_STOPPED
             : Status::SCHEDULING_FAILED;
}

}  // namespace

Status Scheduler::schedule_after(std::chrono::milliseconds delay,
                                 std::shared_ptr<detail::ScheduledJob> job) {
  std::uint64_t milliseconds = 0U;
  if (workers_ == nullptr || job == nullptr ||
      !milliseconds_value(delay, milliseconds)) {
    return Status::INVALID_ARGUMENT;
  }
  return scheduling_status(workers_->add_delayed(milliseconds, std::move(job)));
}

Status Scheduler::schedule_every(std::chrono::milliseconds period,
                                 std::shared_ptr<detail::ScheduledJob> job,
                                 PeriodicHandle& handle) {
  std::uint64_t milliseconds = 0U;
  if (workers_ == nullptr || job == nullptr || period.count() <= 0 ||
      !milliseconds_value(period, milliseconds)) {
    return Status::INVALID_ARGUMENT;
  }
  multithreading::PeriodicJobHandle next;
  const Status status = scheduling_status(
      workers_->add_periodic(milliseconds, std::move(job), next));
  if (is_ok(status)) {
    handle.handle_ = std::move(next);
  }
  return status;
}

}  // namespace puc::timer
