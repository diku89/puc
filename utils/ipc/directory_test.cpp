/**
 * @file directory_test.cpp
 * @brief Tests for named IPC channel registration and dispatch.
 */

#include "utils/ipc/directory.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "utils/ipc/smem_channel.hpp"
#include "utils/multithreading/job_queue.hpp"

namespace puc::ipc {
namespace {

using namespace std::chrono_literals;

TEST(DirectoryTest, BorrowsItsConfiguredSharedDeliveryPool) {
  multithreading::JobQueue workers(4U);
  {
    Directory directory(workers);
    EXPECT_EQ(directory.delivery_worker_count(), 4U);
  }
  EXPECT_TRUE(workers.active());
}

TEST(DirectoryTest, RegistersNamesAssignsStableIdsAndRejectsDuplicates) {
  multithreading::JobQueue workers;
  Directory directory(workers);
  auto first  = std::make_shared<SmemChannel>("//screen/resize_events", 8U);
  auto second = std::make_shared<SmemChannel>("//screen/mouse_events", 8U);
  ChannelId first_id  = 99U;
  ChannelId second_id = 99U;
  ASSERT_EQ(directory.open_channel(first, first_id), Status::OK);
  ASSERT_EQ(directory.open_channel(second, second_id), Status::OK);
  EXPECT_EQ(first_id, 1U);
  EXPECT_EQ(second_id, 2U);
  EXPECT_EQ(directory.size(), 2U);
  EXPECT_EQ(directory.get_channel("//screen/resize_events"), first);
  EXPECT_EQ(directory.get_channel(first_id), first);

  ChannelId duplicate_id = 99U;
  EXPECT_EQ(directory.open_channel(
                std::make_shared<SmemChannel>("//screen/resize_events", 8U),
                duplicate_id),
            Status::DUPLICATE_CHANNEL);
  EXPECT_EQ(duplicate_id, 0U);

  ChannelId found_id = 99U;
  EXPECT_EQ(directory.get_channel_id("//screen/mouse_events", found_id),
            Status::OK);
  EXPECT_EQ(found_id, second_id);
}

TEST(DirectoryTest, ValidatesChannelsAndLookupNames) {
  multithreading::JobQueue workers;
  Directory directory(workers);
  ChannelId id = 99U;
  EXPECT_EQ(directory.open_channel(nullptr, id), Status::INVALID_ARGUMENT);
  EXPECT_EQ(id, 0U);
  EXPECT_EQ(
      directory.open_channel(std::make_shared<SmemChannel>("invalid", 8U), id),
      Status::INVALID_CHANNEL_NAME);
  EXPECT_EQ(directory.open_channel(
                std::make_shared<SmemChannel>("//invalid", 0U), id),
            Status::INVALID_ARGUMENT);
  EXPECT_EQ(directory.get_channel_id("invalid", id),
            Status::INVALID_CHANNEL_NAME);
  EXPECT_EQ(directory.get_channel_id("//missing", id),
            Status::CHANNEL_NOT_FOUND);
  EXPECT_EQ(id, 0U);
  EXPECT_EQ(directory.transmit("invalid", {}).status,
            Status::INVALID_CHANNEL_NAME);
  EXPECT_EQ(directory.transmit("//missing", {}).status,
            Status::CHANNEL_NOT_FOUND);
  EXPECT_EQ(directory.transmit(ChannelId{0U}, {}).status,
            Status::INVALID_ARGUMENT);
  EXPECT_EQ(directory.transmit(ChannelId{42U}, {}).status,
            Status::CHANNEL_NOT_FOUND);
}

TEST(DirectoryTest, DispatchesOutsideItsRegistryLock) {
  multithreading::JobQueue workers;
  Directory directory(workers);
  auto channel = std::make_shared<SmemChannel>("//events", 8U);
  ChannelId id = 0U;
  ASSERT_EQ(directory.open_channel(channel, id), Status::OK);

  std::size_t calls = 0U;
  Subscription subscription;
  Subscription id_subscription;
  ASSERT_EQ(directory.subscribe(
                "//events",
                [&directory, &calls](Channel::Bytes) noexcept {
                  ++calls;
                  ChannelId callback_id = 0U;
                  EXPECT_EQ(directory.get_channel_id("//events", callback_id),
                            Status::OK);
                },
                subscription),
            Status::OK);
  std::size_t id_calls = 0U;
  ASSERT_EQ(directory.subscribe(
                id, [&id_calls](Channel::Bytes) noexcept { ++id_calls; },
                id_subscription),
            Status::OK);
  constexpr std::array payload = {std::uint8_t{4}, std::uint8_t{2}};
  EXPECT_EQ(directory.transmit("//events", payload),
            (TransferResult{.status = Status::OK, .bytes = 2U}));
  EXPECT_EQ(calls, 1U);
  EXPECT_EQ(id_calls, 1U);
  EXPECT_EQ(directory.transmit(id, payload),
            (TransferResult{.status = Status::OK, .bytes = 2U}));
  EXPECT_EQ(calls, 2U);
  EXPECT_EQ(id_calls, 2U);
}

TEST(DirectoryTest, ClosingDropsOnlyTheDirectoryReferenceAndNeverReusesIds) {
  multithreading::JobQueue workers;
  Directory directory(workers);
  auto retained      = std::make_shared<SmemChannel>("//first", 8U);
  ChannelId first_id = 0U;
  ASSERT_EQ(directory.open_channel(retained, first_id), Status::OK);
  EXPECT_EQ(directory.close_channel("//first"), Status::OK);
  EXPECT_EQ(directory.close_channel("//first"), Status::CHANNEL_NOT_FOUND);
  EXPECT_EQ(directory.close_channel("first"), Status::INVALID_CHANNEL_NAME);
  EXPECT_EQ(directory.size(), 0U);
  EXPECT_EQ(directory.get_channel(first_id), nullptr);
  EXPECT_EQ(retained->transmit({}).status, Status::OK);

  ChannelId second_id = 0U;
  ASSERT_EQ(directory.open_channel(
                std::make_shared<SmemChannel>("//second", 8U), second_id),
            Status::OK);
  EXPECT_GT(second_id, first_id);
}

TEST(DirectoryTest, ClosingWaitsForActiveDeliveryAndKeepsOtherRoutesUsable) {
  multithreading::JobQueue workers;
  Directory directory(workers);
  auto upstream = std::make_shared<SmemChannel>(
      "//upstream", 1U, ChannelOptions{.channel_max_depth = 1U});
  auto downstream = std::make_shared<SmemChannel>(
      "//downstream", 1U, ChannelOptions{.channel_max_depth = 1U});
  ChannelId upstream_id   = 0U;
  ChannelId downstream_id = 0U;
  ASSERT_EQ(directory.open_channel(upstream, upstream_id), Status::OK);
  ASSERT_EQ(directory.open_channel(downstream, downstream_id), Status::OK);

  std::mutex mutex;
  std::condition_variable changed;
  bool callback_started     = false;
  bool release_callback     = false;
  bool close_started        = false;
  bool close_finished       = false;
  bool downstream_delivered = false;
  TransferResult forwarded;
  Status close_status = Status::INVALID_ARGUMENT;
  Subscription upstream_subscription;
  Subscription downstream_subscription;
  ASSERT_EQ(upstream->subscribe(
                [&](Channel::Bytes) noexcept {
                  {
                    std::unique_lock lock(mutex);
                    callback_started = true;
                    changed.notify_all();
                    changed.wait(lock, [&] { return release_callback; });
                  }
                  constexpr std::array payload = {std::uint8_t{2U}};
                  const TransferResult result =
                      directory.transmit("//downstream", payload);
                  {
                    const std::lock_guard lock(mutex);
                    forwarded = result;
                  }
                  changed.notify_all();
                },
                upstream_subscription),
            Status::OK);
  ASSERT_EQ(downstream->subscribe(
                [&](Channel::Bytes bytes) noexcept {
                  const std::lock_guard lock(mutex);
                  downstream_delivered =
                      bytes.size() == 1U && bytes.front() == std::uint8_t{2U};
                  changed.notify_all();
                },
                downstream_subscription),
            Status::OK);

  constexpr std::array initial = {std::uint8_t{1U}};
  ASSERT_EQ(directory.transmit("//upstream", initial).status, Status::OK);
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(changed.wait_for(lock, 2s, [&] { return callback_started; }));
  }

