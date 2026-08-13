/**
 * @file framed_io.cpp
 * @brief Partial-I/O-safe framing for Unix sockets and named pipes.
 */

#include "utils/ipc/framed_io.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "utils/logger/logger.hpp"
#include "utils/timer/poller.hpp"

/** @cond IPC_FRAMED_IO_LOGGER_MODULE */
LOGGER_MODULE("IPC Framed I/O");
/** @endcond */

namespace puc::ipc::detail {
namespace {

constexpr std::chrono::milliseconds kPollInterval{50};

/** Wait until a nonblocking descriptor can perform the requested operation. */
Status wait_until_ready(int descriptor, timer::PollInterest interest,
                        const std::atomic<bool>& stopping) noexcept {
  while (!stopping.load(std::memory_order_acquire)) {
    const timer::PollResult result =
        timer::poll_descriptor(descriptor, interest, kPollInterval);
    if (result.status == timer::Status::TIMED_OUT) {
      continue;
    }
    if (result.status == timer::Status::CLOSED) {
      return Status::END_OF_STREAM;
    }
    if (timer::is_ok(result.status)) {
      return Status::OK;
    }
    Logger<ERROR> << "Polling failed for IPC descriptor " << descriptor << ": "
                  << timer::status_message(result.status);
    return Status::IO_ERROR;
  }
  return Status::CHANNEL_UNAVAILABLE;
}

/** Read bytes from the selected kind of descriptor. */
ssize_t read_some(int descriptor, std::span<std::uint8_t> bytes,
                  StreamKind kind) noexcept {
  if (kind == StreamKind::SOCKET) {
    return ::recv(descriptor, bytes.data(), bytes.size(), 0);
  }
  return ::read(descriptor, bytes.data(), bytes.size());
}

/** Write bytes without allowing a disconnected socket to raise SIGPIPE. */
ssize_t write_some(int descriptor, std::span<const std::uint8_t> bytes,
                   StreamKind kind) noexcept {
  if (kind == StreamKind::SOCKET) {
#ifdef MSG_NOSIGNAL
    return ::send(descriptor, bytes.data(), bytes.size(), MSG_NOSIGNAL);
#else
    return ::send(descriptor, bytes.data(), bytes.size(), 0);
#endif
  }
  return ::write(descriptor, bytes.data(), bytes.size());
}

/** Fill a caller-owned buffer or return a precise stream status. */
Status read_exactly(int descriptor, std::span<std::uint8_t> bytes,
                    StreamKind kind,
                    const std::atomic<bool>& stopping) noexcept {
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const Status ready =
        wait_until_ready(descriptor, timer::PollInterest::READABLE, stopping);
    if (!is_ok(ready)) {
      return ready == Status::END_OF_STREAM && offset != 0U
                 ? Status::TRUNCATED_MESSAGE
                 : ready;
    }
    const ssize_t count = read_some(descriptor, bytes.subspan(offset), kind);
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
      continue;
    }
    if (count == 0) {
      return offset == 0U ? Status::END_OF_STREAM : Status::TRUNCATED_MESSAGE;
    }
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
      continue;
    }
    Logger<ERROR> << "IPC read failed on descriptor " << descriptor
                  << " with errno " << errno;
    return Status::IO_ERROR;
  }
  return Status::OK;
}

/** Write an entire buffer despite partial nonblocking writes. */
Status write_all(int descriptor, std::span<const std::uint8_t> bytes,
                 StreamKind kind, const std::atomic<bool>& stopping) noexcept {
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const Status ready =
        wait_until_ready(descriptor, timer::PollInterest::WRITABLE, stopping);
    if (!is_ok(ready)) {
      return ready;
    }
    const ssize_t count = write_some(descriptor, bytes.subspan(offset), kind);
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
      continue;
    }
    if (count == 0) {
      return Status::IO_ERROR;
    }
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
      continue;
    }
    if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN) {
      return Status::NOT_CONNECTED;
    }
    Logger<ERROR> << "IPC write failed on descriptor " << descriptor
                  << " with errno " << errno;
    return Status::IO_ERROR;
  }
  return Status::OK;
}

}  // namespace

