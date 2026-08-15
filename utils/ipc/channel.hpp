#pragma once

/**
 * @file channel.hpp
 * @brief Thread-safe subscription and transport abstraction for IPC events.
 */

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "utils/ipc/status.hpp"

namespace puc::multithreading {
class JobQueue;
}

namespace puc::ipc {

class Directory;

namespace detail {
struct SubscriptionState;
}  // namespace detail

/** Maximum byte length accepted for one ASCII canonical channel name. */
inline constexpr std::size_t kMaximumChannelNameBytes = 255U;

/** Optional delivery policy shared by every concrete Channel. */
struct ChannelOptions {
  /**
   * Maximum number of not-yet-delivered messages retained by the channel.
   *
   * When absent, delivery remains synchronous and borrow-only. When present,
   * the channel copies messages into a bounded asynchronous queue after it is
   * registered with a Directory. A full queue evicts its oldest pending
   * message before retaining the newest one. Zero is invalid.
   */
  std::optional<std::size_t> channel_max_depth;
};

/** Numeric channel identifier used by Directory and the IPC wire format. */
using ChannelId = std::uint32_t;

/**
 * Test whether a name is a canonical absolute IPC path.
 *
 * Names begin with `//` and contain one or more slash-separated ASCII
 * segments. Segment characters are letters, digits, `_`, `-`, and `.`;
 * empty, `.` and `..` segments are rejected. Examples include
 * `//screen/resize_events` and `//screen/trie_errors`.
 */
bool valid_channel_name(std::string_view name) noexcept;

/**
 * Move-only ownership of one active channel subscription.
 *
 * Destroying or resetting this object atomically disables its callback. A
 * concurrent delivery that already observed the callback as enabled may still
 * invoke or finish it; later activation checks will skip it.
 */
class Subscription {
 public:
  /** Construct an inactive subscription. */
  Subscription() noexcept = default;

  Subscription(const Subscription&)            = delete;
  Subscription& operator=(const Subscription&) = delete;

  /** Transfer ownership without changing whether the callback is active. */
  Subscription(Subscription&& other) noexcept;

  /** Disable any current callback, then take ownership from `other`. */
  Subscription& operator=(Subscription&& other) noexcept;

  /** Disable the callback if this subscription remains active. */
  ~Subscription();

  /** Disable the callback and make this object inactive. */
  void reset() noexcept;

  /** Return whether this object currently owns an enabled callback. */
  bool active() const noexcept;

  /** Return the channel-local subscription identifier, or zero when idle. */
  std::uint64_t id() const noexcept;

 private:
  friend class Channel;

  /** Adopt one newly registered callback state. */
  explicit Subscription(
      std::shared_ptr<detail::SubscriptionState> state) noexcept;

  std::shared_ptr<detail::SubscriptionState>
      state_; /**< Shared callback activation state. */
};

/**
 * Named byte-message endpoint with immutable read-side subscription snapshots.
 *
 * Channel separates logical event distribution from a concrete transport.
 * Derived classes implement `transmit()`, while the base class owns any number
 * of subscribers. Subscriber callbacks receive a borrowed view valid only for
 * that invocation and must not retain it. Callbacks are statically required to
 * be `noexcept` and should return quickly. A shared lock is held only while the
 * immutable subscriber snapshot pointer is copied; callbacks run after its
 * release and may therefore transmit or subscribe reentrantly.
 *
 * With no `channel_max_depth`, delivery is synchronous, retains no payload,
 * and performs no subscriber-list allocation. Configuring a depth makes
 * delivery asynchronous after Directory registration. The channel then owns
 * at most the newest N pending payload copies and invokes subscribers FIFO on
 * the caller-owned worker pool borrowed by Directory. The message currently
 * being delivered is not pending and is never evicted.
 */
class Channel {
 public:
  /** Byte view delivered to and from channel transports. */
  using Bytes = std::span<const std::uint8_t>;

  /**
   * Portable move-only callback whose invocation cannot throw.
   *
   * C++23's `std::move_only_function` is not yet present in every supported
   * standard library. This narrow wrapper provides the one signature IPC
   * needs while retaining compile-time `noexcept` enforcement.
   */
  class ReceiveCallback {
   public:
    /** Construct an empty callback. */
    ReceiveCallback() noexcept = default;

