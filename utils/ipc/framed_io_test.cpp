/**
 * @file framed_io_test.cpp
 * @brief Tests for internal length-prefixed stream operations.
 */

#include "utils/ipc/framed_io.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

namespace puc::ipc::detail {
namespace {

/** Own both descriptors in one socketpair for a single test scope. */
class SocketPair {
 public:
  SocketPair() {
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors_.data()) == 0) {
      status_ = configure_socket_descriptor(descriptors_[0]);
      if (is_ok(status_)) {
        status_ = configure_socket_descriptor(descriptors_[1]);
      }
    }
  }

  SocketPair(const SocketPair&)            = delete;
  SocketPair& operator=(const SocketPair&) = delete;

  ~SocketPair() {
    for (const int descriptor : descriptors_) {
      if (descriptor >= 0) {
        static_cast<void>(::close(descriptor));
      }
    }
  }

  int first() const noexcept { return descriptors_[0]; }
  int second() const noexcept { return descriptors_[1]; }
  Status status() const noexcept { return status_; }

 private:
  std::array<int, 2U> descriptors_ = {-1, -1};
  Status status_                   = Status::IO_ERROR;
};

TEST(FramedIoTest, TransfersLargeFramesAcrossPartialNonblockingOperations) {
  SocketPair sockets;
  ASSERT_EQ(sockets.status(), Status::OK);
  std::vector<std::uint8_t> payload(1024U * 1024U);
  for (std::size_t index = 0U; index < payload.size(); ++index) {
    payload[index] = static_cast<std::uint8_t>(index & 0xffU);
  }
  std::atomic<bool> stopping = false;
  std::vector<std::uint8_t> received;
  Status read_status = Status::IO_ERROR;
  std::thread reader([&] {
    read_status = read_frame(sockets.second(), received, payload.size(),
                             StreamKind::SOCKET, stopping);
  });
  const TransferResult written = write_frame(
      sockets.first(), payload, payload.size(), StreamKind::SOCKET, stopping);
  reader.join();
  EXPECT_EQ(written,
            (TransferResult{.status = Status::OK, .bytes = payload.size()}));
  EXPECT_EQ(read_status, Status::OK);
  EXPECT_EQ(received, payload);
}

TEST(FramedIoTest, ReassemblesAHeaderAndPayloadSentInFragments) {
  SocketPair sockets;
  ASSERT_EQ(sockets.status(), Status::OK);
  constexpr std::array first_fragment  = {std::uint8_t{0}, std::uint8_t{0}};
  constexpr std::array second_fragment = {std::uint8_t{0}, std::uint8_t{3},
                                          std::uint8_t{4}};
  constexpr std::array third_fragment  = {std::uint8_t{5}, std::uint8_t{6}};
  ASSERT_EQ(
      ::send(sockets.first(), first_fragment.data(), first_fragment.size(), 0),
      static_cast<ssize_t>(first_fragment.size()));
  ASSERT_EQ(::send(sockets.first(), second_fragment.data(),
                   second_fragment.size(), 0),
            static_cast<ssize_t>(second_fragment.size()));
  ASSERT_EQ(
      ::send(sockets.first(), third_fragment.data(), third_fragment.size(), 0),
      static_cast<ssize_t>(third_fragment.size()));

  std::atomic<bool> stopping = false;
  std::vector<std::uint8_t> received;
  EXPECT_EQ(
      read_frame(sockets.second(), received, 3U, StreamKind::SOCKET, stopping),
      Status::OK);
  EXPECT_EQ(received, (std::vector<std::uint8_t>{
                          std::uint8_t{4}, std::uint8_t{5}, std::uint8_t{6}}));
}

TEST(FramedIoTest, RejectsOversizedLengthBeforeAllocatingPayload) {
  SocketPair sockets;
  ASSERT_EQ(sockets.status(), Status::OK);
  constexpr std::array prefix = {std::uint8_t{0}, std::uint8_t{0},
                                 std::uint8_t{1}, std::uint8_t{0}};
  ASSERT_EQ(::send(sockets.first(), prefix.data(), prefix.size(), 0),
            static_cast<ssize_t>(prefix.size()));
  std::atomic<bool> stopping         = false;
  std::vector<std::uint8_t> received = {9U};
  EXPECT_EQ(read_frame(sockets.second(), received, 255U, StreamKind::SOCKET,
                       stopping),
            Status::MESSAGE_TOO_LARGE);
  EXPECT_TRUE(received.empty());
}

TEST(FramedIoTest, ReportsPeerClosureInsideAFrameAsTruncation) {
  SocketPair sockets;
  ASSERT_EQ(sockets.status(), Status::OK);
  constexpr std::array incomplete = {
      std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{0},
      std::uint8_t{3}, std::uint8_t{1},
  };
  ASSERT_EQ(::send(sockets.first(), incomplete.data(), incomplete.size(), 0),
            static_cast<ssize_t>(incomplete.size()));
  ASSERT_EQ(::shutdown(sockets.first(), SHUT_WR), 0);
  std::atomic<bool> stopping = false;
  std::vector<std::uint8_t> received;
  EXPECT_EQ(
      read_frame(sockets.second(), received, 3U, StreamKind::SOCKET, stopping),
      Status::TRUNCATED_MESSAGE);
}

TEST(FramedIoTest, ObservesCancellationAndInvalidDescriptors) {
  std::atomic<bool> stopping = true;
  std::vector<std::uint8_t> received;
  EXPECT_EQ(read_frame(-1, received, 1U, StreamKind::SOCKET, stopping),
            Status::NOT_CONNECTED);
  EXPECT_EQ(write_frame(-1, {}, 1U, StreamKind::SOCKET, stopping).status,
            Status::NOT_CONNECTED);

  SocketPair sockets;
  ASSERT_EQ(sockets.status(), Status::OK);
  EXPECT_EQ(
      read_frame(sockets.first(), received, 1U, StreamKind::SOCKET, stopping),
      Status::CHANNEL_UNAVAILABLE);
}

}  // namespace
}  // namespace puc::ipc::detail
