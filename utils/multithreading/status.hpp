#pragma once

/**
 * @file status.hpp
 * @brief Human-readable job scheduling result codes.
 */

#include <string_view>

namespace puc::multithreading {

/** Result of submitting work to a JobQueue. */
enum class Status {
  OK,                 /**< The job was accepted. */
  INVALID_ARGUMENT,   /**< A supplied scheduling argument is invalid. */
  INVALID_PERIOD,     /**< A periodic job was given a zero period. */
  DELAY_OUT_OF_RANGE, /**< The millisecond delay cannot be represented. */
  QUEUE_STOPPED,      /**< The queue no longer accepts jobs. */
};

/** Return whether a scheduling operation succeeded. */
constexpr bool is_ok(Status status) noexcept { return status == Status::OK; }

/** Return stable, human-readable text for a scheduling status. */
constexpr std::string_view status_message(Status status) noexcept {
  switch (status) {
    case Status::OK:
      return "success";
    case Status::INVALID_ARGUMENT:
      return "job scheduling argument is invalid";
    case Status::INVALID_PERIOD:
      return "periodic job period must be positive";
    case Status::DELAY_OUT_OF_RANGE:
      return "job delay is outside the supported range";
    case Status::QUEUE_STOPPED:
      return "job queue is stopped";
  }
  return "unknown job queue status";
}

}  // namespace puc::multithreading
