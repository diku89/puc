#pragma once

/**
 * @file smem_channel.hpp
 * @brief In-process direct or bounded asynchronous message delivery.
 */

#include <cstddef>
#include <string>

#include "utils/ipc/channel.hpp"

namespace puc::ipc {

/**
 * Fast in-process channel shared by application layers.
 *
 * With default ChannelOptions, `transmit()` synchronously invokes the current
 * subscriber snapshot without copying or retaining the payload. A configured
 * `channel_max_depth` instead accepts the payload into the bounded asynchronous
 * delivery queue installed when a Directory registers the channel.
 *
 * This is the default mechanism for paths such as
 * `//screen/resize_events`. It is named SmemChannel because subscribers share
 * the same process memory; it is not a POSIX cross-process shared-memory ring.
 */
class SmemChannel final : public Channel {
 public:
  /** Construct a local channel with one maximum message size. */
  SmemChannel(std::string name, std::size_t maximum_message_bytes,
              ChannelOptions options = {});

  /** Destroy the channel after its callers and Directory have quiesced. */
  ~SmemChannel() override;

  /** Deliver directly or enqueue one owned copy under the selected policy. */
  TransferResult transmit(Bytes data) noexcept override;

  /** Return the largest payload accepted by `transmit()`. */
  std::size_t maximum_message_bytes() const noexcept {
    return maximum_message_bytes_;
  }

 private:
  std::size_t maximum_message_bytes_ = 0; /**< Per-message payload limit. */
};

}  // namespace puc::ipc
