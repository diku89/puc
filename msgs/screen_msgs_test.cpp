/**
 * @file screen_msgs_test.cpp
 * @brief Tests for the one-way Screen/TerminalSession payload contract.
 */

#include "msgs/screen_msgs.hpp"

#include <array>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace puc::msg {
namespace {

TEST(ScreenMessagesTest, ExposesCanonicalDirectionSpecificChannels) {
  EXPECT_EQ(kScreenCommandChannel, "//screen/commands");
  EXPECT_EQ(kScreenResizeEventChannel, "//screen/resize_events");
}

TEST(ScreenMessagesTest, MessageValuesMeetTheGenericCodecContract) {
  static_assert(MessageValue<ScreenCommand>);
  static_assert(MessageValue<ScreenResizeEvent>);
}

TEST(ScreenCommandCodecTest, TakeCommandRoundTripsEveryOption) {
  const ScreenCommand original{
      .data =
          ScreenTakeCommand{
              .options =
                  ScreenSessionOptions{
                      .preserve_signals     = false,
                      .alternate_screen     = true,
                      .hide_cursor          = true,
                      .disable_auto_wrap    = true,
                      .bracketed_paste      = true,
                      .focus_reporting      = true,
                      .mouse                = ScreenMouseTracking::DRAG,
                      .kitty_keyboard_flags = 0x12345678U,
                  },
              .initial_bytes = "clear",
              .final_bytes   = "reset",
          },
  };
  ScreenCommandCodec codec;
  std::vector<std::uint8_t> payload;
  ASSERT_EQ(codec.serialize(original, payload), Status::OK);
  EXPECT_EQ(payload.size(), 25U);

  ScreenCommand decoded;
  EXPECT_EQ(codec.deserialize(payload, decoded), Status::OK);
  EXPECT_EQ(decoded, original);
  EXPECT_EQ(
      std::format("{}", decoded),
      R"({"type":"take","preserve_signals":false,"alternate_screen":true,"hide_cursor":true,"disable_auto_wrap":true,"bracketed_paste":true,"focus_reporting":true,"mouse":"drag","kitty_keyboard_flags":305419896,"initial_bytes_hex":"636c656172","final_bytes_hex":"7265736574"})");
}

TEST(ScreenCommandCodecTest, EmptyReleaseCommandUsesOneByte) {
  const ScreenCommand original{.data = ScreenReleaseCommand{}};
  ScreenCommandCodec codec;
  std::vector<std::uint8_t> payload;
  ASSERT_EQ(codec.serialize(original, payload), Status::OK);
  ASSERT_EQ(payload.size(), 1U);

  ScreenCommand decoded;
  EXPECT_EQ(codec.deserialize(payload, decoded), Status::OK);
  EXPECT_EQ(decoded, original);
  EXPECT_EQ(std::format("{}", decoded), R"({"type":"release"})");
}

TEST(ScreenCommandCodecTest, PresentCommandOwnsAndRoundTripsArbitraryBytes) {
  const ScreenCommand original{
      .data = ScreenPresentCommand{.bytes = std::string{"\x1b\0A", 3U}},
  };
  ScreenCommandCodec codec;
  std::vector<std::uint8_t> payload;
  ASSERT_EQ(codec.serialize(original, payload), Status::OK);
  EXPECT_EQ(payload.size(), 8U);

  ScreenCommand decoded;
  EXPECT_EQ(codec.deserialize(payload, decoded), Status::OK);
  EXPECT_EQ(decoded, original);
  EXPECT_EQ(std::format("{}", decoded),
            R"({"type":"present","bytes_hex":"1b0041"})");
}

TEST(ScreenCommandCodecTest, ClipboardCommandOwnsAndRoundTripsUtf8Bytes) {
  const ScreenCommand original{
      .data =
          ScreenSetClipboardCommand{
              .selection = ScreenClipboardSelection::PRIMARY,
              .text      = "ನಮಸ್ಕಾರ",
          },
  };
  ScreenCommandCodec codec;
  std::vector<std::uint8_t> payload;
  ASSERT_EQ(codec.serialize(original, payload), Status::OK);
  EXPECT_EQ(payload.size(), 27U);

  ScreenCommand decoded;
  EXPECT_EQ(codec.deserialize(payload, decoded), Status::OK);
  EXPECT_EQ(decoded, original);
  EXPECT_EQ(
      std::format("{}", decoded),
      R"({"type":"set_clipboard","selection":"primary","text_hex":"e0b2a8e0b2aee0b2b8e0b38de0b295e0b2bee0b2b0"})");
}

TEST(ScreenCommandCodecTest, RejectsUnknownTypesFlagsAndTrailingBytes) {
  ScreenCommandCodec codec;
  ScreenCommand decoded;
  constexpr std::array unknown   = {std::uint8_t{99U}};
  constexpr std::array bad_flags = {
      std::uint8_t{1U}, std::uint8_t{0x80U}, std::uint8_t{0U}, std::uint8_t{0U},
      std::uint8_t{0U}, std::uint8_t{0U},    std::uint8_t{0U}, std::uint8_t{0U},
      std::uint8_t{0U}, std::uint8_t{0U},    std::uint8_t{0U}, std::uint8_t{0U},
      std::uint8_t{0U}, std::uint8_t{0U},    std::uint8_t{0U},
  };
  constexpr std::array release_with_trailer = {std::uint8_t{2U},
                                               std::uint8_t{0U}};
  constexpr std::array truncated_present    = {
      std::uint8_t{3U}, std::uint8_t{0U}, std::uint8_t{0U},
      std::uint8_t{0U}, std::uint8_t{2U}, std::uint8_t{'A'},
  };
  constexpr std::array bad_clipboard = {
      std::uint8_t{4U}, std::uint8_t{99U}, std::uint8_t{0U},
      std::uint8_t{0U}, std::uint8_t{0U},  std::uint8_t{0U},
  };
  EXPECT_EQ(codec.deserialize({}, decoded), Status::MALFORMED_PAYLOAD);
  EXPECT_EQ(codec.deserialize(unknown, decoded), Status::MALFORMED_PAYLOAD);
  EXPECT_EQ(codec.deserialize(bad_flags, decoded), Status::MALFORMED_PAYLOAD);
  EXPECT_EQ(codec.deserialize(release_with_trailer, decoded),
            Status::MALFORMED_PAYLOAD);
  EXPECT_EQ(codec.deserialize(truncated_present, decoded),
            Status::MALFORMED_PAYLOAD);
  EXPECT_EQ(codec.deserialize(bad_clipboard, decoded),
            Status::MALFORMED_PAYLOAD);
}

TEST(ScreenCommandCodecTest, RejectsInvalidMouseEnumOnEncodeAndDecode) {
  ScreenCommand invalid{
      .data =
          ScreenTakeCommand{
              .options =
                  ScreenSessionOptions{
                      .mouse = static_cast<ScreenMouseTracking>(99U),
                  },
          },
  };
  ScreenCommandCodec codec;
  std::vector<std::uint8_t> payload;
  EXPECT_EQ(codec.serialize(invalid, payload), Status::PAYLOAD_ENCODING_FAILED);
  EXPECT_TRUE(payload.empty());

  constexpr std::array bad_mouse = {
      std::uint8_t{1U}, std::uint8_t{0U}, std::uint8_t{99U}, std::uint8_t{0U},
      std::uint8_t{0U}, std::uint8_t{0U}, std::uint8_t{0U},  std::uint8_t{0U},
      std::uint8_t{0U}, std::uint8_t{0U}, std::uint8_t{0U},  std::uint8_t{0U},
      std::uint8_t{0U}, std::uint8_t{0U}, std::uint8_t{0U},
  };
  ScreenCommand decoded;
  EXPECT_EQ(codec.deserialize(bad_mouse, decoded), Status::MALFORMED_PAYLOAD);
}

TEST(ScreenResizeEventCodecTest, UsesFixedWidthPortablePayloadAndJson) {
  constexpr ScreenResizeEvent original{
      .width        = 137U,
      .height       = 27U,
      .pixel_width  = 1918U,
      .pixel_height = 864U,
  };
  ScreenResizeEventCodec codec;
  std::vector<std::uint8_t> payload;
  ASSERT_EQ(codec.serialize(original, payload), Status::OK);
  EXPECT_EQ(payload.size(), 16U);
  EXPECT_EQ(payload[3], 137U);

  ScreenResizeEvent decoded;
  EXPECT_EQ(codec.deserialize(payload, decoded), Status::OK);
  EXPECT_EQ(decoded, original);
  EXPECT_EQ(
      std::format("{}", decoded),
      R"({"width":137,"height":27,"pixel_width":1918,"pixel_height":864})");
}

TEST(ScreenResizeEventCodecTest, RejectsZeroAndMalformedDimensions) {
  ScreenResizeEventCodec codec;
  std::vector<std::uint8_t> payload;
  EXPECT_EQ(codec.serialize(ScreenResizeEvent{}, payload),
            Status::PAYLOAD_ENCODING_FAILED);
  EXPECT_TRUE(payload.empty());

  constexpr std::array short_payload = {std::uint8_t{0U}};
  ScreenResizeEvent decoded;
  EXPECT_EQ(codec.deserialize(short_payload, decoded),
            Status::MALFORMED_PAYLOAD);

  const std::array<std::uint8_t, 16U> zero_width{};
  EXPECT_EQ(codec.deserialize(zero_width, decoded), Status::MALFORMED_PAYLOAD);
}

TEST(ScreenMessagesTest, RegistersBothCodecsWithoutImplicitReplies) {
  MessageCodecCollection codecs;
  ASSERT_EQ(register_screen_codecs(codecs), Status::OK);
  EXPECT_EQ(codecs.size(), 3U);
  EXPECT_EQ(register_screen_codecs(codecs), Status::DUPLICATE_MESSAGE_ID);

  const ScreenCommand command{.data = ScreenReleaseCommand{}};
  std::vector<std::uint8_t> payload;
  EXPECT_EQ(codecs.serialize(MessageId::SCREEN_COMMAND, command, payload),
            Status::OK);
}

}  // namespace
}  // namespace puc::msg
