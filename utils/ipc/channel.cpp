/**
 * @file channel.cpp
 * @brief Channel naming, subscriptions, and snapshot-based callback delivery.
 */

#include "utils/ipc/channel.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "utils/logger/logger.hpp"
#include "utils/multithreading/job_queue.hpp"

/** @cond IPC_CHANNEL_LOGGER_MODULE */
LOGGER_MODULE("IPC Channel");
/** @endcond */

namespace puc::ipc {

namespace detail {

/** One callback plus the flag controlled by its Subscription owner. */
struct SubscriptionState {
  /** Construct one enabled subscriber. */
  SubscriptionState(std::uint64_t configured_id,
                    Channel::ReceiveCallback configured_callback)
      : id(configured_id), callback(std::move(configured_callback)) {}

  std::uint64_t id = 0;              /**< Channel-local nonzero identifier. */
  Channel::ReceiveCallback callback; /**< Immutable no-throw callback. */
  std::atomic<bool> active = true;   /**< Whether delivery may invoke it. */
};

}  // namespace detail

namespace {

/** Return whether one byte is permitted inside a channel-name segment. */
bool valid_segment_byte(unsigned char byte) noexcept {
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
         (byte >= '0' && byte <= '9') || byte == '_' || byte == '-' ||
         byte == '.';
}

}  // namespace

bool valid_channel_name(std::string_view name) noexcept {
  if (name.size() <= 2U || name.size() > kMaximumChannelNameBytes ||
      !name.starts_with("//") || name.back() == '/') {
    return false;
  }

  std::size_t segment_start = 2U;
  while (segment_start < name.size()) {
    const std::size_t separator = name.find('/', segment_start);
    const std::size_t segment_end =
        separator == std::string_view::npos ? name.size() : separator;
    const std::string_view segment =
        name.substr(segment_start, segment_end - segment_start);
    if (segment.empty() || segment == "." || segment == "..") {
      return false;
    }
    for (const unsigned char byte : segment) {
      if (!valid_segment_byte(byte)) {
        return false;
      }
    }
    if (separator == std::string_view::npos) {
      return true;
    }
    segment_start = separator + 1U;
  }
  return false;
}

/** Name, subscribers, and optional bounded asynchronous delivery state. */
class Channel::Impl : public std::enable_shared_from_this<Channel::Impl> {
 public:
  using State = detail::SubscriptionState; /**< Stored callback state type. */
  using Snapshot =
      std::vector<std::shared_ptr<State>>; /**< Immutable callback list. */

  /** Job that delivers exactly one pending message for fairness. */
  class DeliveryJob final : public multithreading::Job {
   public:
    /** Retain the channel state until this delivery invocation completes. */
    explicit DeliveryJob(std::shared_ptr<Impl> implementation)
        : implementation_(std::move(implementation)) {}

    /** Deliver one pending message and arrange any required successor job. */
    void execute() noexcept override { implementation_->deliver_one(); }

   private:
    std::shared_ptr<Impl>
        implementation_; /**< State kept alive for delivery. */
  };

  /** Construct empty subscriber and delivery state for one immutable name. */
  Impl(std::string configured_name, ChannelOptions configured_options)
      : channel_name(std::move(configured_name)),
        maximum_depth(configured_options.channel_max_depth),
        subscribers(std::make_shared<const Snapshot>()) {}

  /** Prevent queued work from outliving the Channel facade. */
  ~Impl() { stop_delivery(); }

  /** Invoke the current immutable callback snapshot on this thread. */
  void invoke(Channel::Bytes data) noexcept {
    const std::shared_ptr<const Snapshot> current =
        std::atomic_load_explicit(&subscribers, std::memory_order_acquire);
    for (const std::shared_ptr<State>& entry : *current) {
      if (entry->active.load(std::memory_order_acquire)) {
        entry->callback(data);
      }
    }
  }

  /** Attach a bounded channel to the caller-owned shared worker pool. */
  Status attach_delivery(multithreading::JobQueue& workers) {
    if (!maximum_depth.has_value()) {
      return Status::OK;
    }
    if (!workers.active()) {
      return Status::INVALID_ARGUMENT;
    }
    const std::lock_guard lock(delivery_mutex);
    if (destroying || delivery_attached) {
      return Status::INVALID_ARGUMENT;
    }
    delivery_workers  = &workers;
    delivery_attached = true;
    return Status::OK;
  }

