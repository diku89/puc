/**
 * @file event_messages_test.cpp
 * @brief Tests for terminal event/message conversion.
 */

#include "puc-cli/tui/terminal/event_messages.hpp"

#include <array>

#include "gtest/gtest.h"

namespace puc::terminal {
namespace {

TEST(EventMessagesTest, RoundTripsEveryTerminalEventAlternative) {
  const std::array<Event, 10U> events{{
      KeyEvent{.key             = KeyCode{U'x'},
               .modifiers       = Modifier::SHIFT | Modifier::CONTROL,
               .action          = KeyAction::REPEAT,
               .shifted_key     = U'X',
               .base_layout_key = U'x',
               .text            = "x"},
      TextEvent{.utf8 = "text ✓"},
      MouseEvent{.position  = {.x = 2U, .y = 3U},
                 .button    = MouseButton::LEFT,
                 .action    = MouseAction::DRAG,
                 .modifiers = Modifier::ALT},
      ScrollEvent{.position  = {.x = 4U, .y = 5U},
                  .delta_x   = -1,
                  .delta_y   = 2,
                  .modifiers = Modifier::CONTROL},
      PasteEvent{.phase = PastePhase::DATA, .data = "paste"},
      FocusEvent{.focused = true},
      ClipboardEvent{.selection = ClipboardSelection::PRIMARY, .data = "clip"},
      CommandEvent{.command = Command::MOVE_WORD_LEFT},
      TerminalResponseEvent{.kind  = TerminalResponseKind::CURSOR_POSITION,
                            .value = 42U,
                            .bytes = "reply"},
      UnknownEvent{.reason = UnknownInputReason::INVALID_UTF8, .bytes = "bad"},
  }};

  for (const Event& expected : events) {
    msg::TerminalInputEvent message;
    ASSERT_EQ(to_message(expected, message), msg::Status::OK);
    Event decoded;
    ASSERT_EQ(from_message(message, decoded), msg::Status::OK);
    EXPECT_EQ(decoded, expected);
  }
}

}  // namespace
}  // namespace puc::terminal
