/**
 * @file socket_channel_test.cpp
 * @brief Tests for framed Unix-domain socket IPC channels.
 */

#include "utils/ipc/socket_channel.hpp"

#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

namespace puc::ipc {
namespace {

using namespace std::chrono_literals;

/** Wait briefly for an asynchronous socket state transition. */
template <typename Predicate>
bool wait_for(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!predicate() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  return predicate();
}

/** Return a short, process-unique path that fits Darwin's sockaddr_un. */
std::filesystem::path socket_path() {
  const ::testing::TestInfo* info =
      ::testing::UnitTest::GetInstance()->current_test_info();
  EXPECT_NE(info, nullptr);
  return std::filesystem::path{"/tmp"} /
         (std::string{"puc_ipc_"} + std::to_string(::getpid()) + "_" +
          (info == nullptr ? "unknown" : info->name()) + ".sock");
}

TEST(SocketChannelTest, ServerIsUsableBeforeAClientConnects) {
  const std::filesystem::path path = socket_path();
  SocketChannel server{"//socket/events", path, SocketRole::SERVER, 8U};
  ASSERT_EQ(server.status(), Status::OK);
  EXPECT_EQ(server.role(), SocketRole::SERVER);
  EXPECT_EQ(server.socket_path(), path);
  EXPECT_EQ(server.maximum_message_bytes(), 8U);
  EXPECT_FALSE(server.connected());
  EXPECT_EQ(server.transmit({}).status, Status::NOT_CONNECTED);
  EXPECT_TRUE(std::filesystem::is_socket(path));
}

TEST(SocketChannelTest, DeliversBidirectionallyAndPreservesEmptyFrames) {
  const std::filesystem::path path = socket_path();
  SocketChannel server{"//socket/events", path, SocketRole::SERVER, 64U};
  ASSERT_EQ(server.status(), Status::OK);

  std::mutex receive_mutex;
  std::condition_variable received;
  std::vector<std::uint8_t> at_server;
  std::vector<std::uint8_t> at_client;
  std::size_t server_calls = 0U;
  std::size_t client_calls = 0U;
  Subscription server_subscription;
  ASSERT_EQ(server.subscribe(
                [&](Channel::Bytes bytes) noexcept {
                  const std::lock_guard lock(receive_mutex);
                  at_server.assign(bytes.begin(), bytes.end());
                  ++server_calls;
                  received.notify_all();
                },
                server_subscription),
            Status::OK);

  {
    SocketChannel client{"//socket/events", path, SocketRole::CLIENT, 64U};
    ASSERT_EQ(client.status(), Status::OK);
    ASSERT_TRUE(wait_for([&server] { return server.connected(); }));
    ASSERT_TRUE(client.connected());
    Subscription client_subscription;
    ASSERT_EQ(client.subscribe(
                  [&](Channel::Bytes bytes) noexcept {
                    const std::lock_guard lock(receive_mutex);
                    at_client.assign(bytes.begin(), bytes.end());
                    ++client_calls;
                    received.notify_all();
                  },
                  client_subscription),
              Status::OK);

    constexpr std::array to_server = {std::uint8_t{1}, std::uint8_t{2},
                                      std::uint8_t{3}};
    EXPECT_EQ(
        client.transmit(to_server),
        (TransferResult{.status = Status::OK, .bytes = to_server.size()}));
    {
      std::unique_lock lock(receive_mutex);
      ASSERT_TRUE(
          received.wait_for(lock, 2s, [&] { return server_calls == 1U; }));
      EXPECT_EQ(at_server, (std::vector<std::uint8_t>{to_server.begin(),
                                                      to_server.end()}));
    }

    constexpr std::array to_client = {std::uint8_t{9}, std::uint8_t{8}};
    EXPECT_EQ(
        server.transmit(to_client),
        (TransferResult{.status = Status::OK, .bytes = to_client.size()}));
    {
      std::unique_lock lock(receive_mutex);
      ASSERT_TRUE(
          received.wait_for(lock, 2s, [&] { return client_calls == 1U; }));
      EXPECT_EQ(at_client, (std::vector<std::uint8_t>{to_client.begin(),
                                                      to_client.end()}));
    }

    EXPECT_EQ(client.transmit({}),
              (TransferResult{.status = Status::OK, .bytes = 0U}));
    {
      std::unique_lock lock(receive_mutex);
      ASSERT_TRUE(
          received.wait_for(lock, 2s, [&] { return server_calls == 2U; }));
      EXPECT_TRUE(at_server.empty());
    }
  }
  EXPECT_TRUE(wait_for([&server] { return !server.connected(); }));
  EXPECT_EQ(server.transmit({}).status, Status::NOT_CONNECTED);
}

TEST(SocketChannelTest, ServerAcceptsAReplacementClient) {
  const std::filesystem::path path = socket_path();
  SocketChannel server{"//socket/events", path, SocketRole::SERVER, 8U};
  ASSERT_EQ(server.status(), Status::OK);
  for (std::size_t connection = 0U; connection < 2U; ++connection) {
    {
      SocketChannel client{"//socket/events", path, SocketRole::CLIENT, 8U};
      ASSERT_EQ(client.status(), Status::OK);
      ASSERT_TRUE(wait_for([&server] { return server.connected(); }));
    }
    ASSERT_TRUE(wait_for([&server] { return !server.connected(); }));
  }
}

TEST(SocketChannelTest, RejectsBadPathsLimitsAndPreexistingEntries) {
  SocketChannel empty_path{"//socket/events", {}, SocketRole::SERVER};
  EXPECT_EQ(empty_path.status(), Status::INVALID_TRANSPORT_PATH);

  SocketChannel long_path{
      "//socket/events", std::filesystem::path{"/tmp"} / std::string(256U, 'x'),
      SocketRole::SERVER};
  EXPECT_EQ(long_path.status(), Status::INVALID_TRANSPORT_PATH);

  SocketChannel zero_limit{"//socket/events", socket_path(), SocketRole::SERVER,
                           0U};
  EXPECT_EQ(zero_limit.status(), Status::INVALID_ARGUMENT);

  const std::filesystem::path path = socket_path();
  SocketChannel first{"//socket/first", path, SocketRole::SERVER, 8U};
  ASSERT_EQ(first.status(), Status::OK);
  SocketChannel second{"//socket/second", path, SocketRole::SERVER, 8U};
  EXPECT_EQ(second.status(), Status::INVALID_TRANSPORT_PATH);
}

TEST(SocketChannelTest, EnforcesFrameLimitBeforeWriting) {
  const std::filesystem::path path = socket_path();
  SocketChannel server{"//socket/events", path, SocketRole::SERVER, 2U};
  ASSERT_EQ(server.status(), Status::OK);
  SocketChannel client{"//socket/events", path, SocketRole::CLIENT, 2U};
  ASSERT_EQ(client.status(), Status::OK);
  ASSERT_TRUE(wait_for([&server] { return server.connected(); }));
  constexpr std::array oversized = {std::uint8_t{1}, std::uint8_t{2},
                                    std::uint8_t{3}};
  EXPECT_EQ(client.transmit(oversized).status, Status::MESSAGE_TOO_LARGE);
}

TEST(SocketChannelTest, SerializesConcurrentWritersWithoutFrameInterleaving) {
  const std::filesystem::path path = socket_path();
  SocketChannel server{"//socket/events", path, SocketRole::SERVER, 8U};
  ASSERT_EQ(server.status(), Status::OK);
  SocketChannel client{"//socket/events", path, SocketRole::CLIENT, 8U};
  ASSERT_EQ(client.status(), Status::OK);
  ASSERT_TRUE(wait_for([&server] { return server.connected(); }));

  constexpr std::size_t thread_count      = 4U;
  constexpr std::size_t frames_per_thread = 100U;
  std::mutex receive_mutex;
  std::condition_variable received;
  std::vector<bool> seen(thread_count * frames_per_thread, false);
  std::size_t receive_count   = 0U;
  std::size_t malformed_count = 0U;
  Subscription subscription;
  ASSERT_EQ(server.subscribe(
                [&](Channel::Bytes bytes) noexcept {
                  const std::lock_guard lock(receive_mutex);
                  if (bytes.size() != 3U || bytes[0] >= thread_count) {
                    ++malformed_count;
                  } else {
                    const std::size_t sequence =
                        (static_cast<std::size_t>(bytes[1]) << 8U) | bytes[2];
                    if (sequence >= frames_per_thread) {
                      ++malformed_count;
                    } else {
                      seen[bytes[0] * frames_per_thread + sequence] = true;
                    }
                  }
                  ++receive_count;
                  received.notify_all();
                },
                subscription),
            Status::OK);

  std::atomic<bool> write_failed = false;
  std::vector<std::thread> writers;
  for (std::size_t thread = 0U; thread < thread_count; ++thread) {
    writers.emplace_back([&, thread] {
      for (std::size_t sequence = 0U; sequence < frames_per_thread;
           ++sequence) {
        const std::array payload = {
            static_cast<std::uint8_t>(thread),
            static_cast<std::uint8_t>(sequence >> 8U),
            static_cast<std::uint8_t>(sequence),
        };
        if (!is_ok(client.transmit(payload).status)) {
          write_failed.store(true, std::memory_order_relaxed);
        }
      }
    });
  }
  for (std::thread& writer : writers) {
    writer.join();
  }
  EXPECT_FALSE(write_failed.load(std::memory_order_relaxed));
  std::unique_lock lock(receive_mutex);
  ASSERT_TRUE(received.wait_for(lock, 2s, [&] {
    return receive_count == thread_count * frames_per_thread;
  }));
  EXPECT_EQ(malformed_count, 0U);
  for (const bool frame_seen : seen) {
    EXPECT_TRUE(frame_seen);
  }
}

TEST(SocketChannelTest, RemovesOnlyThePathItsServerSuccessfullyBound) {
  const std::filesystem::path path = socket_path();
  {
    SocketChannel server{"//socket/events", path, SocketRole::SERVER, 8U};
    ASSERT_EQ(server.status(), Status::OK);
    ASSERT_TRUE(std::filesystem::exists(path));
  }
  EXPECT_FALSE(std::filesystem::exists(path));
}

}  // namespace
}  // namespace puc::ipc
