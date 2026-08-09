/**
 * @file channel_test.cpp
 * @brief Tests for IPC channel names, subscriptions, and local delivery.
 */

#include <array>
#include <atomic>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "utils/ipc/directory.hpp"
#include "utils/ipc/smem_channel.hpp"
#include "utils/multithreading/job_queue.hpp"

namespace puc::ipc {
namespace {

using namespace std::chrono_literals;

TEST(ChannelNameTest, AcceptsCanonicalAbsoluteNames) {
  EXPECT_TRUE(valid_channel_name("//screen/resize_events"));
  EXPECT_TRUE(valid_channel_name("//screen/mouse-events.v2"));
  EXPECT_TRUE(valid_channel_name("//a/0/Z"));
  EXPECT_TRUE(valid_channel_name("//" + std::string(253U, 'a')));
}

TEST(ChannelNameTest, RejectsAmbiguousOrNonCanonicalNames) {
  constexpr std::array invalid_names = {
      std::string_view{},
      std::string_view{"/screen/events"},
      std::string_view{"//"},
      std::string_view{"//screen/"},
      std::string_view{"///x"},
      std::string_view{"//screen//events"},
      std::string_view{"//."},
      std::string_view{"//screen/.."},
      std::string_view{"//screen/events?"},
      std::string_view{"//screen/évents"},
  };
  for (const std::string_view name : invalid_names) {
    EXPECT_FALSE(valid_channel_name(name)) << name;
  }
  EXPECT_FALSE(valid_channel_name("//" + std::string(256U, 'a')));
}

TEST(SmemChannelTest, DeliversCompleteBorrowedMessagesToEverySubscriber) {
  SmemChannel channel{"//screen/resize_events", 64U};
  ASSERT_EQ(channel.status(), Status::OK);

  std::array<std::uint8_t, 3U> first_received{};
  std::array<std::uint8_t, 3U> second_received{};
  Subscription first;
  Subscription second;
  ASSERT_EQ(channel.subscribe(
                [&first_received](Channel::Bytes bytes) noexcept {
                  std::copy(bytes.begin(), bytes.end(), first_received.begin());
                },
                first),
            Status::OK);
  ASSERT_EQ(channel.subscribe(
                [&second_received](Channel::Bytes bytes) noexcept {
                  std::copy(bytes.begin(), bytes.end(),
                            second_received.begin());
                },
                second),
            Status::OK);

  constexpr std::array payload = {std::uint8_t{1}, std::uint8_t{2},
                                  std::uint8_t{3}};
  EXPECT_EQ(channel.transmit(payload),
            (TransferResult{.status = Status::OK, .bytes = payload.size()}));
  EXPECT_EQ(first_received, payload);
  EXPECT_EQ(second_received, payload);
  EXPECT_EQ(channel.subscriber_count(), 2U);
}

TEST(SmemChannelTest, SubscriptionLifetimeAndMoveControlDelivery) {
  SmemChannel channel{"//events", 1U};
  std::atomic<std::size_t> calls = 0U;
  Subscription original;
  ASSERT_EQ(channel.subscribe([&calls](Channel::Bytes) noexcept { ++calls; },
                              original),
            Status::OK);
  ASSERT_TRUE(original.active());
  const std::uint64_t id = original.id();

  Subscription moved = std::move(original);
  EXPECT_FALSE(original.active());
  EXPECT_EQ(original.id(), 0U);
  EXPECT_TRUE(moved.active());
  EXPECT_EQ(moved.id(), id);

  constexpr std::array payload = {std::uint8_t{1}};
  EXPECT_EQ(channel.transmit(payload).status, Status::OK);
  EXPECT_EQ(calls.load(), 1U);
  moved.reset();
  EXPECT_EQ(channel.transmit(payload).status, Status::OK);
  EXPECT_EQ(calls.load(), 1U);
  EXPECT_EQ(channel.subscriber_count(), 0U);
}

TEST(SmemChannelTest, RejectsBadConstructionAndOversizedMessages) {
  SmemChannel invalid_name{"events", 4U};
  EXPECT_EQ(invalid_name.status(), Status::INVALID_CHANNEL_NAME);
  EXPECT_EQ(invalid_name.transmit({}).status, Status::INVALID_CHANNEL_NAME);

  SmemChannel zero_limit{"//events", 0U};
  EXPECT_EQ(zero_limit.status(), Status::INVALID_ARGUMENT);

  SmemChannel limited{"//events", 2U};
  constexpr std::array oversized = {std::uint8_t{1}, std::uint8_t{2},
                                    std::uint8_t{3}};
  EXPECT_EQ(limited.transmit(oversized),
            (TransferResult{.status = Status::MESSAGE_TOO_LARGE}));
}

TEST(SmemChannelTest, SupportsReentrantTransmissionWithoutInternalLocks) {
  SmemChannel channel{"//events", 1U};
  std::size_t calls = 0U;
  Subscription subscription;
  ASSERT_EQ(channel.subscribe(
                [&channel, &calls](Channel::Bytes bytes) noexcept {
                  ++calls;
                  if (calls == 1U) {
                    static_cast<void>(channel.transmit(bytes));
                  }
                },
                subscription),
            Status::OK);
  constexpr std::array payload = {std::uint8_t{1}};
  EXPECT_EQ(channel.transmit(payload).status, Status::OK);
  EXPECT_EQ(calls, 2U);
}

TEST(SmemChannelTest, OwnsMoveOnlyCallbacksAndRejectsEmptyCallbacks) {
  SmemChannel channel{"//events", 1U};
  Subscription subscription;
  Channel::ReceiveCallback empty;
  EXPECT_EQ(channel.subscribe(std::move(empty), subscription),
            Status::INVALID_ARGUMENT);
  using FunctionPointer         = void (*)(Channel::Bytes) noexcept;
  FunctionPointer null_function = nullptr;
  EXPECT_EQ(channel.subscribe(null_function, subscription),
            Status::INVALID_ARGUMENT);

  auto marker = std::make_unique<std::size_t>(42U);
  ASSERT_EQ(channel.subscribe(
                [owned = std::move(marker)](Channel::Bytes) noexcept {
                  EXPECT_EQ(*owned, 42U);
                },
                subscription),
            Status::OK);
  constexpr std::array payload = {std::uint8_t{1}};
  EXPECT_EQ(channel.transmit(payload).status, Status::OK);
}

TEST(SmemChannelTest, ConcurrentTransmittersShareImmutableSubscriberSnapshot) {
  SmemChannel channel{"//events", 1U};
  std::atomic<std::size_t> calls = 0U;
  Subscription subscription;
  ASSERT_EQ(channel.subscribe(
                [&calls](Channel::Bytes) noexcept {
                  calls.fetch_add(1U, std::memory_order_relaxed);
                },
                subscription),
            Status::OK);
  constexpr std::size_t thread_count     = 4U;
  constexpr std::size_t calls_per_thread = 1000U;
  constexpr std::array payload           = {std::uint8_t{1}};
  std::vector<std::thread> transmitters;
  transmitters.reserve(thread_count);
  for (std::size_t thread = 0U; thread < thread_count; ++thread) {
    transmitters.emplace_back([&channel, &payload] {
      for (std::size_t call = 0U; call < calls_per_thread; ++call) {
        EXPECT_EQ(channel.transmit(payload).status, Status::OK);
      }
    });
  }
  for (std::thread& transmitter : transmitters) {
    transmitter.join();
  }
  EXPECT_EQ(calls.load(std::memory_order_relaxed),
            thread_count * calls_per_thread);
}

TEST(SmemChannelTest, ConcurrentSubscriptionAndDeliveryUseCompleteSnapshots) {
  SmemChannel channel{"//events", 1U};
  constexpr std::size_t subscriber_count   = 32U;
  constexpr std::size_t transmission_count = 512U;
  constexpr std::array payload             = {std::uint8_t{1}};
  std::vector<Subscription> subscriptions(subscriber_count);
  std::atomic<std::size_t> calls = 0U;
  std::atomic<bool> start        = false;
  std::atomic<bool> failed       = false;

  std::thread subscriber([&] {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    for (Subscription& subscription : subscriptions) {
      const Status status = channel.subscribe(
          [&calls](Channel::Bytes) noexcept {
            calls.fetch_add(1U, std::memory_order_relaxed);
          },
          subscription);
      if (!is_ok(status)) {
        failed.store(true, std::memory_order_relaxed);
        return;
      }
    }
  });
  std::thread transmitter([&] {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    for (std::size_t index = 0U; index < transmission_count; ++index) {
      if (!is_ok(channel.transmit(payload).status)) {
        failed.store(true, std::memory_order_relaxed);
        return;
      }
      static_cast<void>(channel.subscriber_count());
    }
  });

  start.store(true, std::memory_order_release);
  subscriber.join();
  transmitter.join();
  ASSERT_FALSE(failed.load(std::memory_order_relaxed));
  ASSERT_EQ(channel.subscriber_count(), subscriber_count);

  const std::size_t calls_before = calls.load(std::memory_order_relaxed);
  ASSERT_EQ(channel.transmit(payload).status, Status::OK);
  EXPECT_EQ(calls.load(std::memory_order_relaxed),
            calls_before + subscriber_count);
}

TEST(SmemChannelTest, BoundedDeliveryRetainsNewestPendingMessagesInFifoOrder) {
  multithreading::JobQueue workers(2U);
  Directory directory(workers);
  auto channel = std::make_shared<SmemChannel>(
      "//screen/present_commands", 1U, ChannelOptions{.channel_max_depth = 3U});
  ChannelId channel_id = 0U;
  ASSERT_EQ(directory.open_channel(channel, channel_id), Status::OK);
  EXPECT_EQ(channel->channel_max_depth(), 3U);

  std::mutex mutex;
  std::condition_variable changed;
  bool first_started = false;
  bool release_first = false;
  std::vector<std::uint8_t> received;
  Subscription subscription;
  ASSERT_EQ(channel->subscribe(
                [&](Channel::Bytes bytes) noexcept {
                  if (bytes.front() == 1U) {
                    std::unique_lock lock(mutex);
                    first_started = true;
                    changed.notify_all();
                    changed.wait(lock, [&] { return release_first; });
                  }
                  {
                    const std::lock_guard lock(mutex);
                    received.push_back(bytes.front());
                  }
                  changed.notify_all();
                },
                subscription),
            Status::OK);

  const auto send = [&](std::uint8_t value) {
    const std::array payload = {value};
    return channel->transmit(payload);
  };
  EXPECT_EQ(send(1U), (TransferResult{.status = Status::OK, .bytes = 1U}));
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(changed.wait_for(lock, 2s, [&] { return first_started; }));
  }