  std::thread closer([&] {
    {
      const std::lock_guard lock(mutex);
      close_started = true;
    }
    changed.notify_all();
    const Status result = directory.close_channel("//upstream");
    {
      const std::lock_guard lock(mutex);
      close_status   = result;
      close_finished = true;
    }
    changed.notify_all();
  });
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(changed.wait_for(lock, 2s, [&] { return close_started; }));
  }
  const auto close_deadline = std::chrono::steady_clock::now() + 2s;
  while (directory.get_channel("//upstream") != nullptr &&
         std::chrono::steady_clock::now() < close_deadline) {
    std::this_thread::yield();
  }
  EXPECT_EQ(directory.get_channel("//upstream"), nullptr);
  {
    const std::lock_guard lock(mutex);
    EXPECT_FALSE(close_finished);
    release_callback = true;
  }
  changed.notify_all();
  closer.join();

  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(
        changed.wait_for(lock, 2s, [&] { return downstream_delivered; }));
    EXPECT_EQ(close_status, Status::OK);
    EXPECT_EQ(forwarded, (TransferResult{.status = Status::OK, .bytes = 1U}));
  }
  EXPECT_EQ(directory.get_channel("//upstream"), nullptr);
  EXPECT_EQ(directory.get_channel("//downstream"), downstream);
}

TEST(DirectoryTest, ConcurrentRegistrationAssignsUniqueIds) {
  multithreading::JobQueue workers;
  Directory directory(workers);
  constexpr std::size_t thread_count        = 8U;
  constexpr std::size_t channels_per_thread = 25U;
  std::atomic<bool> failed                  = false;
  std::mutex ids_mutex;
  std::vector<ChannelId> ids;
  ids.reserve(thread_count * channels_per_thread);
  std::vector<std::thread> threads;
  for (std::size_t thread = 0U; thread < thread_count; ++thread) {
    threads.emplace_back([&, thread] {
      for (std::size_t channel_index = 0U; channel_index < channels_per_thread;
           ++channel_index) {
        const std::string name = "//concurrent/t" + std::to_string(thread) +
                                 "/c" + std::to_string(channel_index);
        ChannelId id           = 0U;
        if (!is_ok(directory.open_channel(
                std::make_shared<SmemChannel>(name, 1U), id))) {
          failed.store(true, std::memory_order_relaxed);
          continue;
        }
        const std::lock_guard lock(ids_mutex);
        ids.push_back(id);
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  EXPECT_FALSE(failed.load(std::memory_order_relaxed));
  EXPECT_EQ(directory.size(), thread_count * channels_per_thread);
  ASSERT_EQ(ids.size(), thread_count * channels_per_thread);
  std::ranges::sort(ids);
  EXPECT_EQ(std::ranges::unique(ids).begin(), ids.end());
}

}  // namespace
}  // namespace puc::ipc
