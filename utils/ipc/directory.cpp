/**
 * @file directory.cpp
 * @brief Thread-safe named IPC channel directory implementation.
 */

#include "utils/ipc/directory.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "utils/logger/logger.hpp"
#include "utils/multithreading/job_queue.hpp"

/** @cond IPC_DIRECTORY_LOGGER_MODULE */
LOGGER_MODULE("IPC Directory");
/** @endcond */

namespace puc::ipc {

/** Synchronized name and numeric-id indexes hidden behind Directory's ABI. */
class Directory::Impl {
 public:
  /** A channel and the identifier assigned when it was opened. */
  struct Entry {
    ChannelId id = 0U;                /**< Stable, nonzero identifier. */
    std::shared_ptr<Channel> channel; /**< Directory-owned channel reference. */
  };

  /** Retain a non-owning reference to the caller's shared worker pool. */
  explicit Impl(multithreading::JobQueue& configured_delivery_workers) noexcept
      : delivery_workers(configured_delivery_workers) {}

  mutable std::mutex mutex; /**< Protects entries and next_channel_id. */
  std::map<std::string, Entry, std::less<>> entries; /**< Name registry. */
  std::map<ChannelId, std::shared_ptr<Channel>>
      channels_by_id;             /**< Id index. */
  ChannelId next_channel_id = 1U; /**< Next never-before-used identifier. */
  multithreading::JobQueue&
      delivery_workers; /**< Borrowed executor for bounded channel queues. */
};

Directory::Directory(multithreading::JobQueue& delivery_workers)
    : impl_(std::make_unique<Impl>(delivery_workers)) {}

Directory::~Directory() {
  std::vector<std::shared_ptr<Channel>> channels;
  {
    const std::lock_guard lock(impl_->mutex);
    channels.reserve(impl_->channels_by_id.size());
    for (const auto& [channel_id, channel] : impl_->channels_by_id) {
      static_cast<void>(channel_id);
      channels.push_back(channel);
    }
  }
  // Keep routing visible while each upstream channel quiesces: a callback
  // already in progress may legitimately publish to a downstream channel.
  for (const std::shared_ptr<Channel>& channel : channels) {
    channel->detach_delivery_queue();
  }
  {
    const std::lock_guard lock(impl_->mutex);
    impl_->entries.clear();
    impl_->channels_by_id.clear();
  }
}

Status Directory::open_channel(std::shared_ptr<Channel> channel,
                               ChannelId& channel_id) {
  channel_id = 0U;
  if (channel == nullptr) {
    Logger<ERROR> << "Cannot register a null IPC channel";
    return Status::INVALID_ARGUMENT;
  }
  const Status channel_status = channel->status();
  if (!is_ok(channel_status)) {
    Logger<ERROR> << "Cannot register channel '" << channel->name()
                  << "': " << status_message(channel_status);
    return channel_status;
  }

  const std::lock_guard lock(impl_->mutex);
  if (impl_->entries.contains(channel->name())) {
    Logger<WARN> << "Channel '" << channel->name() << "' is already registered";
    return Status::DUPLICATE_CHANNEL;
  }
  if (impl_->next_channel_id == 0U) {
    Logger<ERROR> << "IPC channel identifiers are exhausted";
    return Status::IDENTIFIER_EXHAUSTED;
  }

  const Status delivery_status =
      channel->attach_delivery_queue(impl_->delivery_workers);
  if (!is_ok(delivery_status)) {
    Logger<ERROR> << "Cannot attach delivery queue for channel '"
                  << channel->name()
                  << "': " << status_message(delivery_status);
    return delivery_status;
  }

  channel_id             = impl_->next_channel_id++;
  const std::string name = channel->name();
  impl_->channels_by_id.emplace(channel_id, channel);
  impl_->entries.emplace(
      name, Impl::Entry{.id = channel_id, .channel = std::move(channel)});
  Logger<DEBUG> << "Registered channel '" << name << "' as " << channel_id;
  return Status::OK;
}

Status Directory::close_channel(std::string_view name) {
  if (!valid_channel_name(name)) {
    return Status::INVALID_CHANNEL_NAME;
  }
  std::shared_ptr<Channel> channel;
  {
    const std::lock_guard lock(impl_->mutex);
    const auto entry = impl_->entries.find(name);
    if (entry == impl_->entries.end()) {
      return Status::CHANNEL_NOT_FOUND;
    }
    channel = entry->second.channel;
    impl_->channels_by_id.erase(entry->second.id);
    impl_->entries.erase(entry);
  }
  channel->detach_delivery_queue();
  Logger<DEBUG> << "Removed channel '" << name << "'";
  return Status::OK;
}

std::shared_ptr<Channel> Directory::get_channel(std::string_view name) const {
  if (!valid_channel_name(name)) {
    return {};
  }
  const std::lock_guard lock(impl_->mutex);
  const auto entry = impl_->entries.find(name);
  return entry == impl_->entries.end() ? std::shared_ptr<Channel>{}
                                       : entry->second.channel;
}

std::shared_ptr<Channel> Directory::get_channel(ChannelId channel_id) const {
  if (channel_id == 0U) {
    return {};
  }
  const std::lock_guard lock(impl_->mutex);
  const auto entry = impl_->channels_by_id.find(channel_id);
  return entry == impl_->channels_by_id.end() ? std::shared_ptr<Channel>{}
                                              : entry->second;
}

Status Directory::get_channel_id(std::string_view name,
                                 ChannelId& channel_id) const {
  channel_id = 0U;
  if (!valid_channel_name(name)) {
    return Status::INVALID_CHANNEL_NAME;
  }
  const std::lock_guard lock(impl_->mutex);
  const auto entry = impl_->entries.find(name);
  if (entry == impl_->entries.end()) {
    return Status::CHANNEL_NOT_FOUND;
  }
  channel_id = entry->second.id;
  return Status::OK;
}

TransferResult Directory::transmit(std::string_view name,
                                   Channel::Bytes data) const noexcept {
  if (!valid_channel_name(name)) {
    return TransferResult{.status = Status::INVALID_CHANNEL_NAME};
  }
  const std::shared_ptr<Channel> channel = get_channel(name);
  if (channel == nullptr) {
    return TransferResult{.status = Status::CHANNEL_NOT_FOUND};
  }
  return channel->transmit(data);
}

TransferResult Directory::transmit(ChannelId channel_id,
                                   Channel::Bytes data) const noexcept {
  if (channel_id == 0U) {
    return TransferResult{.status = Status::INVALID_ARGUMENT};
  }
  const std::shared_ptr<Channel> channel = get_channel(channel_id);
  if (channel == nullptr) {
    return TransferResult{.status = Status::CHANNEL_NOT_FOUND};
  }
  return channel->transmit(data);
}

Status Directory::subscribe(std::string_view name,
                            Channel::ReceiveCallback callback,
                            Subscription& subscription) const {
  if (!valid_channel_name(name)) {
    return Status::INVALID_CHANNEL_NAME;
  }
  const std::shared_ptr<Channel> channel = get_channel(name);
  if (channel == nullptr) {
    return Status::CHANNEL_NOT_FOUND;
  }
  return channel->subscribe(std::move(callback), subscription);
}

Status Directory::subscribe(ChannelId channel_id,
                            Channel::ReceiveCallback callback,
                            Subscription& subscription) const {
  if (channel_id == 0U) {
    return Status::INVALID_ARGUMENT;
  }
  const std::shared_ptr<Channel> channel = get_channel(channel_id);
  if (channel == nullptr) {
    return Status::CHANNEL_NOT_FOUND;
  }
  return channel->subscribe(std::move(callback), subscription);
}

std::size_t Directory::size() const noexcept {
  const std::lock_guard lock(impl_->mutex);
  return impl_->entries.size();
}

std::size_t Directory::delivery_worker_count() const noexcept {
  return impl_->delivery_workers.worker_count();
}

}  // namespace puc::ipc
