#pragma once

/**
 * @file poller.hpp
 * @brief EINTR-safe descriptor-readiness polling.
 */

#include <chrono>

#include "utils/timer/status.hpp"

namespace puc::timer {

/** Descriptor condition requested from the operating-system poller. */
enum class PollInterest {
  READABLE, /**< Wait until a read can make progress. */
  WRITABLE, /**< Wait until a write can make progress. */
};

/** Complete readiness observation returned by one descriptor poll. */
struct PollResult {
  Status status = Status::TIMED_OUT; /**< Overall polling result. */
  bool readable = false;             /**< Whether a read may be attempted. */
  bool writable = false;             /**< Whether a write may be attempted. */
  bool hangup   = false;             /**< Whether the peer also hung up. */

  /** Return whether polling produced the requested ready condition. */
  explicit constexpr operator bool() const noexcept {
    return is_ok(status) && (readable || writable);
  }
};

/** Wait for one readable or writable descriptor condition. */
PollResult poll_descriptor(int descriptor, PollInterest interest,
                           std::chrono::milliseconds timeout) noexcept;

/**
 * Wait until a descriptor can be read, closes, or reaches a timeout.
 *
 * A zero timeout performs a nonblocking poll. Durations that cannot be
 * represented by the operating-system millisecond API are rejected.
 */
PollResult poll_readable(int descriptor,
                         std::chrono::milliseconds timeout) noexcept;

/** Wait until a descriptor can be written, closes, or reaches a timeout. */
PollResult poll_writable(int descriptor,
                         std::chrono::milliseconds timeout) noexcept;

}  // namespace puc::timer
