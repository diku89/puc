#pragma once

/**
 * @file framed_io.hpp
 * @brief Internal length-prefixed byte-stream operations for IPC transports.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "utils/ipc/status.hpp"

namespace puc::ipc::detail {

/** Kind of POSIX descriptor used by the framing helpers. */
enum class StreamKind {
  FILE_DESCRIPTOR, /**< Read and write using POSIX read()/write(). */
  SOCKET,          /**< Read with recv() and write with signal-safe send(). */
};

/** Set nonblocking and close-on-exec descriptor flags. */
Status configure_stream_descriptor(int descriptor) noexcept;

/** Apply platform-specific protection against socket SIGPIPE. */
Status configure_socket_descriptor(int descriptor) noexcept;

/**
 * Write one 32-bit network-order length prefix followed by one payload.
 *
 * The caller must serialize concurrent calls for the same descriptor.
 */
TransferResult write_frame(int descriptor,
                           std::span<const std::uint8_t> payload,
                           std::size_t maximum_message_bytes, StreamKind kind,
                           const std::atomic<bool>& stopping) noexcept;

/**
 * Read one complete length-prefixed payload into reusable owned storage.
 *
 * Output is cleared first. Polling wakes periodically to observe `stopping`.
 */
Status read_frame(int descriptor, std::vector<std::uint8_t>& output,
                  std::size_t maximum_message_bytes, StreamKind kind,
                  const std::atomic<bool>& stopping) noexcept;

}  // namespace puc::ipc::detail
