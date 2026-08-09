/**
 * @file event_test.cpp
 * @brief Tests for normalized terminal event value types.
 */

#include "puc-cli/terminal/event.hpp"

#include <string>
#include <variant>

#include "gtest/gtest.h"

namespace puc::terminal {
namespace {

TEST(TerminalEventTest, ModifierSetsCombineAndInspectBits) {
  Modifiers modifiers = Modifier::SHIFT | Modifier::CONTROL;
  modifiers.add(Modifier::SUPER);

  EXPECT_TRUE(modifiers.contains(Modifier::SHIFT));
  EXPECT_TRUE(modifiers.contains(Modifier::CONTROL));
  EXPECT_TRUE(modifiers.contains(Modifier::SUPER));
  EXPECT_FALSE(modifiers.contains(Modifier::ALT));
  EXPECT_EQ(modifiers.bits(), 0b1101U);
  EXPECT_EQ(Modifiers::from_bits(modifiers.bits()), modifiers);
}

TEST(TerminalEventTest, KeyCodesPreserveNamedAndUnicodeAlternatives) {
  const KeyCode default_key;
  const KeyCode named{NamedKey::PAGE_DOWN};
  const KeyCode unicode{U'λ'};

  EXPECT_EQ(default_key, KeyCode{NamedKey::ESCAPE});
  ASSERT_TRUE(std::holds_alternative<NamedKey>(named.value));
  EXPECT_EQ(std::get<NamedKey>(named.value), NamedKey::PAGE_DOWN);
  ASSERT_TRUE(std::holds_alternative<char32_t>(unicode.value));
  EXPECT_EQ(std::get<char32_t>(unicode.value), U'λ');
}

TEST(TerminalEventTest, NormalizedEventsHaveValueSemantics) {
  const KeyEvent key{.key = U'x', .modifiers = Modifier::ALT};
  const TextEvent text{.utf8 = "text"};
  const MouseEvent mouse{
      .position = {.x = 1, .y = 2},
      .button   = MouseButton::RIGHT,
      .action   = MouseAction::PRESS,
  };
  const ScrollEvent scroll{.position = {.x = 3, .y = 4}, .delta_y = -1};
  const PasteEvent paste{.phase = PastePhase::DATA, .data = "data"};
  const FocusEvent focus{.focused = true};
  const ClipboardEvent clipboard{
      .selection = ClipboardSelection::PRIMARY,
      .data      = "clip",
  };
  const CommandEvent command{.command = Command::COPY};
  const TerminalResponseEvent response{
      .kind  = TerminalResponseKind::MODE_REPORT,
      .value = 1,
      .bytes = "reply",
  };
  const UnknownEvent unknown{
      .reason = UnknownInputReason::MALFORMED_SEQUENCE,
      .bytes  = "bad",
  };

  EXPECT_EQ(key, key);
  EXPECT_EQ(text, text);
  EXPECT_EQ(mouse, mouse);
  EXPECT_EQ(scroll, scroll);
  EXPECT_EQ(paste, paste);
  EXPECT_EQ(focus, focus);
  EXPECT_EQ(clipboard, clipboard);
  EXPECT_EQ(command, command);
  EXPECT_EQ(response, response);
  EXPECT_EQ(unknown, unknown);
}

TEST(TerminalEventTest, VariantRetainsOwnedEventPayloads) {
  Event event = PasteEvent{
      .phase = PastePhase::DATA,
      .data  = "pasted text",
  };

  ASSERT_TRUE(std::holds_alternative<PasteEvent>(event));
  EXPECT_EQ(std::get<PasteEvent>(event).data, "pasted text");

  event = ClipboardEvent{
      .selection = ClipboardSelection::PRIMARY,
      .data      = "clipboard text",
  };
  ASSERT_TRUE(std::holds_alternative<ClipboardEvent>(event));
  EXPECT_EQ(std::get<ClipboardEvent>(event).selection,
            ClipboardSelection::PRIMARY);
  EXPECT_EQ(std::get<ClipboardEvent>(event).data, "clipboard text");

  event = CommandEvent{.command = Command::COPY};
  ASSERT_TRUE(std::holds_alternative<CommandEvent>(event));
  EXPECT_EQ(std::get<CommandEvent>(event).command, Command::COPY);
}

TEST(TerminalEventTest, CellCoordinatesAreZeroBasedValueTypes) {
  constexpr CellPosition position{.x = 10, .y = 20};

  static_assert(position == CellPosition{.x = 10, .y = 20});
}

}  // namespace
}  // namespace puc::terminal