  for (std::uint8_t value = 2U; value <= 6U; ++value) {
    EXPECT_EQ(send(value), (TransferResult{.status = Status::OK, .bytes = 1U}));
  }
  EXPECT_EQ(channel->pending_messages(), 3U);
  EXPECT_EQ(channel->dropped_messages(), 2U);

  {
    const std::lock_guard lock(mutex);
    release_first = true;
  }
  changed.notify_all();
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(
        changed.wait_for(lock, 2s, [&] { return received.size() == 4U; }));
  }
  EXPECT_EQ(received, (std::vector<std::uint8_t>{1U, 4U, 5U, 6U}));
  EXPECT_EQ(channel->pending_messages(), 0U);
}

TEST(SmemChannelTest, BoundedDeliveryRequiresDirectoryAndPositiveDepth) {
  SmemChannel unattached{"//events", 1U,
                         ChannelOptions{.channel_max_depth = 1U}};
  EXPECT_EQ(unattached.status(), Status::OK);
  constexpr std::array payload = {std::uint8_t{1U}};
  EXPECT_EQ(unattached.transmit(payload).status, Status::CHANNEL_UNAVAILABLE);

  SmemChannel zero_depth{"//zero", 1U, ChannelOptions{.channel_max_depth = 0U}};
  EXPECT_EQ(zero_depth.status(), Status::INVALID_ARGUMENT);
  EXPECT_EQ(zero_depth.transmit(payload).status, Status::INVALID_ARGUMENT);
}