  /** Enqueue one owned payload, retaining only the configured newest N. */
  Status enqueue(Channel::Bytes data) {
    std::vector<std::uint8_t> owned(data.begin(), data.end());
    multithreading::JobQueue* workers = nullptr;
    bool must_schedule                = false;
    {
      const std::lock_guard lock(delivery_mutex);
      if (destroying || !delivery_attached) {
        return Status::CHANNEL_UNAVAILABLE;
      }
      if (pending.size() == *maximum_depth) {
        pending.pop_front();
        ++dropped_count;
        Logger<WARN> << "Evicted oldest pending message on '" << channel_name
                     << "' at configured depth " << *maximum_depth;
      }
      pending.push_back(std::move(owned));
      if (!delivery_scheduled) {
        workers = delivery_workers;
        if (workers == nullptr) {
          pending.clear();
          return Status::CHANNEL_UNAVAILABLE;
        }
        delivery_scheduled = true;
        must_schedule      = true;
      }
    }

    if (!must_schedule) {
      return Status::OK;
    }
    const multithreading::Status queued =
        workers->add_urgent(std::make_shared<DeliveryJob>(shared_from_this()));
    if (multithreading::is_ok(queued)) {
      return Status::OK;
    }
    fail_scheduled_delivery();
    Logger<ERROR> << "Could not schedule bounded delivery on '" << channel_name
                  << "': " << multithreading::status_message(queued);
    return Status::CHANNEL_UNAVAILABLE;
  }

  /** Deliver one retained message, then fairly reschedule the next one. */
  void deliver_one() noexcept {
    std::vector<std::uint8_t> message;
    {
      const std::lock_guard lock(delivery_mutex);
      if (destroying || !delivery_attached || pending.empty()) {
        delivery_scheduled = false;
        delivery_changed.notify_all();
        return;
      }
      message = std::move(pending.front());
      pending.pop_front();
    }

    current_delivery() = this;
    invoke(message);
    current_delivery() = nullptr;

    multithreading::JobQueue* workers = nullptr;
    {
      const std::lock_guard lock(delivery_mutex);
      if (destroying || !delivery_attached || pending.empty()) {
        delivery_scheduled = false;
        delivery_changed.notify_all();
        return;
      }
      workers = delivery_workers;
      if (workers == nullptr) {
        pending.clear();
        delivery_scheduled = false;
        delivery_changed.notify_all();
        return;
      }
    }

    const multithreading::Status queued =
        workers->add_urgent(std::make_shared<DeliveryJob>(shared_from_this()));
    if (!multithreading::is_ok(queued)) {
      fail_scheduled_delivery();
      Logger<ERROR> << "Could not continue bounded delivery on '"
                    << channel_name
                    << "': " << multithreading::status_message(queued);
    }
  }

  /** Detach from a live Directory, discarding messages still pending. */
  void detach_delivery() noexcept {
    if (!maximum_depth.has_value()) {
      return;
    }
    std::unique_lock lock(delivery_mutex);
    delivery_attached = false;
    pending.clear();
    delivery_workers = nullptr;
    if (current_delivery() == this) {
      return;
    }
    delivery_changed.wait(lock, [this] { return !delivery_scheduled; });
  }

  /** Stop delivery during final Channel destruction. */
  void stop_delivery() noexcept {
    if (!maximum_depth.has_value()) {
      return;
    }
    std::unique_lock lock(delivery_mutex);
    destroying        = true;
    delivery_attached = false;
    pending.clear();
    delivery_workers = nullptr;
    if (current_delivery() == this) {
      return;
    }
    delivery_changed.wait(lock, [this] { return !delivery_scheduled; });
  }

  /** Clear a queue whose worker rejected its scheduled delivery job. */
  void fail_scheduled_delivery() noexcept {
    const std::lock_guard lock(delivery_mutex);
    pending.clear();
    delivery_scheduled = false;
    delivery_changed.notify_all();
  }

  /** Per-thread marker used to make reentrant detach nonblocking. */
  static Impl*& current_delivery() noexcept {
    static thread_local Impl* implementation = nullptr;
    return implementation;
  }