    /**
     * Type-erase one no-throw callable.
     *
     * @tparam Callback Callable type accepted as `void(Bytes) noexcept`.
     * @param[in] callback Callable to own.
     */
    template <typename Callback>
      requires(
          !std::same_as<std::remove_cvref_t<Callback>, ReceiveCallback> &&
          std::constructible_from<std::remove_cvref_t<Callback>, Callback> &&
          std::is_nothrow_invocable_r_v<void, Callback&, Bytes>)
    ReceiveCallback(Callback&& callback) {
      using StoredCallback = std::remove_cvref_t<Callback>;
      if constexpr (std::is_pointer_v<StoredCallback>) {
        if (callback == nullptr) {
          return;
        }
      }
      implementation_ = std::make_unique<Model<StoredCallback>>(
          std::forward<Callback>(callback));
    }

    ReceiveCallback(const ReceiveCallback&)            = delete;
    ReceiveCallback& operator=(const ReceiveCallback&) = delete;

    /** Transfer ownership of a callback. */
    ReceiveCallback(ReceiveCallback&&) noexcept = default;

    /** Replace this callback by moving another callback into it. */
    ReceiveCallback& operator=(ReceiveCallback&&) noexcept = default;

    /** Destroy the owned callable. */
    ~ReceiveCallback() = default;

    /** Return whether this object owns a callable. */
    explicit operator bool() const noexcept {
      return implementation_ != nullptr;
    }

    /** Invoke the callable when present. */
    void operator()(Bytes bytes) noexcept {
      if (implementation_ != nullptr) {
        implementation_->invoke(bytes);
      }
    }

   private:
    /** Type-erased callback interface. */
    class Interface {
     public:
      Interface()                            = default;
      Interface(const Interface&)            = delete;
      Interface& operator=(const Interface&) = delete;
      virtual ~Interface()                   = default;

      /** Invoke the concrete callback. */
      virtual void invoke(Bytes bytes) noexcept = 0;
    };

    /** Concrete callback holder. */
    template <typename Callback>
    class Model final : public Interface {
     public:
      /** Perfect-forward one callback into the holder. */
      template <typename ConfiguredCallback>
      explicit Model(ConfiguredCallback&& callback)
          : callback_(std::forward<ConfiguredCallback>(callback)) {}

      /** Invoke the held callback. */
      void invoke(Bytes bytes) noexcept override { callback_(bytes); }

     private:
      Callback callback_; /**< Owned concrete callable. */
    };

    std::unique_ptr<Interface> implementation_; /**< Owned type erasure. */
  };

  Channel(const Channel&)            = delete;
  Channel& operator=(const Channel&) = delete;
  Channel(Channel&&)                 = delete;
  Channel& operator=(Channel&&)      = delete;

  /** Destroy subscriber state after a derived transport has stopped. */
  virtual ~Channel();

  /**
   * Send one complete message through the concrete transport.
   *
   * A successful result reports exactly `data.size()` accepted payload bytes.
   * Message boundaries must be preserved by every implementation.
   */
  virtual TransferResult transmit(Bytes data) noexcept = 0;

  /**
   * Register one receive callback and replace `subscription` on success.
   *
   * Existing callbacks remain active. Replacing a populated output object
   * disables only the callback it previously owned.
   */
  Status subscribe(ReceiveCallback callback, Subscription& subscription);

  /** Return the canonical channel name. */
  const std::string& name() const noexcept;

  /** Return the channel's persistent initialization or transport status. */
  Status status() const noexcept;

  /** Return the number of callbacks that have not been disabled. */
  std::size_t subscriber_count() const noexcept;

  /** Return the configured pending-message limit, or no value if synchronous.
   */
  std::optional<std::size_t> channel_max_depth() const noexcept;

  /** Return the number of retained messages awaiting callback delivery. */
  std::size_t pending_messages() const noexcept;

  /** Return the lifetime count of oldest-pending messages evicted at capacity.
   */
  std::uint64_t dropped_messages() const noexcept;

 protected:
  /** Construct a channel and validate its immutable name. */
  explicit Channel(std::string name, ChannelOptions options = {});

  /** Deliver synchronously or enqueue under the configured bounded policy. */
  Status deliver(Bytes data) noexcept;

  /** Record a persistent channel failure, or restore Status::OK. */
  void set_status(Status status) noexcept;

 private:
  friend class Directory;

  /** Attach bounded delivery to one caller-owned worker pool. */
  Status attach_delivery_queue(multithreading::JobQueue& workers);

  /** Stop and discard bounded pending delivery before Directory removal. */
  void detach_delivery_queue() noexcept;

  class Impl;
  std::shared_ptr<Impl> impl_; /**< Name, subscribers, and delivery state. */
};

}  // namespace puc::ipc
