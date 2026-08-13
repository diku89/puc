#pragma once

/**
 * @file status.hpp
 * @brief Result values shared by polling and timer scheduling primitives.
 */

#include <string_view>

namespace puc::timer {

/** Result of polling a descriptor or scheduling timed work. */
enum class Status {
  OK,                /**< The operation completed successfully. */
  TIMED_OUT,         /**< No descriptor input arrived before the deadline. */
  CLOSED,            /**< The polled descriptor reached end of input. */
  INVALID_ARGUMENT,  /**< A descriptor, duration, or callback is invalid. */
  POLL_FAILED,       /**< The operating-system polling operation failed. */
  SCHEDULER_STOPPED, /**< The borrowed worker generation is no longer live. */
  SCHEDULING_FAILED, /**< The worker scheduler rejected timed work. */
};

/** Return whether a timer operation succeeded. */
constexpr bool is_ok(Status status) noexcept { return status == Status::OK; }

/** Return stable human-readable text for a timer result. */
constexpr std::string_view status_message(Status status) noexcept {
  switch (status) {
    case Status::OK:
      return "success";
    case Status::TIMED_OUT:
      return "poll timed out";
    case Status::CLOSED:
      return "descriptor was closed";
    case Status::INVALID_ARGUMENT:
      return "invalid timer argument";
    case Status::POLL_FAILED:
      return "descriptor polling failed";
    case Status::SCHEDULER_STOPPED:
      return "timer scheduler is stopped";
    case Status::SCHEDULING_FAILED:
      return "timed work could not be scheduled";
  }
  return "unknown timer status";
}

}  // namespace puc::timer
