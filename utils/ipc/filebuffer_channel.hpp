#pragma once

/**
 * @file filebuffer_channel.hpp
 * @brief Bidirectional framed IPC over a pair of POSIX named pipes.
 */

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

#include "utils/ipc/channel.hpp"

namespace puc::ipc {

/**
 * Message channel backed by two caller-owned FIFO filesystem entries.
 *
 * Despite its historical name, FileBufferChannel deliberately requires POSIX
 * named pipes rather than ordinary seekable files. One FIFO carries incoming
 * frames and the other carries outgoing frames. Its peer swaps those paths:
 * endpoint A's write path is endpoint B's read path and vice versa.
 *
 * Both FIFOs are opened nonblocking and read by a private worker. Framing is
 * identical to SocketChannel: a four-byte network-order payload length followed
 * by the complete payload. Concurrent writes from this object are serialized;
 * applications must not attach additional independent writers to the same
 * FIFO, because POSIX cannot make arbitrarily large multi-write frames atomic.
 * The caller creates and removes the FIFO entries.
 */
class FileBufferChannel final : public Channel {
 public:
  /**
   * Open an endpoint over two existing, distinct POSIX FIFO paths.
   *
   * Inspect `status()` before use. Construction does not wait for the peer and
   * never creates, truncates, or removes either filesystem entry.
   */
  FileBufferChannel(
      std::string name, std::filesystem::path read_path,
      std::filesystem::path write_path,
      std::size_t maximum_message_bytes = kDefaultMaximumMessageBytes,
      ChannelOptions options            = {});

  /** Stop the reader and close descriptors without removing either FIFO. */
  ~FileBufferChannel() override;

  /** Write one complete length-prefixed frame to the outgoing FIFO. */
  TransferResult transmit(Bytes data) noexcept override;

  /** Return the FIFO from which incoming messages are read. */
  const std::filesystem::path& read_path() const noexcept;

  /** Return the FIFO to which outgoing messages are written. */
  const std::filesystem::path& write_path() const noexcept;

  /** Return the largest payload accepted by this endpoint. */
  std::size_t maximum_message_bytes() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_; /**< FIFO descriptors and reader state. */
};

}  // namespace puc::ipc