TEST(ReceiveCallbackTest, RequiresNothrowInvocationAtCompileTime) {
  const auto may_throw    = [](Channel::Bytes) {};
  const auto cannot_throw = [](Channel::Bytes) noexcept {};
  static_assert(
      !std::constructible_from<Channel::ReceiveCallback, decltype(may_throw)>);
  static_assert(std::constructible_from<Channel::ReceiveCallback,
                                        decltype(cannot_throw)>);
  SUCCEED();
}

TEST(StatusTest, EveryStatusHasStableHumanReadableText) {
  constexpr std::array statuses = {
      Status::OK,
      Status::INVALID_ARGUMENT,
      Status::INVALID_CHANNEL_NAME,
      Status::INVALID_TRANSPORT_PATH,
      Status::DUPLICATE_CHANNEL,
      Status::CHANNEL_NOT_FOUND,
      Status::CHANNEL_UNAVAILABLE,
      Status::NOT_CONNECTED,
      Status::MESSAGE_TOO_LARGE,
      Status::PARTIAL_TRANSFER,
      Status::IO_ERROR,
      Status::END_OF_STREAM,
      Status::MALFORMED_MESSAGE,
      Status::TRUNCATED_MESSAGE,
      Status::UNSUPPORTED_VERSION,
      Status::CHECKSUM_MISMATCH,
      Status::IDENTIFIER_EXHAUSTED,
  };
  for (const Status status : statuses) {
    EXPECT_FALSE(status_message(status).empty());
    EXPECT_EQ(is_ok(status), status == Status::OK);
  }
}

}  // namespace
}  // namespace puc::ipc
