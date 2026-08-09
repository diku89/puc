#pragma once

/**
 * @file status.hpp
 * @brief Human-readable metronome lifecycle result codes.
 */

#include <string_view>

namespace puc::metronome {

/** Result of starting a Metronome publisher. */
enum class Status {
  OK,                   /**< The metronome is running. */
  CHANNEL_SETUP_FAILED, /**< The heartbeat channel could not be registered. */
  MESSAGE_ENCODING_FAILED, /**< The NullMessage payload could not be encoded. */
  SCHEDULING_FAILED,       /**< The worker pool rejected periodic execution. */
};

/** Return whether a metronome operation succeeded. */
constexpr bool is_ok(Status status) noexcept { return status == Status::OK; }

/** Return stable, human-readable text for one metronome result. */
constexpr std::string_view status_message(Status status) noexcept {
  switch (status) {
    case Status::OK:
      return "success";
    case Status::CHANNEL_SETUP_FAILED:
      return "metronome channel could not be registered";
    case Status::MESSAGE_ENCODING_FAILED:
      return "metronome NullMessage could not be encoded";
    case Status::SCHEDULING_FAILED:
      return "metronome periodic job could not be scheduled";
  }
  return "unknown metronome status";
}

}  // namespace puc::metronome