Status configure_stream_descriptor(int descriptor) noexcept {
  const int descriptor_flags = ::fcntl(descriptor, F_GETFD);
  if (descriptor_flags < 0 ||
      ::fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
    Logger<ERROR> << "Could not set close-on-exec for IPC descriptor "
                  << descriptor << " (errno " << errno << ')';
    return Status::IO_ERROR;
  }
  const int status_flags = ::fcntl(descriptor, F_GETFL);
  if (status_flags < 0 ||
      ::fcntl(descriptor, F_SETFL, status_flags | O_NONBLOCK) < 0) {
    Logger<ERROR> << "Could not set nonblocking mode for IPC descriptor "
                  << descriptor << " (errno " << errno << ')';
    return Status::IO_ERROR;
  }
  return Status::OK;
}

Status configure_socket_descriptor(int descriptor) noexcept {
  const Status stream_status = configure_stream_descriptor(descriptor);
  if (!is_ok(stream_status)) {
    return stream_status;
  }
#ifdef SO_NOSIGPIPE
  constexpr int enabled = 1;
  if (::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                   sizeof(enabled)) != 0) {
    Logger<ERROR> << "Could not suppress SIGPIPE on IPC socket " << descriptor
                  << " (errno " << errno << ')';
    return Status::IO_ERROR;
  }
#endif
  return Status::OK;
}

TransferResult write_frame(int descriptor,
                           std::span<const std::uint8_t> payload,
                           std::size_t maximum_message_bytes, StreamKind kind,
                           const std::atomic<bool>& stopping) noexcept {
  if (descriptor < 0) {
    return TransferResult{.status = Status::NOT_CONNECTED};
  }
  if (payload.size() > maximum_message_bytes ||
      payload.size() > std::numeric_limits<std::uint32_t>::max()) {
    return TransferResult{.status = Status::MESSAGE_TOO_LARGE};
  }
  const std::uint32_t size = static_cast<std::uint32_t>(payload.size());
  const std::array<std::uint8_t, 4U> prefix = {
      static_cast<std::uint8_t>(size >> 24U),
      static_cast<std::uint8_t>(size >> 16U),
      static_cast<std::uint8_t>(size >> 8U),
      static_cast<std::uint8_t>(size),
  };
  Status result = write_all(descriptor, prefix, kind, stopping);
  if (is_ok(result)) {
    result = write_all(descriptor, payload, kind, stopping);
  }
  return is_ok(result)
             ? TransferResult{.status = Status::OK, .bytes = payload.size()}
             : TransferResult{.status = result};
}

Status read_frame(int descriptor, std::vector<std::uint8_t>& output,
                  std::size_t maximum_message_bytes, StreamKind kind,
                  const std::atomic<bool>& stopping) noexcept {
  output.clear();
  if (descriptor < 0) {
    return Status::NOT_CONNECTED;
  }
  std::array<std::uint8_t, 4U> prefix{};
  const Status prefix_status = read_exactly(descriptor, prefix, kind, stopping);
  if (!is_ok(prefix_status)) {
    return prefix_status;
  }
  const std::uint32_t size = (static_cast<std::uint32_t>(prefix[0]) << 24U) |
                             (static_cast<std::uint32_t>(prefix[1]) << 16U) |
                             (static_cast<std::uint32_t>(prefix[2]) << 8U) |
                             static_cast<std::uint32_t>(prefix[3]);
  if (size > maximum_message_bytes) {
    return Status::MESSAGE_TOO_LARGE;
  }
  output.resize(size);
  return read_exactly(descriptor, output, kind, stopping);
}

}  // namespace puc::ipc::detail
