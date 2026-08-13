/**
 * @file clipboard_test.cpp
 * @brief Tests for bounded OSC 52 clipboard request and response handling.
 */

#include "puc-cli/tui/terminal/clipboard.hpp"

#include <array>
#include <string>
#include <string_view>

#include "gtest/gtest.h"

namespace puc::terminal {
namespace {

struct ClipboardEncoding {
  std::string_view plain;
  std::string_view encoded;
};

TEST(TerminalClipboardTest, WritesCanonicalPaddedBase64) {
  constexpr std::array encodings{
      ClipboardEncoding{"", ""},
      ClipboardEncoding{"f", "Zg=="},
      ClipboardEncoding{"fo", "Zm8="},
      ClipboardEncoding{"foo", "Zm9v"},
      ClipboardEncoding{"foobar", "Zm9vYmFy"},
  };

  for (const ClipboardEncoding& encoding : encodings) {
    std::string output;
    ASSERT_EQ(build_clipboard_write(ClipboardSelection::CLIPBOARD,
                                    encoding.plain, output),
              Status::OK);
    EXPECT_EQ(output, "\x1b]52;c;" + std::string{encoding.encoded} + "\x1b\\");
  }
}

TEST(TerminalClipboardTest, PreservesBinaryBytesThroughBase64) {
  const std::string bytes{"\0\xff\x10", 3};
  std::string output;
  ASSERT_EQ(build_clipboard_write(ClipboardSelection::PRIMARY, bytes, output),
            Status::OK);
  EXPECT_EQ(output, "\x1b]52;p;AP8Q\x1b\\");

  ClipboardEvent event;
  ASSERT_EQ(
      decode_clipboard_payload(ClipboardSelection::PRIMARY, "AP8Q", event),
      Status::OK);
  EXPECT_EQ(event.selection, ClipboardSelection::PRIMARY);
  EXPECT_EQ(event.data, bytes);
}

TEST(TerminalClipboardTest, EncodesAndDecodesBothNonAlphanumericSymbols) {
  const std::string bytes{"\xfb\xff", 2};
  std::string output;
  ASSERT_EQ(build_clipboard_write(ClipboardSelection::CLIPBOARD, bytes, output),
            Status::OK);
  EXPECT_EQ(output, "\x1b]52;c;+/8=\x1b\\");

  ClipboardEvent event;
  ASSERT_EQ(
      decode_clipboard_payload(ClipboardSelection::CLIPBOARD, "+/8=", event),
      Status::OK);
  EXPECT_EQ(event.data, bytes);
}

TEST(TerminalClipboardTest, QueriesUseTheRequestedSelection) {
  std::string output;
  build_clipboard_query(ClipboardSelection::CLIPBOARD, output);
  EXPECT_EQ(output, "\x1b]52;c;?\x1b\\");

  build_clipboard_query(ClipboardSelection::PRIMARY, output);
  EXPECT_EQ(output, "\x1b]52;p;?\x1b\\");
}

TEST(TerminalClipboardTest, WriteLimitLeavesExistingOutputUnchanged) {
  std::string output = "existing";
  EXPECT_EQ(build_clipboard_write(ClipboardSelection::CLIPBOARD, "too large",
                                  output, 3),
            Status::OUTPUT_LIMIT_EXCEEDED);
  EXPECT_EQ(output, "existing");
}

TEST(TerminalClipboardTest, DecodesPaddedAndUnpaddedPayloads) {
  constexpr std::array encodings{
      ClipboardEncoding{"", ""},      ClipboardEncoding{"f", "Zg=="},
      ClipboardEncoding{"f", "Zg"},   ClipboardEncoding{"fo", "Zm8="},
      ClipboardEncoding{"fo", "Zm8"}, ClipboardEncoding{"foo", "Zm9v"},
  };

  for (const ClipboardEncoding& encoding : encodings) {
    ClipboardEvent event;
    ASSERT_EQ(decode_clipboard_payload(ClipboardSelection::CLIPBOARD,
                                       encoding.encoded, event),
              Status::OK);
    EXPECT_EQ(event.data, encoding.plain);
  }
}

TEST(TerminalClipboardTest, RejectsMalformedBase64WithoutChangingEvent) {
  constexpr std::array<std::string_view, 9> malformed{
      "A", "A===", "=AAA", "AA=A", "AA=", "AAA==", "####", "TR==", "TWF=",
  };
  for (const std::string_view input : malformed) {
    ClipboardEvent event{
        .selection = ClipboardSelection::PRIMARY,
        .data      = "existing",
    };
    EXPECT_EQ(
        decode_clipboard_payload(ClipboardSelection::CLIPBOARD, input, event),
        Status::INVALID_ARGUMENT)
        << input;
    EXPECT_EQ(event.selection, ClipboardSelection::PRIMARY);
    EXPECT_EQ(event.data, "existing");
  }
}

TEST(TerminalClipboardTest, DecodeLimitIsAppliedBeforeProducingData) {
  ClipboardEvent event{.data = "existing"};
  EXPECT_EQ(
      decode_clipboard_payload(ClipboardSelection::CLIPBOARD, "Zm9v", event, 2),
      Status::INPUT_LIMIT_EXCEEDED);
  EXPECT_EQ(event.data, "existing");
}

}  // namespace
}  // namespace puc::terminal
