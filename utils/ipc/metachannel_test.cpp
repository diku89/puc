/**
 * @file metachannel_test.cpp
 * @brief Tests for multi-transport IPC channel composition.
 */

#include "utils/ipc/metachannel.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

namespace puc::ipc {
namespace {

/** Controllable channel used to observe metachannel fan-out and fan-in. */
class FakeChannel final : public Channel {
 public:
  explicit FakeChannel(std::string name, Status transmit_status = Status::OK,
                       bool echo = false)
      : Channel(std::move(name)),
        transmit_status_(transmit_status),
        echo_(echo) {}

  TransferResult transmit(Bytes data) noexcept override {
    ++transmit_calls_;
    transmitted_.emplace_back(data.begin(), data.end());
    if (!is_ok(transmit_status_)) {
      return TransferResult{.status = transmit_status_};
    }
    if (echo_) {
      deliver(data);
    }
    return TransferResult{.status = Status::OK, .bytes = data.size()};
  }

  void receive(Bytes data) noexcept { deliver(data); }

  void make_unavailable(Status status) noexcept { set_status(status); }

  std::size_t transmit_calls() const noexcept { return transmit_calls_; }
  const std::vector<std::vector<std::uint8_t>>& transmitted() const noexcept {
    return transmitted_;
  }

