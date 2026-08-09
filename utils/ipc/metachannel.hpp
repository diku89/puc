#pragma once

/**
 * @file metachannel.hpp
 * @brief Fan-out and fan-in composition of concrete IPC channels.
 */

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "utils/ipc/channel.hpp"

namespace puc::ipc {

/**
 * One logical channel composed from multiple delivery mechanisms.
 *
 * `transmit()` sends the same complete message to every underlying channel in
 * vector order. Success requires every destination to accept it; a mixture of
 * successes and failures returns Status::PARTIAL_TRANSFER. Messages received
 * by any underlying channel are delivered once to the MetaChannel's current
 * subscribers. Thus an echoing underlying channel, such as SmemChannel,
 * naturally contributes one inbound delivery during fan-out.
 *
 * Underlying channels are retained for this object's lifetime. Destruction
 * disables the private forwarding subscriptions before releasing those
 * references. As with every Channel, callers must not destroy the object from
 * inside one of its own callbacks.
 */
class MetaChannel final : public Channel {
 public:
  /**
   * Construct a logical channel over distinct, non-null channels.
   *
   * At least one healthy underlying channel is required. Inspect `status()`
   * before use; construction reports the first invalid underlying status or
   * Status::INVALID_ARGUMENT for an empty/null/duplicate collection.
   */
  MetaChannel(std::string name,
              std::vector<std::shared_ptr<Channel>> underlying_channels,
              ChannelOptions options = {});

  /** Disable inbound relays before releasing underlying channel references. */
  ~MetaChannel() override;

  /** Fan one complete message out to every underlying channel in order. */
  TransferResult transmit(Bytes data) noexcept override;

  /** Return a borrowed view of the ordered underlying channel references. */
  std::span<const std::shared_ptr<Channel>> underlying_channels()
      const noexcept;

  /** Return the number of fan-out destinations. */
  std::size_t destination_count() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_; /**< Underlying channels and relay state. */
};

}  // namespace puc::ipc
