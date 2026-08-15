/**
 * @file decoder_test.cpp
 * @brief Table-driven and chunk-boundary tests for terminal input decoding.
 */

#include "puc-cli/tui/terminal/decoder.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "gtest/gtest.h"
#include "puc-cli/tui/terminal/clipboard.hpp"

namespace puc::terminal {
namespace {

/** Build the runtime-configured decoder used by protocol tests. */
Decoder configured_decoder(DecoderLimits limits = {}) {
  Decoder decoder(limits);
  std::error_code error;
  const std::filesystem::path root = std::filesystem::current_path(error);
  if (error) {
    ADD_FAILURE() << "Could not resolve the test runfiles directory";
    return decoder;
  }
  properties::Properties configurations{root, root / "missing_user_overrides"};
  EXPECT_EQ(decoder.setup(configurations, "xterm-256color"), Status::OK);
  return decoder;
}

/** Build a decoder with one test-local user mapping overlay. */
Decoder configured_decoder_with_override(std::string_view test_name,
                                         std::string_view override_toml,
                                         DecoderLimits limits = {}) {
  Decoder decoder(limits);
  std::error_code error;
  const std::filesystem::path primary_root =
      std::filesystem::current_path(error);
  if (error) {
    ADD_FAILURE() << "Could not resolve the test runfiles directory";
    return decoder;
  }
  const char* temporary_directory = std::getenv("TEST_TMPDIR");
  if (temporary_directory == nullptr) {
    ADD_FAILURE() << "TEST_TMPDIR is unavailable";
    return decoder;
  }

  const std::filesystem::path override_root =
      std::filesystem::path{temporary_directory} / "decoder_overrides" /
      test_name;
  static_cast<void>(std::filesystem::remove_all(override_root, error));
  error.clear();
  if (!std::filesystem::create_directories(override_root, error) || error) {
    ADD_FAILURE() << "Could not create the user-override directory";
    return decoder;
  }

  const std::filesystem::path override_path = override_root / "input_keys.toml";
  FILE* file = std::fopen(override_path.string().c_str(), "wb");
  if (file == nullptr) {
    ADD_FAILURE() << "Could not create the user-override file";
    return decoder;
  }
  const bool written =
      std::fwrite(override_toml.data(), 1U, override_toml.size(), file) ==
      override_toml.size();
  const bool closed = std::fclose(file) == 0;
  if (!written || !closed) {
    ADD_FAILURE() << "Could not write the user-override file";
    return decoder;
  }

  properties::Properties configurations{primary_root, override_root};
  EXPECT_EQ(decoder.setup(configurations, "xterm-256color"), Status::OK);
  return decoder;
}

/** Feed one string and assert the decoder accepted it. */
std::vector<Event> decode(Decoder& decoder, std::string_view bytes) {
  std::vector<Event> events;
  EXPECT_EQ(decoder.feed(bytes, events), Status::OK);
  return events;
}

/** Deliver the decoder's currently requested timeout generation. */
void expire_pending_input(Decoder& decoder, std::vector<Event>& events) {
  const std::optional<TimeoutInput> timeout = decoder.pending_timeout();
  ASSERT_TRUE(timeout.has_value());
  ASSERT_EQ(decoder.handle_timeout(*timeout, events), Status::OK);
}

/** Extract a KeyEvent while producing a useful assertion on type mismatch. */
const KeyEvent& key_event(const Event& event) {
  EXPECT_TRUE(std::holds_alternative<KeyEvent>(event));
  return std::get<KeyEvent>(event);
}

/** Extract and compare one named key event. */
void expect_named_key(const Event& event, NamedKey key,
                      Modifiers modifiers = {},
                      KeyAction action    = KeyAction::PRESS) {
  const KeyEvent& decoded = key_event(event);
  ASSERT_TRUE(std::holds_alternative<NamedKey>(decoded.key.value));
  EXPECT_EQ(std::get<NamedKey>(decoded.key.value), key);
  EXPECT_EQ(decoded.modifiers, modifiers);
  EXPECT_EQ(decoded.action, action);
}

/** Extract and compare one Unicode logical key event. */
void expect_unicode_key(const Event& event, char32_t key,
                        Modifiers modifiers = {},
                        KeyAction action    = KeyAction::PRESS) {
  const KeyEvent& decoded = key_event(event);
  ASSERT_TRUE(std::holds_alternative<char32_t>(decoded.key.value));
  EXPECT_EQ(std::get<char32_t>(decoded.key.value), key);
  EXPECT_EQ(decoded.modifiers, modifiers);
  EXPECT_EQ(decoded.action, action);
}

TEST(TerminalDecoderTest, GroupsContiguousAsciiAndUtf8Text) {
  Decoder decoder = configured_decoder();
  const std::vector<Event> events =
      decode(decoder, "hello \xce\xbb \xf0\x9f\x98\x80");

  ASSERT_EQ(events.size(), 1U);
  ASSERT_TRUE(std::holds_alternative<TextEvent>(events[0]));
  EXPECT_EQ(std::get<TextEvent>(events[0]).utf8,
            "hello \xce\xbb \xf0\x9f\x98\x80");
  EXPECT_EQ(decoder.pending_bytes(), 0U);
}

TEST(TerminalDecoderTest, RetainsUtf8AcrossEveryByteBoundary) {
  constexpr std::string_view text = "\xf0\x9f\x98\x80";
  for (std::size_t split = 0; split <= text.size(); ++split) {
    Decoder decoder = configured_decoder();
    std::vector<Event> events;
    ASSERT_EQ(decoder.feed(text.substr(0, split), events), Status::OK);
    ASSERT_EQ(decoder.feed(text.substr(split), events), Status::OK);
    ASSERT_EQ(events.size(), 1U) << split;
    ASSERT_TRUE(std::holds_alternative<TextEvent>(events[0]));
    EXPECT_EQ(std::get<TextEvent>(events[0]).utf8, text);
  }
}

TEST(TerminalDecoderTest, ReportsInvalidUtf8AndRecoversFollowingText) {
  Decoder decoder                 = configured_decoder();
  const std::vector<Event> events = decode(decoder, "\xc0\xafX");

  ASSERT_EQ(events.size(), 3U);
  ASSERT_TRUE(std::holds_alternative<UnknownEvent>(events[0]));
  EXPECT_EQ(std::get<UnknownEvent>(events[0]).reason,
            UnknownInputReason::INVALID_UTF8);
  ASSERT_TRUE(std::holds_alternative<UnknownEvent>(events[1]));
  EXPECT_EQ(std::get<UnknownEvent>(events[1]).reason,
            UnknownInputReason::INVALID_UTF8);
  ASSERT_TRUE(std::holds_alternative<TextEvent>(events[2]));
  EXPECT_EQ(std::get<TextEvent>(events[2]).utf8, "X");
}

TEST(TerminalDecoderTest, RejectsEveryStructurallyInvalidUtf8Class) {
  const std::array<std::string_view, 6> invalid{
      "\x80",              // Stray continuation byte.
      "\xe2\x28\xa1",      // Invalid continuation byte.
      "\xe0\x80\x80",      // Overlong three-byte encoding.
      "\xed\xa0\x80",      // UTF-16 surrogate.
      "\xf0\x80\x80\x80",  // Overlong four-byte encoding.
      "\xf4\x90\x80\x80",  // Above Unicode's maximum scalar.
  };

  for (const std::string_view malformed : invalid) {
    Decoder decoder = configured_decoder();
    const std::vector<Event> events =
        decode(decoder, std::string{malformed} + "x");
    ASSERT_FALSE(events.empty()) << malformed;
    EXPECT_EQ(std::get<UnknownEvent>(events.front()).reason,
              UnknownInputReason::INVALID_UTF8)
        << malformed;
    EXPECT_EQ(decoder.pending_bytes(), 0U);
  }
}

TEST(TerminalDecoderTest, NormalizesLegacyControlBytes) {
  std::string bytes;
  bytes.push_back('\0');
  bytes.push_back('\x01');
  bytes.push_back('\x03');
  bytes.push_back('\t');
  bytes.push_back('\n');
  bytes.push_back('\r');
  bytes.push_back('\x7f');

  Decoder decoder                 = configured_decoder();
  const std::vector<Event> events = decode(decoder, bytes);
  ASSERT_EQ(events.size(), 7U);
  expect_unicode_key(events[0], U' ', Modifier::CONTROL);
  expect_unicode_key(events[1], U'a', Modifier::CONTROL);
  expect_unicode_key(events[2], U'c', Modifier::CONTROL);
  expect_named_key(events[3], NamedKey::TAB);
  expect_named_key(events[4], NamedKey::ENTER);
  expect_named_key(events[5], NamedKey::ENTER);
  expect_named_key(events[6], NamedKey::BACKSPACE);
}

TEST(TerminalDecoderTest, NormalizesRemainingRepresentableControlBytes) {
  std::string bytes;
  bytes.push_back('\b');
  bytes.push_back('\x1c');
  bytes.push_back('\x1d');
  bytes.push_back('\x1e');
  bytes.push_back('\x1f');
  bytes.push_back('\x0b');

  Decoder decoder                 = configured_decoder();
  const std::vector<Event> events = decode(decoder, bytes);
  ASSERT_EQ(events.size(), 6U);
  expect_named_key(events[0], NamedKey::BACKSPACE);
  expect_unicode_key(events[1], U'\\', Modifier::CONTROL);
  expect_unicode_key(events[2], U']', Modifier::CONTROL);
  expect_unicode_key(events[3], U'^', Modifier::CONTROL);
  expect_unicode_key(events[4], U'_', Modifier::CONTROL);
  expect_unicode_key(events[5], U'k', Modifier::CONTROL);
}

TEST(TerminalDecoderTest, StandaloneEscapeWaitsForCallerTimeout) {
  Decoder decoder = configured_decoder();
  std::vector<Event> events;
  ASSERT_EQ(decoder.feed("\x1b", events), Status::OK);
  EXPECT_TRUE(events.empty());
  EXPECT_EQ(decoder.pending_bytes(), 1U);

  expire_pending_input(decoder, events);
  ASSERT_EQ(events.size(), 1U);
  expect_named_key(events[0], NamedKey::ESCAPE);
  EXPECT_EQ(decoder.pending_bytes(), 0U);
}

TEST(TerminalDecoderTest, EscapePrefixNormalizesAltModifiedKeys) {
  Decoder decoder           = configured_decoder();
  std::vector<Event> events = decode(decoder,
                                     "\x1b"
                                     "a\x1b\x03\x1b\x1b");

  ASSERT_EQ(events.size(), 3U);
  expect_unicode_key(events[0], U'a', Modifier::ALT);
  expect_unicode_key(events[1], U'c', Modifier::CONTROL | Modifier::ALT);
  expect_named_key(events[2], NamedKey::ESCAPE, Modifier::ALT);
}

TEST(TerminalDecoderTest, EscapePrefixSupportsMultibyteUnicodeKeys) {
  Decoder decoder = configured_decoder();
  const std::vector<Event> events =
      decode(decoder, "\x1b\xc3\xa5\x1b\xf0\x9f\x98\x80");

  ASSERT_EQ(events.size(), 2U);
  expect_unicode_key(events[0], U'\u00e5', Modifier::ALT);
  expect_unicode_key(events[1], U'\U0001f600', Modifier::ALT);
}

TEST(TerminalDecoderTest, EscapeTimeoutPreservesIncompleteUtf8ForLaterInput) {
  Decoder decoder = configured_decoder();
  std::vector<Event> events;
  ASSERT_EQ(decoder.feed("\x1b\xc3", events), Status::OK);
  EXPECT_TRUE(events.empty());

  expire_pending_input(decoder, events);
  ASSERT_EQ(events.size(), 1U);
  expect_named_key(events[0], NamedKey::ESCAPE);
  EXPECT_EQ(decoder.pending_bytes(), 1U);

  ASSERT_EQ(decoder.feed("\xa5", events), Status::OK);
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(std::get<TextEvent>(events[1]).utf8, "\xc3\xa5");
}

TEST(TerminalDecoderTest, InvalidUtf8AfterEscapeIsReportedAndRecovers) {
  Decoder decoder                 = configured_decoder();
  const std::vector<Event> events = decode(decoder, "\x1b\xffx");
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(std::get<UnknownEvent>(events[0]).reason,
            UnknownInputReason::INVALID_UTF8);
  EXPECT_EQ(std::get<TextEvent>(events[1]).utf8, "x");
}

TEST(TerminalDecoderTest, TimeoutResolvesAnIncompleteCsiAsAltBracket) {
  Decoder decoder = configured_decoder();
  std::vector<Event> events;
  ASSERT_EQ(decoder.feed("\x1b[1", events), Status::OK);
  EXPECT_TRUE(events.empty());

  expire_pending_input(decoder, events);
  ASSERT_EQ(events.size(), 2U);
  expect_unicode_key(events[0], U'[', Modifier::ALT);
  ASSERT_TRUE(std::holds_alternative<TextEvent>(events[1]));
  EXPECT_EQ(std::get<TextEvent>(events[1]).utf8, "1");
}

TEST(TerminalDecoderTest, NewInputGenerationMakesAnOlderTimeoutHarmless) {
  Decoder decoder = configured_decoder();
  std::vector<Event> events;
  ASSERT_EQ(decoder.feed("\x1b", events), Status::OK);
  const std::optional<TimeoutInput> first = decoder.pending_timeout();
  ASSERT_TRUE(first.has_value());

  ASSERT_EQ(decoder.feed("[", events), Status::OK);
  const std::optional<TimeoutInput> second = decoder.pending_timeout();
  ASSERT_TRUE(second.has_value());
  EXPECT_NE(first, second);

  EXPECT_EQ(decoder.handle_timeout(*first, events), Status::OK);
  EXPECT_TRUE(events.empty());
  EXPECT_EQ(decoder.pending_timeout(), second);

  EXPECT_EQ(decoder.handle_timeout(*second, events), Status::OK);
  ASSERT_EQ(events.size(), 1U);
  expect_unicode_key(events[0], U'[', Modifier::ALT);
  EXPECT_EQ(decoder.pending_bytes(), 0U);
  EXPECT_EQ(decoder.pending_timeout(), std::nullopt);
}

TEST(TerminalDecoderTest, TimeoutReportsAnIncompleteUtf8PrefixAndResets) {
  Decoder decoder = configured_decoder();
  std::vector<Event> events;
  ASSERT_EQ(decoder.feed("\xf0\x9f", events), Status::OK);
  ASSERT_TRUE(decoder.pending_timeout().has_value());

  expire_pending_input(decoder, events);
  ASSERT_EQ(events.size(), 1U);
  const auto* unknown = std::get_if<UnknownEvent>(&events.front());
  ASSERT_NE(unknown, nullptr);
  EXPECT_EQ(unknown->reason, UnknownInputReason::INCOMPLETE_SEQUENCE);
  EXPECT_EQ(unknown->bytes, "\xf0\x9f");
  EXPECT_EQ(decoder.pending_bytes(), 0U);
  EXPECT_EQ(decoder.pending_timeout(), std::nullopt);

  ASSERT_EQ(decoder.feed("x", events), Status::OK);
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(std::get<TextEvent>(events.back()).utf8, "x");
}

TEST(TerminalDecoderTest, BracketedPasteDoesNotRequestSequenceTimeouts) {
  Decoder decoder = configured_decoder();
  std::vector<Event> events;
  ASSERT_EQ(decoder.feed("\x1b[200~partial", events), Status::OK);
  EXPECT_TRUE(decoder.paste_in_progress());
  EXPECT_EQ(decoder.pending_timeout(), std::nullopt);
}

struct FixedKeyEncoding {
  std::string_view sequence;
  NamedKey key;
};

TEST(TerminalDecoderTest, DecodesLegacyCsiAndSs3KeyTables) {
  constexpr std::array encodings{
      FixedKeyEncoding{"\x1b[A", NamedKey::UP},
      FixedKeyEncoding{"\x1b[B", NamedKey::DOWN},
      FixedKeyEncoding{"\x1b[C", NamedKey::RIGHT},
      FixedKeyEncoding{"\x1b[D", NamedKey::LEFT},
      FixedKeyEncoding{"\x1b[H", NamedKey::HOME},
      FixedKeyEncoding{"\x1b[F", NamedKey::END},
      FixedKeyEncoding{"\x1b[1~", NamedKey::HOME},
      FixedKeyEncoding{"\x1b[2~", NamedKey::INSERT},
      FixedKeyEncoding{"\x1b[3~", NamedKey::DELETE_KEY},
      FixedKeyEncoding{"\x1b[4~", NamedKey::END},
      FixedKeyEncoding{"\x1b[5~", NamedKey::PAGE_UP},
      FixedKeyEncoding{"\x1b[6~", NamedKey::PAGE_DOWN},
      FixedKeyEncoding{"\x1b[7~", NamedKey::HOME},
      FixedKeyEncoding{"\x1b[8~", NamedKey::END},
      FixedKeyEncoding{"\x1b[11~", NamedKey::F1},
      FixedKeyEncoding{"\x1b[12~", NamedKey::F2},
      FixedKeyEncoding{"\x1b[13~", NamedKey::F3},
      FixedKeyEncoding{"\x1b[14~", NamedKey::F4},
      FixedKeyEncoding{"\x1b[15~", NamedKey::F5},
      FixedKeyEncoding{"\x1b[17~", NamedKey::F6},
      FixedKeyEncoding{"\x1b[18~", NamedKey::F7},
      FixedKeyEncoding{"\x1b[19~", NamedKey::F8},
      FixedKeyEncoding{"\x1b[20~", NamedKey::F9},
      FixedKeyEncoding{"\x1b[21~", NamedKey::F10},
      FixedKeyEncoding{"\x1b[23~", NamedKey::F11},
      FixedKeyEncoding{"\x1b[24~", NamedKey::F12},
      FixedKeyEncoding{"\x1bOA", NamedKey::UP},
      FixedKeyEncoding{"\x1bOB", NamedKey::DOWN},
      FixedKeyEncoding{"\x1bOC", NamedKey::RIGHT},
      FixedKeyEncoding{"\x1bOD", NamedKey::LEFT},
      FixedKeyEncoding{"\x1bOH", NamedKey::HOME},
      FixedKeyEncoding{"\x1bOF", NamedKey::END},
      FixedKeyEncoding{"\x1bOP", NamedKey::F1},
      FixedKeyEncoding{"\x1bOQ", NamedKey::F2},
      FixedKeyEncoding{"\x1bOR", NamedKey::F3},
      FixedKeyEncoding{"\x1bOS", NamedKey::F4},
      FixedKeyEncoding{"\x1bOM", NamedKey::KEYPAD_ENTER},
      FixedKeyEncoding{"\x1bOo", NamedKey::KEYPAD_DIVIDE},
      FixedKeyEncoding{"\x1bOj", NamedKey::KEYPAD_MULTIPLY},
      FixedKeyEncoding{"\x1bOm", NamedKey::KEYPAD_SUBTRACT},
      FixedKeyEncoding{"\x1bOk", NamedKey::KEYPAD_ADD},
      FixedKeyEncoding{"\x1bOn", NamedKey::KEYPAD_DECIMAL},
      FixedKeyEncoding{"\x1bOX", NamedKey::KEYPAD_EQUAL},
      FixedKeyEncoding{"\x1bOp", NamedKey::KEYPAD_0},
      FixedKeyEncoding{"\x1bOq", NamedKey::KEYPAD_1},
      FixedKeyEncoding{"\x1bOr", NamedKey::KEYPAD_2},
      FixedKeyEncoding{"\x1bOs", NamedKey::KEYPAD_3},
      FixedKeyEncoding{"\x1bOt", NamedKey::KEYPAD_4},
      FixedKeyEncoding{"\x1bOu", NamedKey::KEYPAD_5},
      FixedKeyEncoding{"\x1bOv", NamedKey::KEYPAD_6},
      FixedKeyEncoding{"\x1bOw", NamedKey::KEYPAD_7},
      FixedKeyEncoding{"\x1bOx", NamedKey::KEYPAD_8},
      FixedKeyEncoding{"\x1bOy", NamedKey::KEYPAD_9},
  };

  for (const FixedKeyEncoding& encoding : encodings) {
    Decoder decoder                 = configured_decoder();
    const std::vector<Event> events = decode(decoder, encoding.sequence);
    ASSERT_EQ(events.size(), 1U) << encoding.sequence;
    expect_named_key(events[0], encoding.key);
  }
}

TEST(TerminalDecoderTest, ShiftTabIsANormalizedKey) {
  Decoder decoder                 = configured_decoder();
  const std::vector<Event> events = decode(decoder, "\x1b[Z");
  ASSERT_EQ(events.size(), 1U);
  expect_named_key(events[0], NamedKey::TAB, Modifier::SHIFT);
}

TEST(TerminalDecoderTest, FixedKeysDecodeAcrossEveryChunkBoundary) {
  constexpr std::string_view sequence = "\x1b[24~";
  for (std::size_t split = 0; split <= sequence.size(); ++split) {
    Decoder decoder = configured_decoder();
    std::vector<Event> events;
    ASSERT_EQ(decoder.feed(sequence.substr(0, split), events), Status::OK);
    ASSERT_EQ(decoder.feed(sequence.substr(split), events), Status::OK);
    ASSERT_EQ(events.size(), 1U) << split;
    expect_named_key(events[0], NamedKey::F12);
  }
}

TEST(TerminalDecoderTest, ConfiguredPrefixAndLongerExactKeyRemainDistinct) {
  Decoder decoder =
      configured_decoder_with_override("prefix_and_longer_exact", R"toml(
[[mapping]]
sequence = "<CSI>x"
kind = "key"
key = "F30"

[[mapping]]
sequence = "<CSI>xy"
kind = "key"
key = "F31"
)toml");

  std::vector<Event> events;
  ASSERT_EQ(decoder.feed("\x1b[x", events), Status::OK);
  EXPECT_TRUE(events.empty());
  expire_pending_input(decoder, events);
  ASSERT_EQ(events.size(), 1U);
  expect_named_key(events[0], NamedKey::F30);

  events.clear();
  ASSERT_EQ(decoder.feed("\x1b[xy", events), Status::OK);
  ASSERT_EQ(events.size(), 1U);
  expect_named_key(events[0], NamedKey::F31);
}

TEST(TerminalDecoderTest, ConfiguredPrefixWinsWhenFollowingByteDiverges) {
  Decoder decoder =
      configured_decoder_with_override("prefix_divergence", R"toml(
[[mapping]]
sequence = "<CSI>x"
kind = "key"
key = "F30"

[[mapping]]
sequence = "<CSI>xy"
kind = "key"
key = "F31"
)toml");

  const std::vector<Event> events = decode(decoder, "\x1b[xq");
  ASSERT_EQ(events.size(), 2U);
  expect_named_key(events[0], NamedKey::F30);
  EXPECT_EQ(std::get<TextEvent>(events[1]).utf8, "q");
}

TEST(TerminalDecoderTest, DecodesParameterizedLegacyKeys) {
  Decoder decoder = configured_decoder();
  const std::vector<Event> events =
      decode(decoder, "\x1b[1;5A\x1b[1;4D\x1b[15;3~\x1b[27;6;97~");

  ASSERT_EQ(events.size(), 4U);
  expect_named_key(events[0], NamedKey::UP, Modifier::CONTROL);
  expect_named_key(events[1], NamedKey::LEFT, Modifier::SHIFT | Modifier::ALT);
  expect_named_key(events[2], NamedKey::F5, Modifier::ALT);
  expect_unicode_key(events[3], U'a', Modifier::SHIFT | Modifier::CONTROL);
}

TEST(TerminalDecoderTest, DecodesEveryParameterizedTildeKey) {
  struct TildeKey {
    std::uint32_t number;
    NamedKey key;
  };
  constexpr std::array keys{
      TildeKey{1, NamedKey::HOME},
      TildeKey{2, NamedKey::INSERT},
      TildeKey{3, NamedKey::DELETE_KEY},
      TildeKey{4, NamedKey::END},
      TildeKey{5, NamedKey::PAGE_UP},
      TildeKey{6, NamedKey::PAGE_DOWN},
      TildeKey{7, NamedKey::HOME},
      TildeKey{8, NamedKey::END},
      TildeKey{11, NamedKey::F1},
      TildeKey{12, NamedKey::F2},
      TildeKey{13, NamedKey::F3},
      TildeKey{14, NamedKey::F4},
      TildeKey{15, NamedKey::F5},
      TildeKey{17, NamedKey::F6},
      TildeKey{18, NamedKey::F7},
      TildeKey{19, NamedKey::F8},
      TildeKey{20, NamedKey::F9},
      TildeKey{21, NamedKey::F10},
      TildeKey{23, NamedKey::F11},
      TildeKey{24, NamedKey::F12},
      TildeKey{57427, NamedKey::KEYPAD_BEGIN},
  };

  for (const TildeKey& key : keys) {
    Decoder decoder = configured_decoder();
    const std::vector<Event> events =
        decode(decoder, "\x1b[" + std::to_string(key.number) + ";3~");
    ASSERT_EQ(events.size(), 1U) << key.number;
    expect_named_key(events[0], key.key, Modifier::ALT);
  }
}

TEST(TerminalDecoderTest, DecodesEveryParameterizedCsiLetterKey) {
  constexpr std::array keys{
      FixedKeyEncoding{"A", NamedKey::UP},
      FixedKeyEncoding{"B", NamedKey::DOWN},
      FixedKeyEncoding{"C", NamedKey::RIGHT},
      FixedKeyEncoding{"D", NamedKey::LEFT},
      FixedKeyEncoding{"H", NamedKey::HOME},
      FixedKeyEncoding{"F", NamedKey::END},
      FixedKeyEncoding{"P", NamedKey::F1},
      FixedKeyEncoding{"Q", NamedKey::F2},
      FixedKeyEncoding{"S", NamedKey::F4},
      FixedKeyEncoding{"E", NamedKey::KEYPAD_BEGIN},
  };

  for (const FixedKeyEncoding& key : keys) {
    Decoder decoder = configured_decoder();
    const std::vector<Event> events =
        decode(decoder, "\x1b[1;3" + std::string{key.sequence});
    ASSERT_EQ(events.size(), 1U) << key.sequence;
    if (current_operating_system() == OperatingSystem::DARWIN) {
      std::optional<Command> mac_command;
      if (key.key == NamedKey::UP) {
        mac_command = Command::MOVE_PAGE_UP;
      } else if (key.key == NamedKey::DOWN) {
        mac_command = Command::MOVE_PAGE_DOWN;
      } else if (key.key == NamedKey::LEFT) {
        mac_command = Command::MOVE_WORD_LEFT;
      } else if (key.key == NamedKey::RIGHT) {
        mac_command = Command::MOVE_WORD_RIGHT;
      }
      if (mac_command.has_value()) {
        EXPECT_EQ(events[0], Event{CommandEvent{.command = *mac_command}});
        continue;
      }
    }
    expect_named_key(events[0], key.key, Modifier::ALT);
  }
}

TEST(TerminalDecoderTest, ExtendedCsiLetterKeysCarryEventActions) {
  Decoder decoder                 = configured_decoder();
  const std::vector<Event> events = decode(decoder, "\x1b[1;5:2A\x1b[1;1:3C");

  ASSERT_EQ(events.size(), 2U);
  expect_named_key(events[0], NamedKey::UP, Modifier::CONTROL,
                   KeyAction::REPEAT);
  expect_named_key(events[1], NamedKey::RIGHT, {}, KeyAction::RELEASE);
}

TEST(TerminalDecoderTest, RejectsMalformedParameterizedLegacyKeys) {
  struct MalformedKey {
    std::string_view sequence;
    UnknownInputReason reason;
  };
  constexpr std::array malformed{
      MalformedKey{"\x1b[;2~", UnknownInputReason::MALFORMED_SEQUENCE},
      MalformedKey{"\x1b[99~", UnknownInputReason::UNSUPPORTED_SEQUENCE},
      MalformedKey{"\x1b[1;2;3~", UnknownInputReason::UNSUPPORTED_SEQUENCE},
      MalformedKey{"\x1b[1;0~", UnknownInputReason::MALFORMED_SEQUENCE},
      MalformedKey{"\x1b[27;2;57344~", UnknownInputReason::MALFORMED_SEQUENCE},
      MalformedKey{"\x1b[2A", UnknownInputReason::MALFORMED_SEQUENCE},
      MalformedKey{"\x1b[1;2;3A", UnknownInputReason::MALFORMED_SEQUENCE},
      MalformedKey{"\x1b[1;0A", UnknownInputReason::MALFORMED_SEQUENCE},
      MalformedKey{"\x1b[2E", UnknownInputReason::MALFORMED_SEQUENCE},
  };

  for (const MalformedKey& key : malformed) {
    Decoder decoder                 = configured_decoder();
    const std::vector<Event> events = decode(decoder, key.sequence);
    ASSERT_EQ(events.size(), 1U) << key.sequence;
    EXPECT_EQ(std::get<UnknownEvent>(events[0]).reason, key.reason)
        << key.sequence;
  }
}

TEST(TerminalDecoderTest, DecodesKittyUnicodeModifiersAndActions) {
  Decoder decoder                 = configured_decoder();
  const std::vector<Event> events = decode(decoder, "\x1b[97;6:2u\x1b[98;3:3u");

  ASSERT_EQ(events.size(), 2U);
  expect_unicode_key(events[0], U'a', Modifier::SHIFT | Modifier::CONTROL,
                     KeyAction::REPEAT);
  expect_unicode_key(events[1], U'b', Modifier::ALT, KeyAction::RELEASE);
}

TEST(TerminalDecoderTest, DecodesKittyAlternateKeysAndAssociatedText) {
  Decoder decoder                 = configured_decoder();
  const std::vector<Event> events = decode(decoder, "\x1b[97:65:99;2;65u");

  ASSERT_EQ(events.size(), 1U);
  const KeyEvent& event = key_event(events[0]);
  EXPECT_EQ(std::get<char32_t>(event.key.value), U'a');
  EXPECT_EQ(event.shifted_key, U'A');
  EXPECT_EQ(event.base_layout_key, U'c');
  EXPECT_EQ(event.modifiers, Modifiers{Modifier::SHIFT});
  EXPECT_EQ(event.text, "A");
}

TEST(TerminalDecoderTest, DecodesEnhancedColonWithAssociatedText) {
  Decoder decoder                 = configured_decoder();
  const std::vector<Event> events = decode(decoder, "\x1b[59:58;2;58u");

  ASSERT_EQ(events.size(), 1U);
  const KeyEvent& event = key_event(events[0]);
  EXPECT_EQ(std::get<char32_t>(event.key.value), U';');
  EXPECT_EQ(event.shifted_key, U':');
  EXPECT_EQ(event.modifiers, Modifiers{Modifier::SHIFT});
  EXPECT_EQ(event.text, ":");
}

TEST(TerminalDecoderTest, DecodesPureKittyTextEvents) {
  Decoder decoder = configured_decoder();
  const std::vector<Event> events =
      decode(decoder, "\x1b[0;;229u\x1b[0;;97:769u");

  ASSERT_EQ(events.size(), 2U);
  ASSERT_TRUE(std::holds_alternative<TextEvent>(events[0]));
  EXPECT_EQ(std::get<TextEvent>(events[0]).utf8, "\xc3\xa5");
  ASSERT_TRUE(std::holds_alternative<TextEvent>(events[1]));
  EXPECT_EQ(std::get<TextEvent>(events[1]).utf8, "a\xcc\x81");
}

TEST(TerminalDecoderTest, KittyAssociatedTextEncodesAllUtf8Widths) {
  Decoder decoder = configured_decoder();
  const std::vector<Event> events =
      decode(decoder, "\x1b[0;;65:229:8364:128512u");

  ASSERT_EQ(events.size(), 1U);
  ASSERT_TRUE(std::holds_alternative<TextEvent>(events[0]));
  EXPECT_EQ(std::get<TextEvent>(events[0]).utf8,
            "A\xc3\xa5\xe2\x82\xac\xf0\x9f\x98\x80");
}

TEST(TerminalDecoderTest, DecodesAllContiguousKittyF13ThroughF35Codes) {
  for (std::uint32_t code = 57376; code <= 57398; ++code) {
    Decoder decoder                 = configured_decoder();
    const std::string sequence      = "\x1b[" + std::to_string(code) + "u";
    const std::vector<Event> events = decode(decoder, sequence);
    ASSERT_EQ(events.size(), 1U) << code;
    expect_named_key(events[0], static_cast<NamedKey>(
                                    static_cast<unsigned int>(NamedKey::F13) +
                                    code - 57376U));
  }
}

TEST(TerminalDecoderTest, DecodesKittyNavigationAndF1ThroughF12Codes) {
  struct FunctionalKey {
    std::uint32_t code;
    NamedKey key;
  };
  constexpr std::array navigation{
      FunctionalKey{57348, NamedKey::INSERT},
      FunctionalKey{57349, NamedKey::DELETE_KEY},
      FunctionalKey{57350, NamedKey::LEFT},
      FunctionalKey{57351, NamedKey::RIGHT},
      FunctionalKey{57352, NamedKey::UP},
      FunctionalKey{57353, NamedKey::DOWN},
      FunctionalKey{57354, NamedKey::PAGE_UP},
      FunctionalKey{57355, NamedKey::PAGE_DOWN},
      FunctionalKey{57356, NamedKey::HOME},
      FunctionalKey{57357, NamedKey::END},
  };

  for (const FunctionalKey& key : navigation) {
    Decoder decoder = configured_decoder();
    const std::vector<Event> events =
        decode(decoder, "\x1b[" + std::to_string(key.code) + "u");
    ASSERT_EQ(events.size(), 1U) << key.code;
    expect_named_key(events[0], key.key);
  }
  for (std::uint32_t code = 57364U; code <= 57375U; ++code) {
    Decoder decoder = configured_decoder();
    const std::vector<Event> events =
        decode(decoder, "\x1b[" + std::to_string(code) + "u");
    ASSERT_EQ(events.size(), 1U) << code;
    expect_named_key(events[0], static_cast<NamedKey>(
                                    static_cast<unsigned int>(NamedKey::F1) +
                                    code - 57364U));
  }
}

TEST(TerminalDecoderTest, DecodesEveryKittyKeypadAndMediaCode) {
  constexpr std::array expected{
      NamedKey::KEYPAD_0,           NamedKey::KEYPAD_1,
      NamedKey::KEYPAD_2,           NamedKey::KEYPAD_3,
      NamedKey::KEYPAD_4,           NamedKey::KEYPAD_5,
      NamedKey::KEYPAD_6,           NamedKey::KEYPAD_7,
      NamedKey::KEYPAD_8,           NamedKey::KEYPAD_9,
      NamedKey::KEYPAD_DECIMAL,     NamedKey::KEYPAD_DIVIDE,
      NamedKey::KEYPAD_MULTIPLY,    NamedKey::KEYPAD_SUBTRACT,
      NamedKey::KEYPAD_ADD,         NamedKey::KEYPAD_ENTER,
      NamedKey::KEYPAD_EQUAL,       NamedKey::KEYPAD_SEPARATOR,
      NamedKey::KEYPAD_LEFT,        NamedKey::KEYPAD_RIGHT,
      NamedKey::KEYPAD_UP,          NamedKey::KEYPAD_DOWN,
      NamedKey::KEYPAD_PAGE_UP,     NamedKey::KEYPAD_PAGE_DOWN,
      NamedKey::KEYPAD_HOME,        NamedKey::KEYPAD_END,
      NamedKey::KEYPAD_INSERT,      NamedKey::KEYPAD_DELETE,
      NamedKey::KEYPAD_BEGIN,       NamedKey::MEDIA_PLAY,
      NamedKey::MEDIA_PAUSE,        NamedKey::MEDIA_PLAY_PAUSE,
      NamedKey::MEDIA_REVERSE,      NamedKey::MEDIA_STOP,
      NamedKey::MEDIA_FAST_FORWARD, NamedKey::MEDIA_REWIND,
      NamedKey::MEDIA_TRACK_NEXT,   NamedKey::MEDIA_TRACK_PREVIOUS,
      NamedKey::MEDIA_RECORD,       NamedKey::VOLUME_DOWN,
      NamedKey::VOLUME_UP,          NamedKey::VOLUME_MUTE,
      NamedKey::LEFT_SHIFT,         NamedKey::LEFT_CONTROL,
      NamedKey::LEFT_ALT,           NamedKey::LEFT_SUPER,
      NamedKey::LEFT_HYPER,         NamedKey::LEFT_META,
      NamedKey::RIGHT_SHIFT,        NamedKey::RIGHT_CONTROL,
      NamedKey::RIGHT_ALT,          NamedKey::RIGHT_SUPER,
      NamedKey::RIGHT_HYPER,        NamedKey::RIGHT_META,
      NamedKey::ISO_LEVEL3_SHIFT,   NamedKey::ISO_LEVEL5_SHIFT,
  };

  for (std::size_t index = 0; index < expected.size(); ++index) {
    const std::uint32_t code = 57399U + static_cast<std::uint32_t>(index);
    Decoder decoder          = configured_decoder();
    const std::vector<Event> events =
        decode(decoder, "\x1b[" + std::to_string(code) + "u");
    ASSERT_EQ(events.size(), 1U) << code;
    expect_named_key(events[0], expected[index]);
  }
}

TEST(TerminalDecoderTest, DecodesRepresentativeKittyFunctionalKeys) {
  struct FunctionalKey {
    std::uint32_t code;
    NamedKey key;
  };
  constexpr std::array keys{
      FunctionalKey{27, NamedKey::ESCAPE},
      FunctionalKey{9, NamedKey::TAB},
      FunctionalKey{13, NamedKey::ENTER},
      FunctionalKey{127, NamedKey::BACKSPACE},
      FunctionalKey{57358, NamedKey::CAPS_LOCK},
      FunctionalKey{57359, NamedKey::SCROLL_LOCK},
      FunctionalKey{57360, NamedKey::NUM_LOCK},
      FunctionalKey{57361, NamedKey::PRINT_SCREEN},
      FunctionalKey{57362, NamedKey::PAUSE},
      FunctionalKey{57363, NamedKey::MENU},
      FunctionalKey{57399, NamedKey::KEYPAD_0},
      FunctionalKey{57414, NamedKey::KEYPAD_ENTER},
      FunctionalKey{57427, NamedKey::KEYPAD_BEGIN},
      FunctionalKey{57428, NamedKey::MEDIA_PLAY},
      FunctionalKey{57440, NamedKey::VOLUME_MUTE},
      FunctionalKey{57441, NamedKey::LEFT_SHIFT},
      FunctionalKey{57452, NamedKey::RIGHT_META},
      FunctionalKey{57454, NamedKey::ISO_LEVEL5_SHIFT},
  };

  for (const FunctionalKey& key : keys) {
    Decoder decoder = configured_decoder();
    const std::vector<Event> events =
        decode(decoder, "\x1b[" + std::to_string(key.code) + "u");
    ASSERT_EQ(events.size(), 1U) << key.code;
    expect_named_key(events[0], key.key);
  }
}

TEST(TerminalDecoderTest, RejectsMalformedKittyEventsAndRecovers) {
  constexpr std::array<std::string_view, 6> malformed{
      "\x1b[97;0u",    // Modifiers are encoded as one plus the bit field.
      "\x1b[97;1:4u",  // Event types stop at release (3).
      "\x1b[55296u",   // UTF-16 surrogate, not a Unicode scalar.
      "\x1b[0;;31u",   // Associated text cannot contain controls.
      "\x1b[0u",       // Pure text event without associated text.
      "\x1b[57344u",   // Unassigned Kitty private-use function code.
  };

  for (const std::string_view sequence : malformed) {
    Decoder decoder           = configured_decoder();
    std::vector<Event> events = decode(decoder, std::string{sequence} + "x");
    ASSERT_EQ(events.size(), 2U) << sequence;
    ASSERT_TRUE(std::holds_alternative<UnknownEvent>(events[0]));
    ASSERT_TRUE(std::holds_alternative<TextEvent>(events[1]));
    EXPECT_EQ(std::get<TextEvent>(events[1]).utf8, "x");
  }
}

TEST(TerminalDecoderTest, RejectsMalformedExtendedKeyFields) {
  std::string excessive_text = "\x1b[0;;1";
  for (std::size_t index = 0; index < 64U; ++index) {
    excessive_text.append(":1");
  }
  excessive_text.push_back('u');

  const std::array malformed{
      std::string{"\x1b[97;1;97;98u"},  // Too many top-level fields.
      std::string{"\x1b[97:65:97:1u"},  // Too many key fields.
      std::string{"\x1b[97:55296u"},    // Invalid shifted scalar.
      std::string{"\x1b[97::55296u"},   // Invalid base-layout scalar.
      std::string{"\x1b[97;257u"},      // Unknown modifier bits.
      std::string{"\x1b[97;1:u"},       // Missing event action.
      std::string{"\x1b[0;;127u"},      // Disallowed C1/control text.
      std::move(excessive_text),        // Associated-text field limit.
  };

  for (const std::string& sequence : malformed) {
    Decoder decoder                 = configured_decoder();
    const std::vector<Event> events = decode(decoder, sequence);
    ASSERT_EQ(events.size(), 1U) << sequence;
    EXPECT_EQ(std::get<UnknownEvent>(events[0]).reason,
              UnknownInputReason::MALFORMED_SEQUENCE)
        << sequence;
  }
}

TEST(TerminalDecoderTest, RoutesKittyQueryAndStackReplies) {
  Decoder decoder = configured_decoder();
  const std::vector<Event> events =
      decode(decoder, "\x1b[>1u\x1b[<u\x1b[?42949672960u");

  ASSERT_EQ(events.size(), 3U);
  EXPECT_EQ(std::get<TerminalResponseEvent>(events[0]).kind,
            TerminalResponseKind::KITTY_KEYBOARD_FLAGS);
  EXPECT_EQ(std::get<TerminalResponseEvent>(events[1]).kind,
            TerminalResponseKind::KITTY_KEYBOARD_FLAGS);
  EXPECT_EQ(std::get<UnknownEvent>(events[2]).reason,
            UnknownInputReason::MALFORMED_SEQUENCE);
}

TEST(TerminalDecoderTest, DecodesFocusTransitions) {
  Decoder decoder                 = configured_decoder();
  const std::vector<Event> events = decode(decoder, "\x1b[I\x1b[O");
  ASSERT_EQ(events.size(), 2U);
  ASSERT_TRUE(std::holds_alternative<FocusEvent>(events[0]));
  ASSERT_TRUE(std::get<FocusEvent>(events[0]).focused);
  ASSERT_TRUE(std::holds_alternative<FocusEvent>(events[1]));
  EXPECT_FALSE(std::get<FocusEvent>(events[1]).focused);
}

TEST(TerminalDecoderTest, DecodesSgrMousePressReleaseMoveAndDrag) {
  Decoder decoder                 = configured_decoder();
  const std::vector<Event> events = decode(decoder,
                                           "\x1b[<0;10;20M\x1b[<0;10;20m"
                                           "\x1b[<32;11;21M\x1b[<35;12;22M");
  ASSERT_EQ(events.size(), 4U);

  EXPECT_EQ(std::get<MouseEvent>(events[0]), (MouseEvent{
                                                 .position = {.x = 9, .y = 19},
                                                 .button   = MouseButton::LEFT,
                                                 .action   = MouseAction::PRESS,
                                             }));
  EXPECT_EQ(std::get<MouseEvent>(events[1]).action, MouseAction::RELEASE);
  EXPECT_EQ(std::get<MouseEvent>(events[2]).action, MouseAction::DRAG);
  EXPECT_EQ(std::get<MouseEvent>(events[2]).button, MouseButton::LEFT);
  EXPECT_EQ(std::get<MouseEvent>(events[3]).action, MouseAction::MOVE);
  EXPECT_EQ(std::get<MouseEvent>(events[3]).button, MouseButton::NONE);
}

TEST(TerminalDecoderTest, DecodesMouseModifiersAndAuxiliaryButtons) {
  Decoder decoder = configured_decoder();
  const std::vector<Event> events =
      decode(decoder, "\x1b[<21;3;4M\x1b[<128;5;6M");
  ASSERT_EQ(events.size(), 2U);
  const MouseEvent& modified = std::get<MouseEvent>(events[0]);
  EXPECT_EQ(modified.button, MouseButton::MIDDLE);
  EXPECT_EQ(modified.modifiers, Modifier::SHIFT | Modifier::CONTROL);
  EXPECT_EQ(std::get<MouseEvent>(events[1]).button, MouseButton::AUXILIARY_1);
}

TEST(TerminalDecoderTest, DecodesAllPrimaryAndAuxiliaryMouseButtons) {
  Decoder decoder                 = configured_decoder();
  const std::vector<Event> events = decode(decoder,
                                           "\x1b[<2;1;1M\x1b[<137;1;1M"
                                           "\x1b[<138;1;1M\x1b[<139;1;1M");
  ASSERT_EQ(events.size(), 4U);
  EXPECT_EQ(std::get<MouseEvent>(events[0]).button, MouseButton::RIGHT);
  EXPECT_EQ(std::get<MouseEvent>(events[1]).button, MouseButton::AUXILIARY_2);
  EXPECT_TRUE(
      std::get<MouseEvent>(events[1]).modifiers.contains(Modifier::ALT));
  EXPECT_EQ(std::get<MouseEvent>(events[2]).button, MouseButton::AUXILIARY_3);
  EXPECT_EQ(std::get<MouseEvent>(events[3]).button, MouseButton::AUXILIARY_4);
}

TEST(TerminalDecoderTest, RejectsUnsupportedAndMalformedMouseReports) {
  constexpr std::array<std::string_view, 5> malformed{
      "\x1b[<0;1M",      // Missing coordinate.
      "\x1b[<;1;1M",     // Missing button number.
      "\x1b[<0;1;1;1M",  // Too many fields.
      "\x1b[<192;1;1M",  // Conflicting wheel and auxiliary groups.
      "\x1b[<256;1;1M",  // Unknown button bit.
  };

  for (const std::string_view sequence : malformed) {
    Decoder decoder                 = configured_decoder();
    const std::vector<Event> events = decode(decoder, sequence);
    ASSERT_EQ(events.size(), 1U) << sequence;
    ASSERT_TRUE(std::holds_alternative<UnknownEvent>(events[0]));
  }
}

TEST(TerminalDecoderTest, NormalizesVerticalAndHorizontalWheelButtons) {
  Decoder decoder                 = configured_decoder();
  const std::vector<Event> events = decode(decoder,
                                           "\x1b[<64;1;1M\x1b[<65;1;1M"
                                           "\x1b[<66;1;1M\x1b[<67;1;1M");
  ASSERT_EQ(events.size(), 4U);
  EXPECT_EQ(std::get<ScrollEvent>(events[0]).delta_y, 1);
  EXPECT_EQ(std::get<ScrollEvent>(events[1]).delta_y, -1);
  EXPECT_EQ(std::get<ScrollEvent>(events[2]).delta_x, 1);
  EXPECT_EQ(std::get<ScrollEvent>(events[3]).delta_x, -1);
}

TEST(TerminalDecoderTest, ReportsMalformedMouseCoordinatesWithoutDesync) {
  Decoder decoder                 = configured_decoder();
  const std::vector<Event> events = decode(decoder, "\x1b[<0;0;2Mx");
  ASSERT_EQ(events.size(), 2U);
  ASSERT_TRUE(std::holds_alternative<UnknownEvent>(events[0]));
  EXPECT_EQ(std::get<UnknownEvent>(events[0]).reason,
            UnknownInputReason::MALFORMED_SEQUENCE);
  EXPECT_EQ(std::get<TextEvent>(events[1]).utf8, "x");
}

TEST(TerminalDecoderTest, StreamsBracketedPasteWithoutParsingItsContents) {
  Decoder decoder = configured_decoder();
  const std::vector<Event> events =
      decode(decoder, "\x1b[200~hello\x1b[A\x03world\x1b[201~x");
  ASSERT_EQ(events.size(), 4U);
  EXPECT_EQ(std::get<PasteEvent>(events[0]).phase, PastePhase::BEGIN);
  EXPECT_EQ(std::get<PasteEvent>(events[1]), (PasteEvent{
                                                 .phase = PastePhase::DATA,
                                                 .data = "hello\x1b[A\x03world",
                                             }));
  EXPECT_EQ(std::get<PasteEvent>(events[2]).phase, PastePhase::END);
  EXPECT_EQ(std::get<TextEvent>(events[3]).utf8, "x");
}

TEST(TerminalDecoderTest, EmptyPasteStillHasBalancedBoundaries) {
  Decoder decoder                 = configured_decoder();
  const std::vector<Event> events = decode(decoder, "\x1b[200~\x1b[201~");
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(std::get<PasteEvent>(events[0]).phase, PastePhase::BEGIN);
  EXPECT_EQ(std::get<PasteEvent>(events[1]).phase, PastePhase::END);
}

TEST(TerminalDecoderTest, PasteMarkersDecodeAcrossEveryChunkBoundary) {
  constexpr std::string_view sequence = "\x1b[200~abc\x1b[201~";
  for (std::size_t split = 0; split <= sequence.size(); ++split) {
    Decoder decoder = configured_decoder();
    std::vector<Event> events;
    ASSERT_EQ(decoder.feed(sequence.substr(0, split), events), Status::OK);
    ASSERT_EQ(decoder.feed(sequence.substr(split), events), Status::OK);
    ASSERT_GE(events.size(), 3U) << split;
    EXPECT_EQ(std::get<PasteEvent>(events.front()).phase, PastePhase::BEGIN);
    EXPECT_EQ(std::get<PasteEvent>(events.back()).phase, PastePhase::END);
    std::string data;
    for (const Event& event : events) {
      if (const auto* paste = std::get_if<PasteEvent>(&event);
          paste != nullptr && paste->phase == PastePhase::DATA) {
        data.append(paste->data);
      }
    }
    EXPECT_EQ(data, "abc") << split;
  }
}

TEST(TerminalDecoderTest, PasteLimitCancelsAndDiscardsUntilEndMarker) {
  Decoder decoder = configured_decoder(DecoderLimits{.maximum_paste_bytes = 5});
  std::vector<Event> events;
  EXPECT_EQ(decoder.feed("\x1b[200~123456\x1b[201~x", events),
            Status::INPUT_LIMIT_EXCEEDED);
  ASSERT_EQ(events.size(), 3U);
  EXPECT_EQ(std::get<PasteEvent>(events[0]).phase, PastePhase::BEGIN);
  EXPECT_EQ(std::get<PasteEvent>(events[1]).phase, PastePhase::CANCEL);
  EXPECT_EQ(std::get<TextEvent>(events[2]).utf8, "x");
  EXPECT_FALSE(decoder.paste_in_progress());
}

TEST(TerminalDecoderTest, DecodesOsc52ResponsesWithStAndBelTerminators) {
  Decoder decoder = configured_decoder();
  const std::vector<Event> events =
      decode(decoder, "\x1b]52;c;Zm9v\x1b\\\x1b]52;p;YmFy\a");
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(std::get<ClipboardEvent>(events[0]),
            (ClipboardEvent{
                .selection = ClipboardSelection::CLIPBOARD,
                .data      = "foo",
            }));
  EXPECT_EQ(std::get<ClipboardEvent>(events[1]),
            (ClipboardEvent{
                .selection = ClipboardSelection::PRIMARY,
                .data      = "bar",
            }));
}

TEST(TerminalDecoderTest, Osc52ResponseDecodesAcrossEveryChunkBoundary) {
  constexpr std::string_view sequence = "\x1b]52;c;Zm9v\x1b\\";
  for (std::size_t split = 0; split <= sequence.size(); ++split) {
    Decoder decoder = configured_decoder();
    std::vector<Event> events;
    ASSERT_EQ(decoder.feed(sequence.substr(0, split), events), Status::OK);
    ASSERT_EQ(decoder.feed(sequence.substr(split), events), Status::OK);
    ASSERT_EQ(events.size(), 1U) << split;
    ASSERT_TRUE(std::holds_alternative<ClipboardEvent>(events[0]));
    EXPECT_EQ(std::get<ClipboardEvent>(events[0]).data, "foo");
  }
}

TEST(TerminalDecoderTest, Osc52MayExceedTheGenericSequenceLimit) {
  const std::string clipboard_data(6000U, 'z');
  std::string sequence;
  ASSERT_EQ(build_clipboard_write(ClipboardSelection::CLIPBOARD, clipboard_data,
                                  sequence, 6000U),
            Status::OK);

  Decoder decoder                 = configured_decoder(DecoderLimits{
      .maximum_sequence_bytes  = 32U,
      .maximum_clipboard_bytes = 6000U,
  });
  const std::vector<Event> events = decode(decoder, sequence);
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(std::get<ClipboardEvent>(events[0]).data, clipboard_data);
}

TEST(TerminalDecoderTest, ClipboardAndGenericSequenceLimitsAreIndependent) {
  {
    Decoder decoder = configured_decoder(DecoderLimits{
        .maximum_sequence_bytes  = 64U,
        .maximum_clipboard_bytes = 2U,
    });
    std::vector<Event> events;
    EXPECT_EQ(decoder.feed("\x1b]52;c;Zm9v\x1b\\", events),
              Status::INPUT_LIMIT_EXCEEDED);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(std::get<UnknownEvent>(events[0]).reason,
              UnknownInputReason::LIMIT_EXCEEDED);
  }
  {
    Decoder decoder =
        configured_decoder(DecoderLimits{.maximum_sequence_bytes = 8U});
    std::vector<Event> events;
    EXPECT_EQ(decoder.feed("\x1b]123456789", events),
              Status::INPUT_LIMIT_EXCEEDED);
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(std::get<UnknownEvent>(events[0]).reason,
              UnknownInputReason::LIMIT_EXCEEDED);
  }
}

TEST(TerminalDecoderTest, ClassifiesClipboardQueriesAndBadSelectors) {
  Decoder decoder                 = configured_decoder();
  const std::vector<Event> events = decode(decoder,
                                           "\x1b]52;c;?\x1b\\\x1b]52;x;Zm9v\a"
                                           "\x1b]52;c\x1b\\");
  ASSERT_EQ(events.size(), 3U);
  EXPECT_EQ(std::get<TerminalResponseEvent>(events[0]).kind,
            TerminalResponseKind::OPERATING_SYSTEM_COMMAND);
  EXPECT_EQ(std::get<UnknownEvent>(events[1]).reason,
            UnknownInputReason::UNSUPPORTED_SEQUENCE);
  EXPECT_EQ(std::get<UnknownEvent>(events[2]).reason,
            UnknownInputReason::MALFORMED_SEQUENCE);
}

TEST(TerminalDecoderTest, MalformedClipboardResponseDoesNotBecomeKeyInput) {
  Decoder decoder                 = configured_decoder();
  const std::vector<Event> events = decode(decoder, "\x1b]52;c;%%%\x1b\\x");
  ASSERT_EQ(events.size(), 2U);
  ASSERT_TRUE(std::holds_alternative<UnknownEvent>(events[0]));
  EXPECT_TRUE(std::get<UnknownEvent>(events[0]).bytes.empty());
  EXPECT_EQ(std::get<TextEvent>(events[1]).utf8, "x");
}

TEST(TerminalDecoderTest, RoutesProtocolRepliesAwayFromKeyboardInput) {
  Decoder decoder                 = configured_decoder();
  const std::vector<Event> events = decode(decoder,
                                           "\x1b[?19u\x1b[12;34R\x1b[?1;2c"
                                           "\x1b]10;rgb:ffff/ffff/ffff\x1b\\"
                                           "\x1bP1+r1234\x1b\\");
  ASSERT_EQ(events.size(), 5U);
  EXPECT_EQ(std::get<TerminalResponseEvent>(events[0]).kind,
            TerminalResponseKind::KITTY_KEYBOARD_FLAGS);
  EXPECT_EQ(std::get<TerminalResponseEvent>(events[0]).value, 19U);
  EXPECT_EQ(std::get<TerminalResponseEvent>(events[1]).kind,
            TerminalResponseKind::CURSOR_POSITION);
  EXPECT_EQ(std::get<TerminalResponseEvent>(events[1]).value, 12U);
  EXPECT_EQ(std::get<TerminalResponseEvent>(events[2]).kind,
            TerminalResponseKind::DEVICE_ATTRIBUTES);
  EXPECT_EQ(std::get<TerminalResponseEvent>(events[3]).kind,
            TerminalResponseKind::OPERATING_SYSTEM_COMMAND);
  EXPECT_EQ(std::get<TerminalResponseEvent>(events[4]).kind,
            TerminalResponseKind::DEVICE_CONTROL_STRING);
}

TEST(TerminalDecoderTest, MalformedCursorReportRemainsObservable) {
  Decoder decoder                 = configured_decoder();
  const std::vector<Event> events = decode(decoder, "\x1b[12R");
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(std::get<UnknownEvent>(events[0]).reason,
            UnknownInputReason::MALFORMED_SEQUENCE);
}

TEST(TerminalDecoderTest, RoutesModeApcAndPrivacyMessageReplies) {
  Decoder decoder                 = configured_decoder();
  const std::vector<Event> events = decode(decoder,
                                           "\x1b[?2026;1$y\x1b_payload\x1b\\"
                                           "\x1b^payload\x1b\\");
  ASSERT_EQ(events.size(), 3U);
  EXPECT_EQ(std::get<TerminalResponseEvent>(events[0]).kind,
            TerminalResponseKind::MODE_REPORT);
  EXPECT_EQ(std::get<TerminalResponseEvent>(events[1]).kind,
            TerminalResponseKind::OPERATING_SYSTEM_COMMAND);
  EXPECT_EQ(std::get<TerminalResponseEvent>(events[2]).kind,
            TerminalResponseKind::OPERATING_SYSTEM_COMMAND);
}

TEST(TerminalDecoderTest, ParameterizedProtocolsDecodeAcrossEveryBoundary) {
  constexpr std::array<std::string_view, 5> sequences{
      "\x1b[97:65:99;6:2;65:769u", "\x1b[<149;123;456M",   "\x1b[?2026;1$y",
      "\x1bP1+r1234\x1b\\",        "\x1b]52;p;Zm9v\x1b\\",
  };

  for (const std::string_view sequence : sequences) {
    Decoder baseline_decoder          = configured_decoder();
    const std::vector<Event> expected = decode(baseline_decoder, sequence);
    ASSERT_EQ(expected.size(), 1U) << sequence;

    for (std::size_t split = 0; split <= sequence.size(); ++split) {
      Decoder decoder = configured_decoder();
      std::vector<Event> events;
      ASSERT_EQ(decoder.feed(sequence.substr(0, split), events), Status::OK);
      ASSERT_EQ(decoder.feed(sequence.substr(split), events), Status::OK);
      EXPECT_EQ(events, expected) << sequence << " at split " << split;
    }
  }
}

TEST(TerminalDecoderTest, BoundedCsiAndDcsRecoverAfterLimitErrors) {
  for (const std::string_view sequence : {"\x1b[12345", "\x1bP12345"}) {
    Decoder decoder =
        configured_decoder(DecoderLimits{.maximum_sequence_bytes = 4U});
    std::vector<Event> events;
    EXPECT_EQ(decoder.feed(sequence, events), Status::INPUT_LIMIT_EXCEEDED);
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(std::get<UnknownEvent>(events[0]).reason,
              UnknownInputReason::LIMIT_EXCEEDED);
    EXPECT_EQ(decoder.pending_bytes(), 0U);
  }
}

TEST(TerminalDecoderTest, MalformedCsiBytesDoNotDesynchronizeText) {
  std::string input = "\x1b[";
  input.push_back('\x1f');
  input.push_back('x');
  Decoder decoder                 = configured_decoder();
  const std::vector<Event> events = decode(decoder, input);
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(std::get<UnknownEvent>(events[0]).reason,
            UnknownInputReason::MALFORMED_SEQUENCE);
  EXPECT_EQ(std::get<TextEvent>(events[1]).utf8, "x");
}

TEST(TerminalDecoderTest, NewEscapeInterruptsCsiAndIsRetriedFromTheRoot) {
  Decoder decoder                 = configured_decoder();
  const std::vector<Event> events = decode(decoder, "\x1b[123\x1b[A");

  ASSERT_EQ(events.size(), 2U);
  ASSERT_TRUE(std::holds_alternative<UnknownEvent>(events[0]));
  EXPECT_EQ(std::get<UnknownEvent>(events[0]),
            (UnknownEvent{
                .reason = UnknownInputReason::INCOMPLETE_SEQUENCE,
                .bytes  = "\x1b[123",
            }));
  ASSERT_TRUE(std::holds_alternative<KeyEvent>(events[1]));
  EXPECT_EQ(std::get<KeyEvent>(events[1]).key, KeyCode{NamedKey::UP});
}

TEST(TerminalDecoderTest, NewEscapeInterruptsOscAndIsRetriedFromTheRoot) {
  Decoder decoder                 = configured_decoder();
  const std::vector<Event> events = decode(decoder, "\x1b]title\x1b[A");

  ASSERT_EQ(events.size(), 2U);
  ASSERT_TRUE(std::holds_alternative<UnknownEvent>(events[0]));
  EXPECT_EQ(std::get<UnknownEvent>(events[0]),
            (UnknownEvent{
                .reason = UnknownInputReason::INCOMPLETE_SEQUENCE,
                .bytes  = "\x1b]title",
            }));
  ASSERT_TRUE(std::holds_alternative<KeyEvent>(events[1]));
  EXPECT_EQ(std::get<KeyEvent>(events[1]).key, KeyCode{NamedKey::UP});
}

TEST(TerminalDecoderTest, UnknownCsiAndSs3SequencesRemainObservable) {
  Decoder decoder                 = configured_decoder();
  const std::vector<Event> events = decode(decoder, "\x1b[999z\x1bOz");
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(std::get<UnknownEvent>(events[0]).reason,
            UnknownInputReason::UNSUPPORTED_SEQUENCE);
  EXPECT_EQ(std::get<UnknownEvent>(events[1]).reason,
            UnknownInputReason::UNSUPPORTED_SEQUENCE);
}

TEST(TerminalDecoderTest, FinishReportsTruncatedUtf8AndCsi) {
  {
    Decoder decoder = configured_decoder();
    std::vector<Event> events;
    ASSERT_EQ(decoder.feed("\xf0\x9f", events), Status::OK);
    ASSERT_EQ(decoder.finish(events), Status::OK);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(std::get<UnknownEvent>(events[0]).reason,
              UnknownInputReason::INCOMPLETE_SEQUENCE);
  }
  {
    Decoder decoder = configured_decoder();
    std::vector<Event> events;
    ASSERT_EQ(decoder.feed("\x1b[12;", events), Status::OK);
    ASSERT_EQ(decoder.finish(events), Status::OK);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(std::get<UnknownEvent>(events[0]).reason,
              UnknownInputReason::INCOMPLETE_SEQUENCE);
  }
}

TEST(TerminalDecoderTest, TextBeforePartialUtf8EmitsWithoutLosingThePrefix) {
  Decoder decoder = configured_decoder();
  std::vector<Event> events;
  ASSERT_EQ(decoder.feed("a\xc3", events), Status::OK);
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(std::get<TextEvent>(events[0]).utf8, "a");
  EXPECT_EQ(decoder.pending_bytes(), 1U);

  ASSERT_EQ(decoder.feed("\xa5", events), Status::OK);
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(std::get<TextEvent>(events[1]).utf8, "\xc3\xa5");
}

TEST(TerminalDecoderTest, FinishReportsAnIncompleteDcs) {
  Decoder decoder = configured_decoder();
  std::vector<Event> events;
  ASSERT_EQ(decoder.feed("\x1bPpartial", events), Status::OK);
  ASSERT_EQ(decoder.finish(events), Status::OK);
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(std::get<UnknownEvent>(events[0]).reason,
            UnknownInputReason::INCOMPLETE_SEQUENCE);
}

TEST(TerminalDecoderTest, FinishResolvesEscapeAndExactConfiguredKeys) {
  {
    Decoder decoder = configured_decoder();
    std::vector<Event> events;
    ASSERT_EQ(decoder.feed("\x1b", events), Status::OK);
    ASSERT_EQ(decoder.finish(events), Status::OK);
    ASSERT_EQ(events.size(), 1U);
    expect_named_key(events[0], NamedKey::ESCAPE);
  }
  {
    Decoder decoder =
        configured_decoder_with_override("finish_exact_key", R"toml(
[[mapping]]
sequence = "<CSI>x"
kind = "key"
key = "F35"

[[mapping]]
sequence = "<CSI>xy"
kind = "key"
key = "F34"
)toml");
    std::vector<Event> events;
    ASSERT_EQ(decoder.feed("\x1b[x", events), Status::OK);
    ASSERT_EQ(decoder.finish(events), Status::OK);
    ASSERT_EQ(events.size(), 1U);
    expect_named_key(events[0], NamedKey::F35);
  }
}

TEST(TerminalDecoderTest, FinishCancelsAnUnterminatedPaste) {
  Decoder decoder = configured_decoder();
  std::vector<Event> events;
  ASSERT_EQ(decoder.feed("\x1b[200~partial", events), Status::OK);
  ASSERT_TRUE(decoder.paste_in_progress());
  ASSERT_EQ(decoder.finish(events), Status::OK);

  ASSERT_EQ(events.size(), 3U);
  EXPECT_EQ(std::get<PasteEvent>(events[0]).phase, PastePhase::BEGIN);
  EXPECT_EQ(std::get<PasteEvent>(events[1]).data, "partial");
  EXPECT_EQ(std::get<PasteEvent>(events[2]).phase, PastePhase::CANCEL);
  EXPECT_FALSE(decoder.paste_in_progress());
}

TEST(TerminalDecoderTest, FinishPreservesPartialPasteEndMarkerAsData) {
  Decoder decoder = configured_decoder();
  std::vector<Event> events;
  ASSERT_EQ(decoder.feed("\x1b[200~data\x1b[20", events), Status::OK);
  ASSERT_EQ(decoder.finish(events), Status::OK);

  ASSERT_EQ(events.size(), 4U);
  EXPECT_EQ(std::get<PasteEvent>(events[0]).phase, PastePhase::BEGIN);
  EXPECT_EQ(std::get<PasteEvent>(events[1]).data, "data");
  EXPECT_EQ(std::get<PasteEvent>(events[2]).data, "\x1b[20");
  EXPECT_EQ(std::get<PasteEvent>(events[3]).phase, PastePhase::CANCEL);
}

TEST(TerminalDecoderTest, FinishDoesNotDuplicateACancelledPasteEvent) {
  Decoder decoder =
      configured_decoder(DecoderLimits{.maximum_paste_bytes = 2U});
  std::vector<Event> events;
  EXPECT_EQ(decoder.feed("\x1b[200~abc", events), Status::INPUT_LIMIT_EXCEEDED);
  ASSERT_EQ(decoder.finish(events), Status::OK);
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(std::get<PasteEvent>(events[0]).phase, PastePhase::BEGIN);
  EXPECT_EQ(std::get<PasteEvent>(events[1]).phase, PastePhase::CANCEL);
}

TEST(TerminalDecoderTest, PendingInputLimitRejectsWithoutBufferingData) {
  Decoder decoder = configured_decoder(DecoderLimits{
      .maximum_pending_bytes  = 4,
      .maximum_sequence_bytes = 4,
  });
  std::vector<Event> events;
  EXPECT_EQ(decoder.feed("12345", events), Status::INPUT_LIMIT_EXCEEDED);
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(std::get<UnknownEvent>(events[0]).reason,
            UnknownInputReason::LIMIT_EXCEEDED);
  EXPECT_EQ(decoder.pending_bytes(), 0U);
}

TEST(TerminalDecoderTest, ResetDiscardsProtocolStateButKeepsConfiguredKeys) {
  Decoder decoder =
      configured_decoder_with_override("reset_configured_key", R"toml(
[[mapping]]
sequence = "<CSI>x"
kind = "key"
key = "F35"
)toml");
  std::vector<Event> events;
  ASSERT_EQ(decoder.feed("\x1b[200~partial", events), Status::OK);
  ASSERT_TRUE(decoder.paste_in_progress());

  decoder.reset();
  EXPECT_FALSE(decoder.paste_in_progress());
  EXPECT_EQ(decoder.pending_bytes(), 0U);

  events.clear();
  ASSERT_EQ(decoder.feed("\x1b[x", events), Status::OK);
  ASSERT_EQ(events.size(), 1U);
  expect_named_key(events[0], NamedKey::F35);
}

}  // namespace
}  // namespace puc::terminal