 private:
  Status transmit_status_     = Status::OK;
  bool echo_                  = false;
  std::size_t transmit_calls_ = 0U;
  std::vector<std::vector<std::uint8_t>> transmitted_;
};

TEST(MetaChannelTest, FansOutInConfiguredOrderAndReportsFullSuccess) {
  auto first  = std::make_shared<FakeChannel>("//physical/first");
  auto second = std::make_shared<FakeChannel>("//physical/second");
  MetaChannel meta{"//screen/events", {first, second}};
  ASSERT_EQ(meta.status(), Status::OK);
  EXPECT_EQ(meta.destination_count(), 2U);
  ASSERT_EQ(meta.underlying_channels().size(), 2U);
  EXPECT_EQ(meta.underlying_channels()[0], first);
  EXPECT_EQ(meta.underlying_channels()[1], second);

  constexpr std::array payload = {std::uint8_t{1}, std::uint8_t{2}};
  EXPECT_EQ(meta.transmit(payload),
            (TransferResult{.status = Status::OK, .bytes = payload.size()}));
  EXPECT_EQ(first->transmit_calls(), 1U);
  EXPECT_EQ(second->transmit_calls(), 1U);
  EXPECT_EQ(first->transmitted()[0],
            (std::vector<std::uint8_t>{payload.begin(), payload.end()}));
  EXPECT_EQ(second->transmitted()[0],
            (std::vector<std::uint8_t>{payload.begin(), payload.end()}));
}

TEST(MetaChannelTest, RelaysIncomingMessagesFromEveryUnderlyingChannel) {
  auto first  = std::make_shared<FakeChannel>("//physical/first");
  auto second = std::make_shared<FakeChannel>("//physical/second");
  MetaChannel meta{"//screen/events", {first, second}};
  ASSERT_EQ(meta.status(), Status::OK);
  std::vector<std::vector<std::uint8_t>> received;
  Subscription subscription;
  ASSERT_EQ(meta.subscribe(
                [&received](Channel::Bytes bytes) noexcept {
                  received.emplace_back(bytes.begin(), bytes.end());
                },
                subscription),
            Status::OK);

  constexpr std::array first_message  = {std::uint8_t{1}};
  constexpr std::array second_message = {std::uint8_t{2}, std::uint8_t{3}};
  first->receive(first_message);
  second->receive(second_message);
  ASSERT_EQ(received.size(), 2U);
  EXPECT_EQ(received[0], (std::vector<std::uint8_t>{first_message.begin(),
                                                    first_message.end()}));
  EXPECT_EQ(received[1], (std::vector<std::uint8_t>{second_message.begin(),
                                                    second_message.end()}));
}

TEST(MetaChannelTest, EchoingDestinationsEachContributeAnInboundDelivery) {
  auto first =
      std::make_shared<FakeChannel>("//physical/first", Status::OK, true);
  auto second =
      std::make_shared<FakeChannel>("//physical/second", Status::OK, true);
  MetaChannel meta{"//screen/events", {first, second}};
  std::size_t receives = 0U;
  Subscription subscription;
  ASSERT_EQ(meta.subscribe([&receives](Channel::Bytes) noexcept { ++receives; },
                           subscription),
            Status::OK);
  constexpr std::array payload = {std::uint8_t{1}};
  EXPECT_EQ(meta.transmit(payload).status, Status::OK);
  EXPECT_EQ(receives, 2U);
}

TEST(MetaChannelTest, EchoRelayPermitsReentrantTransmission) {
  auto channel =
      std::make_shared<FakeChannel>("//physical/echo", Status::OK, true);
  MetaChannel meta{"//screen/events", {channel}};
  std::size_t receives = 0U;
  Subscription subscription;
  ASSERT_EQ(meta.subscribe(
                [&meta, &receives](Channel::Bytes bytes) noexcept {
                  ++receives;
                  if (receives == 1U) {
                    static_cast<void>(meta.transmit(bytes));
                  }
                },
                subscription),
            Status::OK);
  constexpr std::array payload = {std::uint8_t{1}};
  EXPECT_EQ(meta.transmit(payload).status, Status::OK);
  EXPECT_EQ(receives, 2U);
}

TEST(MetaChannelTest, DistinguishesPartialAndCompleteFanoutFailure) {
  auto successful = std::make_shared<FakeChannel>("//physical/ok");
  auto failed =
      std::make_shared<FakeChannel>("//physical/fail", Status::IO_ERROR);
  MetaChannel partial{"//screen/partial", {successful, failed}};
  ASSERT_EQ(partial.status(), Status::OK);
  constexpr std::array payload = {std::uint8_t{1}};
  EXPECT_EQ(partial.transmit(payload),
            (TransferResult{.status = Status::PARTIAL_TRANSFER}));

  auto also_failed =
      std::make_shared<FakeChannel>("//physical/fail2", Status::NOT_CONNECTED);
  MetaChannel complete_failure{"//screen/failure", {failed, also_failed}};
  ASSERT_EQ(complete_failure.status(), Status::OK);
  EXPECT_EQ(complete_failure.transmit(payload),
            (TransferResult{.status = Status::IO_ERROR}));
}

TEST(MetaChannelTest, RejectsEmptyNullDuplicateAndUnavailableDestinations) {
  MetaChannel empty{"//meta/empty", {}};
  EXPECT_EQ(empty.status(), Status::INVALID_ARGUMENT);

  MetaChannel null_channel{"//meta/null", {nullptr}};
  EXPECT_EQ(null_channel.status(), Status::INVALID_ARGUMENT);

  auto channel = std::make_shared<FakeChannel>("//physical/one");
  MetaChannel duplicate{"//meta/duplicate", {channel, channel}};
  EXPECT_EQ(duplicate.status(), Status::INVALID_ARGUMENT);

  auto unavailable = std::make_shared<FakeChannel>("//physical/unavailable");
  unavailable->make_unavailable(Status::CHANNEL_UNAVAILABLE);
  MetaChannel invalid{"//meta/unavailable", {unavailable}};
  EXPECT_EQ(invalid.status(), Status::CHANNEL_UNAVAILABLE);
  EXPECT_EQ(unavailable->subscriber_count(), 0U);
}

TEST(MetaChannelTest, DestructionDisablesPrivateUnderlyingSubscriptions) {
  auto channel = std::make_shared<FakeChannel>("//physical/one");
  {
    MetaChannel meta{"//screen/events", {channel}};
    ASSERT_EQ(meta.status(), Status::OK);
    EXPECT_EQ(channel->subscriber_count(), 1U);
  }
  EXPECT_EQ(channel->subscriber_count(), 0U);
  constexpr std::array payload = {std::uint8_t{1}};
  channel->receive(payload);
}

}  // namespace
}  // namespace puc::ipc
