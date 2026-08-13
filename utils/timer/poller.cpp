/**
 * @file poller.cpp
 * @brief POSIX descriptor-readiness polling implementation.
 */

#include "utils/timer/poller.hpp"

#include <poll.h>

#include <cerrno>
#include <chrono>
#include <limits>

namespace puc::timer {

PollResult poll_descriptor(int descriptor, PollInterest interest,
                           std::chrono::milliseconds timeout) noexcept {
  if (descriptor < 0 || timeout.count() < 0 ||
      timeout.count() > std::numeric_limits<int>::max()) {
    return {.status = Status::INVALID_ARGUMENT};
  }

  short requested = 0;
  switch (interest) {
    case PollInterest::READABLE:
      requested = POLLIN;
      break;
    case PollInterest::WRITABLE:
      requested = POLLOUT;
      break;
    default:
      return {.status = Status::INVALID_ARGUMENT};
  }

  pollfd event{
      .fd      = descriptor,
      .events  = requested,
      .revents = 0,
  };
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  int remaining       = static_cast<int>(timeout.count());
  int result          = 0;
  while (true) {
    result = ::poll(&event, 1U, remaining);
    if (result >= 0 || errno != EINTR) {
      break;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return {.status = Status::TIMED_OUT};
    }
    remaining = static_cast<int>(
        std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count());
  }

  if (result < 0) {
    return {.status = Status::POLL_FAILED};
  }
  if (result == 0) {
    return {.status = Status::TIMED_OUT};
  }
  if ((event.revents & (POLLERR | POLLNVAL)) != 0) {
    return {.status = Status::POLL_FAILED};
  }
  const bool hangup   = (event.revents & POLLHUP) != 0;
  const bool readable = (event.revents & POLLIN) != 0;
  const bool writable = (event.revents & POLLOUT) != 0;
  if ((event.revents & requested) != 0) {
    return {.status   = Status::OK,
            .readable = readable,
            .writable = writable,
            .hangup   = hangup};
  }
  return {.status = hangup ? Status::CLOSED : Status::POLL_FAILED,
          .hangup = hangup};
}

PollResult poll_readable(int descriptor,
                         std::chrono::milliseconds timeout) noexcept {
  return poll_descriptor(descriptor, PollInterest::READABLE, timeout);
}

PollResult poll_writable(int descriptor,
                         std::chrono::milliseconds timeout) noexcept {
  return poll_descriptor(descriptor, PollInterest::WRITABLE, timeout);
}

}  // namespace puc::timer
