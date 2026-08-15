/**
 * @file cmdframe_msgs_test.cpp
 * @brief Tests for the command-frame notification payload contract.
 */

#include "msgs/cmdframe_msgs.hpp"

#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace puc::msg {
namespace {

/** Deliberate IPC envelope limit used by registration tests. */
constexpr std::size_t kTestMaximumPayloadBytes = 1024U;

TEST(CmdFrameMessagesTest, ExposesCanonicalNotificationChannelAndMessageId) {
  EXPECT_EQ(kCmdFrameNotifyChannel, "//cmdframe/notify");
  EXPECT_EQ(to_wire_id(MessageId::CMD_FRAME_NOTIFICATION), 3U);
  static_assert(MessageValue<CmdFrameNotification>);
}

TEST(CmdFrameNotificationCodecTest, RoundTripsOwnedUtf8Text) {
  const CmdFrameNotification original{.text = "ready ✓\n"};
  CmdFrameNotificationCodec codec;
  std::vector<std::uint8_t> payload;
  ASSERT_EQ(codec.serialize(original, payload), Status::OK);
  EXPECT_EQ(std::string(payload.begin(), payload.end()), original.text);

  CmdFrameNotification decoded;
  EXPECT_EQ(codec.deserialize(payload, decoded), Status::OK);
  EXPECT_EQ(decoded, original);
  EXPECT_EQ(std::format("{}", CmdFrameNotification{.text = "ok"}),
            R"({"text_hex":"6f6b"})");
}

TEST(CmdFrameNotificationCodecTest, AcceptsEmptyTextForClearing) {
  CmdFrameNotificationCodec codec;
  std::vector<std::uint8_t> payload;
  ASSERT_EQ(codec.serialize(CmdFrameNotification{}, payload), Status::OK);
  EXPECT_TRUE(payload.empty());

  CmdFrameNotification decoded{.text = "stale"};
  EXPECT_EQ(codec.deserialize(payload, decoded), Status::OK);
  EXPECT_TRUE(decoded.text.empty());
}

TEST(CmdFrameNotificationCodecTest, RejectsMalformedUtf8) {
  CmdFrameNotificationCodec codec;
  const CmdFrameNotification malformed{
      .text = std::string{"\xf0\x28\x8c\x28", 4U},
  };
  std::vector<std::uint8_t> payload;
  EXPECT_EQ(codec.serialize(malformed, payload),
            Status::PAYLOAD_ENCODING_FAILED);
  EXPECT_TRUE(payload.empty());

  constexpr std::array malformed_payload = {
      std::uint8_t{0xedU}, std::uint8_t{0xa0U}, std::uint8_t{0x80U}};
  CmdFrameNotification decoded;
  EXPECT_EQ(codec.deserialize(malformed_payload, decoded),
            Status::MALFORMED_PAYLOAD);
  EXPECT_TRUE(decoded.text.empty());
}

TEST(CmdFrameMessagesTest, RegistersNotificationCodec) {
  MessageCodecCollection codecs{kTestMaximumPayloadBytes};
  EXPECT_EQ(register_cmdframe_codecs(codecs), Status::OK);
  EXPECT_EQ(codecs.size(), 2U);
  EXPECT_EQ(register_cmdframe_codecs(codecs), Status::DUPLICATE_MESSAGE_ID);

  const CmdFrameNotification notification{.text = "working"};
  std::vector<std::uint8_t> payload;
  EXPECT_EQ(codecs.serialize(MessageId::CMD_FRAME_NOTIFICATION, notification,
                             payload),
            Status::OK);
}

}  // namespace
}  // namespace puc::msg
