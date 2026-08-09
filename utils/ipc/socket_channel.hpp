#pragma once

/**
 * @file socket_channel.hpp
 * @brief Bidirectional framed IPC over a Unix-domain stream socket.
 */

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

#include "utils/ipc/channel.hpp"

namespace puc::ipc {

/** Connection role assumed by one SocketChannel endpoint. */
enum class SocketRole {
  SERVER, /**< Bind, listen, and accept one peer at a time. */
  CLIENT, /**< Connect once to an already listening server. */
};

/**
 * Unix-domain socket channel preserving complete byte-message boundaries.
 *
 * Each message is encoded as a four-byte network-order length followed by its
 * payload. The server accepts one client at a time and returns to `accept()`
 * after that client disconnects. The client performs one connection attempt
 * during construction. Incoming frames are delivered from a private reader
 * thread; therefore subscriber callbacks must be thread-safe and quick.
 * Concurrent `transmit()` calls are serialized so their frames cannot
 * interleave.
 *
 * A server never removes a pre-existing filesystem entry. Once it binds an
 * unused path, it owns and removes only that socket entry during destruction.
 */
class SocketChannel final : public Channel {
 public:
  /**
   * Construct and start one Unix-domain socket endpoint.
   *
   * Inspect `status()` before use. A server is healthy before a peer connects;
   * its `transmit()` reports Status::NOT_CONNECTED until then.
   */
  SocketChannel(std::string name, std::filesystem::path socket_path,
                SocketRole role,
                std::size_t maximum_message_bytes = kDefaultMaximumMessageBytes,
                ChannelOptions options            = {});

  /** Stop the reader, close descriptors, and remove an owned server socket. */
  ~SocketChannel() override;

  /** Send one complete length-prefixed frame to the connected peer. */
  TransferResult transmit(Bytes data) noexcept override;

  /** Return whether this endpoint currently has an accepted/connected peer. */
  bool connected() const noexcept;

  /** Return whether this endpoint connects or accepts. */
  SocketRole role() const noexcept;

  /** Return the absolute Unix-domain socket path used by the endpoint. */
  const std::filesystem::path& socket_path() const noexcept;

  /** Return the largest payload accepted by this endpoint. */
  std::size_t maximum_message_bytes() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_; /**< Descriptors, synchronization, and reader. */
};

}  // namespace puc::ipc
