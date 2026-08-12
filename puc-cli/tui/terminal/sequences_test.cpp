/**
 * @file sequences_test.cpp
 * @brief Exhaustive tests for PUC-owned terminal control sequences.
 */

#include "puc-cli/tui/terminal/sequences.hpp"

#include <array>
#include <string>
#include <string_view>

#include "gtest/gtest.h"

namespace puc::terminal {
namespace {

struct ModeEncoding {
  Mode mode;
  std::string_view enabled;
  std::string_view disabled;
};

TEST(TerminalSequencesTest, EveryWhitelistedModeHasSymmetricEncodings) {
  constexpr std::array encodings{
      ModeEncoding{Mode::ALTERNATE_SCREEN, "\x1b[?1049h", "\x1b[?1049l"},
      ModeEncoding{Mode::CURSOR_VISIBLE, "\x1b[?25h", "\x1b[?25l"},
      ModeEncoding{Mode::AUTO_WRAP, "\x1b[?7h", "\x1b[?7l"},
      ModeEncoding{Mode::BRACKETED_PASTE, "\x1b[?2004h", "\x1b[?2004l"},
      ModeEncoding{Mode::FOCUS_REPORTING, "\x1b[?1004h", "\x1b[?1004l"},
      ModeEncoding{Mode::MOUSE_BUTTON_TRACKING, "\x1b[?1000h", "\x1b[?1000l"},
      ModeEncoding{Mode::MOUSE_DRAG_TRACKING, "\x1b[?1002h", "\x1b[?1002l"},
      ModeEncoding{Mode::MOUSE_MOTION_TRACKING, "\x1b[?1003h", "\x1b[?1003l"},
      ModeEncoding{Mode::SGR_MOUSE, "\x1b[?1006h", "\x1b[?1006l"},
      ModeEncoding{Mode::APPLICATION_CURSOR, "\x1b[?1h", "\x1b[?1l"},
      ModeEncoding{Mode::APPLICATION_KEYPAD, "\x1b=", "\x1b>"},
      ModeEncoding{Mode::SYNCHRONIZED_OUTPUT, "\x1b[?2026h", "\x1b[?2026l"},
  };

  for (const ModeEncoding& encoding : encodings) {
    EXPECT_EQ(mode_sequence(encoding.mode, true), encoding.enabled);
    EXPECT_EQ(mode_sequence(encoding.mode, false), encoding.disabled);
  }
}

TEST(TerminalSequencesTest, UnknownModeHasNoUnsafeFallbackSequence) {
  constexpr Mode unknown = static_cast<Mode>(999);
  EXPECT_TRUE(mode_sequence(unknown, true).empty());
  EXPECT_TRUE(mode_sequence(unknown, false).empty());
}

TEST(TerminalSequencesTest, KittyQueriesAndStackPopAreStatic) {
  EXPECT_EQ(kitty_keyboard_query(), "\x1b[?u");
  EXPECT_EQ(kitty_keyboard_pop(), "\x1b[<u");
}

TEST(TerminalSequencesTest, KittyPushEncodesEveryValidFlagBit) {
  std::string output;
  EXPECT_EQ(build_kitty_keyboard_push(0, output), Status::OK);
  EXPECT_EQ(output, "\x1b[>0u");

  EXPECT_EQ(build_kitty_keyboard_push(31, output), Status::OK);
  EXPECT_EQ(output, "\x1b[>31u");

  const std::uint32_t selected =
      KeyboardEnhancement::DISAMBIGUATE_ESCAPE_CODES |
      KeyboardEnhancement::REPORT_EVENT_TYPES;
  EXPECT_EQ(build_kitty_keyboard_push(selected, output), Status::OK);
  EXPECT_EQ(output, "\x1b[>3u");
}

TEST(TerminalSequencesTest, KittyPushRejectsUnknownFlagsWithoutMutation) {
  std::string output = "unchanged";
  EXPECT_EQ(build_kitty_keyboard_push(32, output), Status::INVALID_ARGUMENT);
  EXPECT_EQ(output, "unchanged");
}

}  // namespace
}  // namespace puc::terminal
