#pragma once

/**
 * @file directory.hpp
 * @brief Thread-safe registry and dispatcher for named IPC channels.
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "utils/ipc/channel.hpp"

namespace puc::ipc {

/**
 * Process-local source of truth for channel names and identifiers.
 *
 * A Directory owns shared references to registered channels. Each canonical
 * name receives a nonzero, monotonically increasing identifier that is never
 * reused during the Directory's lifetime. Registration and lookup are
 * thread-safe. Transmission and callback registration occur after the
 * directory lock is released, so channel callbacks may safely call back into
 * the same Directory.
 */
class Directory {
 public:
  /**
   * Construct an empty directory using a caller-owned delivery pool.
   *
   * The pool must remain alive and accepting work through Directory
   * destruction. Directory detaches and drains its channels, but never stops
   * or joins the pool.
   */
  explicit Directory(multithreading::JobQueue& delivery_workers);

  Directory(const Directory&)            = delete;
  Directory& operator=(const Directory&) = delete;
  Directory(Directory&&)                 = delete;
  Directory& operator=(Directory&&)      = delete;

  /** Detach channels and finish their active callbacks. */
  ~Directory();

  /**
   * Register a channel and return its assigned identifier.
   *
   * @param[in] channel Channel to retain.
   * @param[out] channel_id Assigned nonzero identifier; reset to zero first.
   * @return Status::OK, Status::INVALID_ARGUMENT for a null channel, the
   *         channel's persistent error status, Status::DUPLICATE_CHANNEL, or
   *         Status::IDENTIFIER_EXHAUSTED.
   */
  Status open_channel(std::shared_ptr<Channel> channel, ChannelId& channel_id);

  /**
   * Remove a registered channel without invalidating external references.
   *
   * @return Status::OK, Status::INVALID_CHANNEL_NAME, or
   *         Status::CHANNEL_NOT_FOUND.
   */
  Status close_channel(std::string_view name);

  /** Return a shared channel reference, or an empty reference when absent. */
  std::shared_ptr<Channel> get_channel(std::string_view name) const;

  /** Return a shared channel reference by wire id, or empty when absent. */
  std::shared_ptr<Channel> get_channel(ChannelId channel_id) const;

  /**
   * Look up a channel's wire identifier.
   *
   * @param[in] name Canonical channel name.
   * @param[out] channel_id Found identifier; reset to zero first.
   */
  Status get_channel_id(std::string_view name, ChannelId& channel_id) const;

  /** Send one complete message through the channel registered under `name`. */
  TransferResult transmit(std::string_view name,
                          Channel::Bytes data) const noexcept;

  /** Send one complete message through the channel assigned `channel_id`. */
  TransferResult transmit(ChannelId channel_id,
                          Channel::Bytes data) const noexcept;

  /** Register a receive callback on the channel named by `name`. */
  Status subscribe(std::string_view name, Channel::ReceiveCallback callback,
                   Subscription& subscription) const;

  /** Register a receive callback using a nonzero wire channel identifier. */
  Status subscribe(ChannelId channel_id, Channel::ReceiveCallback callback,
                   Subscription& subscription) const;

  /** Return the number of currently registered channel names. */
  std::size_t size() const noexcept;

  /** Return the fixed number of asynchronous delivery workers. */
  std::size_t delivery_worker_count() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_; /**< Registry state hidden from consumers. */
};

}  // namespace puc::ipc
