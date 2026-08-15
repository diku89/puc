/**
 * @file filebuffer_channel_test.cpp
 * @brief Tests for framed IPC over pairs of POSIX named pipes.
 */

#include "utils/ipc/filebuffer_channel.hpp"

#include <sys/stat.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "gtest/gtest.h"

namespace puc::ipc {
namespace {

using namespace std::chrono_literals;

class FileBufferChannelTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char* temporary_directory = std::getenv("TEST_TMPDIR");
    ASSERT_NE(temporary_directory, nullptr);
    const ::testing::TestInfo* info =
        ::testing::UnitTest::GetInstance()->current_test_info();
    ASSERT_NE(info, nullptr);
    root_ = std::filesystem::path{temporary_directory} /
            (std::string{"fifo_"} + info->name());
    std::error_code error;
    static_cast<void>(std::filesystem::remove_all(root_, error));
    error.clear();
    ASSERT_TRUE(std::filesystem::create_directories(root_, error));
    ASSERT_FALSE(error);
    first_fifo_  = root_ / "first.fifo";
    second_fifo_ = root_ / "second.fifo";
    ASSERT_EQ(::mkfifo(first_fifo_.string().c_str(), 0600), 0);
    ASSERT_EQ(::mkfifo(second_fifo_.string().c_str(), 0600), 0);
  }

  void write_regular_file(const std::filesystem::path& path,
                          std::string_view contents) {
    FILE* file = std::fopen(path.string().c_str(), "wb");
    ASSERT_NE(file, nullptr);
    EXPECT_EQ(std::fwrite(contents.data(), 1U, contents.size(), file),
              contents.size());
    EXPECT_EQ(std::fclose(file), 0);
  }

  std::filesystem::path root_;
  std::filesystem::path first_fifo_;
  std::filesystem::path second_fifo_;
};

TEST_F(FileBufferChannelTest, DeliversBidirectionallyAndPreservesBoundaries) {
  FileBufferChannel first{"//fifo/events", first_fifo_, second_fifo_, 64U};
  FileBufferChannel second{"//fifo/events", second_fifo_, first_fifo_, 64U};
  ASSERT_EQ(first.status(), Status::OK);
  ASSERT_EQ(second.status(), Status::OK);
  EXPECT_EQ(first.read_path(), first_fifo_);
  EXPECT_EQ(first.write_path(), second_fifo_);
  EXPECT_EQ(first.maximum_message_bytes(), 64U);

  std::mutex receive_mutex;
  std::condition_variable received;
  std::vector<std::vector<std::uint8_t>> at_first;
  std::vector<std::vector<std::uint8_t>> at_second;
  Subscription first_subscription;
  Subscription second_subscription;
  ASSERT_EQ(first.subscribe(
                [&](Channel::Bytes bytes) noexcept {
                  const std::lock_guard lock(receive_mutex);
                  at_first.emplace_back(bytes.begin(), bytes.end());
                  received.notify_all();
                },
                first_subscription),
            Status::OK);
  ASSERT_EQ(second.subscribe(
                [&](Channel::Bytes bytes) noexcept {
                  const std::lock_guard lock(receive_mutex);
                  at_second.emplace_back(bytes.begin(), bytes.end());
                  received.notify_all();
                },
                second_subscription),
            Status::OK);

  constexpr std::array first_payload  = {std::uint8_t{1}, std::uint8_t{2},
                                         std::uint8_t{3}};
  constexpr std::array second_payload = {std::uint8_t{8}, std::uint8_t{9}};
  EXPECT_EQ(
      first.transmit(first_payload),
      (TransferResult{.status = Status::OK, .bytes = first_payload.size()}));
  EXPECT_EQ(
      second.transmit(second_payload),
      (TransferResult{.status = Status::OK, .bytes = second_payload.size()}));
  EXPECT_EQ(first.transmit({}),
            (TransferResult{.status = Status::OK, .bytes = 0U}));

  std::unique_lock lock(receive_mutex);
  ASSERT_TRUE(received.wait_for(lock, 2s, [&] {
    return at_first.size() == 1U && at_second.size() == 2U;
  }));
  ASSERT_EQ(at_first.size(), 1U);
  EXPECT_EQ(at_first[0], (std::vector<std::uint8_t>{second_payload.begin(),
                                                    second_payload.end()}));
  ASSERT_EQ(at_second.size(), 2U);
  EXPECT_EQ(at_second[0], (std::vector<std::uint8_t>{first_payload.begin(),
                                                     first_payload.end()}));
  EXPECT_TRUE(at_second[1].empty());
}

TEST_F(FileBufferChannelTest, EnforcesConfiguredMessageLimit) {
  FileBufferChannel channel{"//fifo/events", first_fifo_, second_fifo_, 2U};
  ASSERT_EQ(channel.status(), Status::OK);
  constexpr std::array oversized = {std::uint8_t{1}, std::uint8_t{2},
                                    std::uint8_t{3}};
  EXPECT_EQ(channel.transmit(oversized).status, Status::MESSAGE_TOO_LARGE);
}

TEST_F(FileBufferChannelTest, RejectsMissingOrdinaryAndIdenticalPaths) {
  FileBufferChannel missing{"//fifo/missing", root_ / "missing", second_fifo_,
                            8U};
  EXPECT_EQ(missing.status(), Status::INVALID_TRANSPORT_PATH);

  const std::filesystem::path regular = root_ / "regular";
  write_regular_file(regular, "not a fifo");
  FileBufferChannel ordinary{"//fifo/regular", regular, second_fifo_, 8U};
  EXPECT_EQ(ordinary.status(), Status::INVALID_TRANSPORT_PATH);

  FileBufferChannel identical{"//fifo/same", first_fifo_, first_fifo_, 8U};
  EXPECT_EQ(identical.status(), Status::INVALID_TRANSPORT_PATH);

  const std::filesystem::path alias = root_ / "first_alias.fifo";
  std::error_code symlink_error;
  std::filesystem::create_symlink(first_fifo_, alias, symlink_error);
  if (!symlink_error) {
    FileBufferChannel aliased{"//fifo/alias", first_fifo_, alias, 8U};
    EXPECT_EQ(aliased.status(), Status::INVALID_TRANSPORT_PATH);
  }

  FileBufferChannel zero_limit{"//fifo/zero", first_fifo_, second_fifo_, 0U};
  EXPECT_EQ(zero_limit.status(), Status::INVALID_ARGUMENT);
}

TEST_F(FileBufferChannelTest, LeavesCallerOwnedFifosInPlace) {
  {
    FileBufferChannel channel{"//fifo/events", first_fifo_, second_fifo_, 8U};
    ASSERT_EQ(channel.status(), Status::OK);
  }
  EXPECT_TRUE(std::filesystem::is_fifo(first_fifo_));
  EXPECT_TRUE(std::filesystem::is_fifo(second_fifo_));
}

}  // namespace
}  // namespace puc::ipc
