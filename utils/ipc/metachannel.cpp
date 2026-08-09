/**
 * @file metachannel.cpp
 * @brief Composite IPC channel implementation.
 */

#include "utils/ipc/metachannel.hpp"

#include <memory>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

#include "utils/logger/logger.hpp"

/** @cond IPC_META_LOGGER_MODULE */
LOGGER_MODULE("IPC Meta Channel");
/** @endcond */

namespace puc::ipc {

/** Ordered destinations and synchronized inbound relay subscriptions. */
class MetaChannel::Impl {
 public:
  /** Synchronizes relay callbacks with MetaChannel destruction. */
  struct Relay {
    std::recursive_mutex
        mutex; /**< Protects owner and permits reentrant relay. */
    MetaChannel* owner = nullptr; /**< Live destination, or null at shutdown. */
  };

  /** Retain the ordered fan-out destinations. */
  Impl(MetaChannel& owner,
       std::vector<std::shared_ptr<Channel>> configured_channels)
      : owner_(owner),
        channels_(std::move(configured_channels)),
        relay_(std::make_shared<Relay>()) {}

  Impl(const Impl&)            = delete;
  Impl& operator=(const Impl&) = delete;

  /** Ensure forwarding callbacks are disabled before members disappear. */
  ~Impl() { stop(); }

  /** Validate destinations and install one private relay on each. */
  Status start() {
    if (channels_.empty()) {
      Logger<ERROR> << "A metachannel requires at least one destination";
      return Status::INVALID_ARGUMENT;
    }
    std::set<const Channel*> distinct_channels;
    for (const std::shared_ptr<Channel>& channel : channels_) {
      if (channel == nullptr ||
          !distinct_channels.insert(channel.get()).second) {
        Logger<ERROR> << "A metachannel contains a null or duplicate channel";
        return Status::INVALID_ARGUMENT;
      }
      if (!is_ok(channel->status())) {
        Logger<ERROR> << "Underlying channel '" << channel->name()
                      << "' is unavailable: "
                      << status_message(channel->status());
        return channel->status();
      }
    }

    {
      const std::lock_guard lock(relay_->mutex);
      relay_->owner = &owner_;
    }
    relays_.reserve(channels_.size());
    for (const std::shared_ptr<Channel>& channel : channels_) {
      Subscription subscription;
      const Status result = channel->subscribe(
          [relay = relay_](Channel::Bytes bytes) noexcept {
            const std::lock_guard lock(relay->mutex);
            if (relay->owner != nullptr) {
              relay->owner->deliver(bytes);
            }
          },
          subscription);
      if (!is_ok(result)) {
        Logger<ERROR> << "Could not relay underlying channel '"
                      << channel->name() << "': " << status_message(result);
        stop();
        return result;
      }
      relays_.push_back(std::move(subscription));
    }
    return Status::OK;
  }

  /** Wait for current relays, disable new ones, and reset subscriptions. */
  void stop() noexcept {
    if (relay_ != nullptr) {
      const std::lock_guard lock(relay_->mutex);
      relay_->owner = nullptr;
    }
    relays_.clear();
  }

  /** Send in deterministic order and summarize destination outcomes. */
  TransferResult transmit(Bytes data) noexcept {
    std::size_t successful_destinations = 0U;
    Status first_failure                = Status::OK;
    for (const std::shared_ptr<Channel>& channel : channels_) {
      const TransferResult result = channel->transmit(data);
      if (is_ok(result.status) && result.bytes == data.size()) {
        ++successful_destinations;
      } else if (is_ok(first_failure)) {
        first_failure =
            is_ok(result.status) ? Status::PARTIAL_TRANSFER : result.status;
      }
    }
    if (successful_destinations == channels_.size()) {
      return TransferResult{.status = Status::OK, .bytes = data.size()};
    }
    if (successful_destinations == 0U) {
      return TransferResult{.status = first_failure};
    }
    Logger<WARN> << "Only " << successful_destinations << " of "
                 << channels_.size() << " destinations accepted a message on "
                 << owner_.name();
    return TransferResult{.status = Status::PARTIAL_TRANSFER};
  }

  /** Return the immutable destination vector as a borrowed span. */
  std::span<const std::shared_ptr<Channel>> channels() const noexcept {
    return channels_;
  }

 private:
  MetaChannel& owner_; /**< Logical channel receiving inbound relays. */
  std::vector<std::shared_ptr<Channel>> channels_; /**< Fan-out order. */
  std::shared_ptr<Relay> relay_; /**< Callback/destructor synchronization. */
  std::vector<Subscription> relays_; /**< One private inbound subscription. */
};

MetaChannel::MetaChannel(
    std::string name, std::vector<std::shared_ptr<Channel>> underlying_channels,
    ChannelOptions options)
    : Channel(std::move(name), std::move(options)),
      impl_(std::make_unique<Impl>(*this, std::move(underlying_channels))) {
  if (is_ok(status())) {
    set_status(impl_->start());
  }
}

MetaChannel::~MetaChannel() = default;

TransferResult MetaChannel::transmit(Bytes data) noexcept {
  const Status current_status = status();
  return is_ok(current_status) ? impl_->transmit(data)
                               : TransferResult{.status = current_status};
}

std::span<const std::shared_ptr<Channel>> MetaChannel::underlying_channels()
    const noexcept {
  return impl_->channels();
}

std::size_t MetaChannel::destination_count() const noexcept {
  return impl_->channels().size();
}

}  // namespace puc::ipc