  std::string channel_name; /**< Immutable public channel path. */
  std::optional<std::size_t> maximum_depth; /**< Pending limit, if async. */
  std::atomic<Status> channel_status = Status::OK; /**< Persistent status. */
  std::mutex subscription_mutex; /**< Serializes snapshot replacement. */
  std::shared_ptr<const Snapshot>
      subscribers; /**< Atomically replaced read-side callback snapshot. */
  std::uint64_t next_subscription_id = 1U; /**< Next id, zero is reserved. */
  mutable std::mutex delivery_mutex; /**< Protects bounded delivery fields. */
  std::condition_variable delivery_changed; /**< Signals drain completion. */
  std::deque<std::vector<std::uint8_t>> pending; /**< Retained FIFO payloads. */
  multithreading::JobQueue* delivery_workers =
      nullptr;                     /**< Borrowed pool while registered. */
  bool delivery_attached  = false; /**< A Directory currently owns the queue. */
  bool delivery_scheduled = false; /**< One delivery job is queued/running. */
  bool destroying         = false; /**< Channel facade is being destroyed. */
  std::uint64_t dropped_count = 0U; /**< Oldest-pending eviction count. */
};

Subscription::Subscription(
    std::shared_ptr<detail::SubscriptionState> state) noexcept
    : state_(std::move(state)) {}

Subscription::Subscription(Subscription&& other) noexcept
    : state_(std::move(other.state_)) {}

Subscription& Subscription::operator=(Subscription&& other) noexcept {
  if (this != &other) {
    reset();
    state_ = std::move(other.state_);
  }
  return *this;
}

Subscription::~Subscription() { reset(); }

void Subscription::reset() noexcept {
  if (state_ != nullptr) {
    state_->active.store(false, std::memory_order_release);
    state_.reset();
  }
}

bool Subscription::active() const noexcept {
  return state_ != nullptr && state_->active.load(std::memory_order_acquire);
}

std::uint64_t Subscription::id() const noexcept {
  return state_ == nullptr ? 0U : state_->id;
}

Channel::Channel(std::string name, ChannelOptions options)
    : impl_(std::make_shared<Impl>(std::move(name), std::move(options))) {
  if (!valid_channel_name(impl_->channel_name)) {
    impl_->channel_status.store(Status::INVALID_CHANNEL_NAME,
                                std::memory_order_release);
    Logger<ERROR> << "Rejected invalid IPC channel name '"
                  << impl_->channel_name << "'";
  } else if (impl_->maximum_depth.has_value() && *impl_->maximum_depth == 0U) {
    impl_->channel_status.store(Status::INVALID_ARGUMENT,
                                std::memory_order_release);
    Logger<ERROR> << "Rejected zero channel_max_depth on '"
                  << impl_->channel_name << "'";
  }
}

Channel::~Channel() { impl_->stop_delivery(); }

Status Channel::subscribe(ReceiveCallback callback,
                          Subscription& subscription) {
  const Status current_status = status();
  if (!is_ok(current_status)) {
    return current_status;
  }
  if (!callback) {
    Logger<ERROR> << "Cannot register an empty callback on " << name();
    return Status::INVALID_ARGUMENT;
  }

  const std::lock_guard lock(impl_->subscription_mutex);
  if (impl_->next_subscription_id == 0U) {
    Logger<ERROR> << "Subscription identifiers exhausted on " << name();
    return Status::IDENTIFIER_EXHAUSTED;
  }

  auto state = std::make_shared<detail::SubscriptionState>(
      impl_->next_subscription_id++, std::move(callback));
  const std::shared_ptr<const Impl::Snapshot> current =
      std::atomic_load_explicit(&impl_->subscribers, std::memory_order_acquire);
  auto next = std::make_shared<Impl::Snapshot>();
  next->reserve(current->size() + 1U);
  for (const std::shared_ptr<detail::SubscriptionState>& entry : *current) {
    if (entry->active.load(std::memory_order_acquire)) {
      next->push_back(entry);
    }
  }
  next->push_back(state);
  std::atomic_store_explicit(
      &impl_->subscribers,
      std::shared_ptr<const Impl::Snapshot>{std::move(next)},
      std::memory_order_release);
  subscription = Subscription{std::move(state)};
  Logger<DEBUG> << "Registered subscriber " << subscription.id() << " on "
                << name();
  return Status::OK;
}

const std::string& Channel::name() const noexcept {
  return impl_->channel_name;
}

Status Channel::status() const noexcept {
  return impl_->channel_status.load(std::memory_order_acquire);
}

std::size_t Channel::subscriber_count() const noexcept {
  const std::shared_ptr<const Impl::Snapshot> current =
      std::atomic_load_explicit(&impl_->subscribers, std::memory_order_acquire);
  std::size_t count = 0;
  for (const std::shared_ptr<detail::SubscriptionState>& entry : *current) {
    if (entry->active.load(std::memory_order_acquire)) {
      ++count;
    }
  }
  return count;
}

std::optional<std::size_t> Channel::channel_max_depth() const noexcept {
  return impl_->maximum_depth;
}

std::size_t Channel::pending_messages() const noexcept {
  const std::lock_guard lock(impl_->delivery_mutex);
  return impl_->pending.size();
}

std::uint64_t Channel::dropped_messages() const noexcept {
  const std::lock_guard lock(impl_->delivery_mutex);
  return impl_->dropped_count;
}

Status Channel::deliver(Bytes data) noexcept {
  if (!impl_->maximum_depth.has_value()) {
    impl_->invoke(data);
    return Status::OK;
  }
  return impl_->enqueue(data);
}

void Channel::set_status(Status status) noexcept {
  impl_->channel_status.store(status, std::memory_order_release);
}

Status Channel::attach_delivery_queue(multithreading::JobQueue& workers) {
  return impl_->attach_delivery(workers);
}

void Channel::detach_delivery_queue() noexcept { impl_->detach_delivery(); }

}  // namespace puc::ipc
