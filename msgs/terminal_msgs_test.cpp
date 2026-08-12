/**
 * @file terminal_msgs_test.cpp
 * @brief Tests for normalized terminal-input message encoding.
 */

#include "msgs/terminal_msgs.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace puc::msg {
namespace {

TEST(TerminalMessagesTest, RoundTripsEveryEventAlternative) {
  const std::array<TerminalInputEvent, 10U> events{{
      TerminalInputEvent{.data = TerminalKeyEvent{.named           = false,
                                                  .key             = U'x',
                                                  .modifiers       = 5U,
                                                  .action          = 1U,
                                                  .shifted_key     = U'X',
                                                  .base_layout_key = U'x',
                                                  .text            = "x"}},
      TerminalInputEvent{.data = TerminalTextEvent{.utf8 = "hello ✓"}},
      TerminalInputEvent{.data = TerminalMouseEvent{.x         = 3U,
                                                    .y         = 4U,
                                                    .button    = 1U,
                                                    .action    = 3U,
                                                    .modifiers = 2U}},
      TerminalInputEvent{.data = TerminalScrollEvent{.x         = 8U,
                                                     .y         = 9U,
                                                     .delta_x   = -2,
                                                     .delta_y   = 3,
                                                     .modifiers = 4U}},
      TerminalInputEvent{.data =
                             TerminalPasteEvent{.phase = 1U, .data = "paste"}},
      TerminalInputEvent{.data = TerminalFocusEvent{.focused = true}},
      TerminalInputEvent{
          .data = TerminalClipboardEvent{.selection = 1U, .data = "clipboard"}},
      TerminalInputEvent{.data = TerminalCommandEvent{.command = 4U}},
      TerminalInputEvent{.data = TerminalResponseEvent{.kind  = 2U,
                                                       .value = 42U,
                                                       .bytes = "response"}},
      TerminalInputEvent{.data =
                             TerminalUnknownEvent{.reason = 3U, .bytes = "?"}},
  }};

  TerminalInputEventCodec codec;
  for (const TerminalInputEvent& expected : events) {
    std::vector<std::uint8_t> payload;
    ASSERT_EQ(codec.serialize(expected, payload), Status::OK);
    TerminalInputEvent decoded;
    ASSERT_EQ(codec.deserialize(payload, decoded), Status::OK);
    EXPECT_EQ(decoded, expected);
  }
}

TEST(TerminalMessagesTest, RejectsMalformedAndOutOfRangeValues) {
  TerminalInputEventCodec codec;
  TerminalInputEvent decoded;
  EXPECT_EQ(codec.deserialize({}, decoded), Status::MALFORMED_PAYLOAD);
  const std::array<std::uint8_t, 2U> invalid_tag{99U, 0U};
  EXPECT_EQ(codec.deserialize(invalid_tag, decoded), Status::MALFORMED_PAYLOAD);

  const TerminalInputEvent invalid{
      .data = TerminalKeyEvent{.named = false, .key = 0xd800U}};
  std::vector<std::uint8_t> payload;
  EXPECT_EQ(codec.serialize(invalid, payload), Status::PAYLOAD_ENCODING_FAILED);
  EXPECT_TRUE(payload.empty());
}

TEST(TerminalMessagesTest, RegistersCanonicalMessageIdentifier) {
  MessageCodecCollection codecs;
  ASSERT_EQ(register_terminal_codecs(codecs), Status::OK);
  EXPECT_EQ(codecs.size(), 2U);
  const TerminalInputEvent event{.data = TerminalCommandEvent{.command = 1U}};
  std::vector<std::uint8_t> payload;
  EXPECT_EQ(codecs.serialize(MessageId::TERMINAL_INPUT_EVENT, event, payload),
            Status::OK);
}

}  // namespace
}  // namespace puc::msg
