/**
 * @file input_frame_test.cpp
 * @brief Tests for input editing, modes, selection, layout, and rendering.
 */

#include "puc-cli/tui/frames/input_frame.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>

#include "gtest/gtest.h"
#include "puc-cli/tui/rendering/canvas.hpp"
#include "puc-cli/tui/rendering/screen.hpp"
#include "puc-cli/tui/rendering/status.hpp"
#include "puc-cli/tui/rendering/theme.hpp"
#include "puc-cli/tui/rendering/zbuf.hpp"
#include "puc-cli/tui/terminal/event.hpp"
#include "utils/multithreading/job_queue.hpp"

namespace puc::tui {
namespace {

using namespace std::chrono_literals;

/** Return a palette whose values expose every InputFrame color role. */
Theme input_theme() {
  Theme theme;
  Theme::Colors colors{};
  colors.primary              = 10U;
  colors.secondary            = 3U;
  colors.tertiary             = 9U;
  colors.highlight_background = 7U;
  colors.highlight_text       = 8U;
  colors.text                 = 1U;
  colors.text_secondary       = 4U;
  colors.text_muted           = 5U;
  colors.text_error           = 11U;
  colors.text_warning         = 12U;
  colors.text_success         = 6U;
  colors.text_info            = 13U;
  colors.text_emphasis        = 14U;
  colors.background           = 2U;
  theme.load_colors(colors);
  return theme;
}

/** Feed one committed-text event. */
void type(InputFrame& frame, std::string text,
          InputFrame::Clock::time_point now = {}) {
  ASSERT_EQ(
      frame.handle_event(
          terminal::Event{terminal::TextEvent{.utf8 = std::move(text)}}, now),
      Status::OK);
}

/** Feed one named key press. */
void key(InputFrame& frame, terminal::NamedKey named,
         terminal::Modifiers modifiers     = {},
         InputFrame::Clock::time_point now = {}) {
  ASSERT_EQ(frame.handle_event(terminal::Event{terminal::KeyEvent{
                                   .key       = named,
                                   .modifiers = modifiers,
                               }},
                               now),
            Status::OK);
}

/** Feed one configured semantic command. */
void command(InputFrame& frame, terminal::Command value,
             InputFrame::Clock::time_point now = {}) {
  ASSERT_EQ(frame.handle_event(
                terminal::Event{terminal::CommandEvent{.command = value}}, now),
            Status::OK);
}

/** Draw at the bottom of a 40x25 Canvas and publish the result. */
Canvas::Rect draw(InputFrame& frame, Canvas& canvas, const Theme& theme,
                  std::size_t height = 5U) {
  const auto [width, screen_height] = canvas.get_dimensions();
  const Canvas::Rect rect{
      .x      = 0U,
      .y      = screen_height - height,
      .width  = width,
      .height = height,
  };
  EXPECT_EQ(canvas.begin_frame(), Status::OK);
  EXPECT_EQ(frame.draw(theme, canvas, rect), Status::OK);
  EXPECT_EQ(canvas.end_frame(), Status::OK);
  return rect;
}

/** Return one published cell by absolute canvas coordinates. */
const Canvas::Cell& at(const Canvas& canvas, std::size_t x, std::size_t y) {
  const auto [width, height] = canvas.get_dimensions();
  static_cast<void>(height);
  return canvas.get_drawable_buffer()[y * width + x];
}

TEST(InputFrameGeometryTest, AppliesMinimumAndScreenRelativeMaximumHeights) {
  EXPECT_EQ(InputFrame::maximum_height(0U), 0U);
  EXPECT_EQ(InputFrame::maximum_height(5U), 5U);
  EXPECT_EQ(InputFrame::maximum_height(10U), 7U);
  EXPECT_EQ(InputFrame::maximum_height(40U), 8U);
  EXPECT_EQ(InputFrame::maximum_height(100U), 20U);

  InputFrame frame;
  EXPECT_EQ(frame.preferred_height(40U, 40U), 5U);
  type(frame, "one\ntwo\nthree\nfour");
  EXPECT_EQ(frame.preferred_height(40U, 40U), 8U);
  EXPECT_EQ(frame.preferred_height(40U, 6U), 6U);
}

TEST(InputFrameGeometryTest, RejectsNarrowPartialAndOverheightRectangles) {
  const Theme theme = input_theme();

  Canvas narrow(39U, 25U);
  ASSERT_EQ(narrow.begin_frame(), Status::OK);
  InputFrame narrow_frame;
  EXPECT_EQ(narrow_frame.draw(
                theme, narrow,
                Canvas::Rect{.x = 0U, .y = 20U, .width = 39U, .height = 5U}),
            Status::INVALID_DIMENSIONS);
  EXPECT_EQ(narrow.cancel_frame(), Status::OK);

  Canvas canvas(40U, 25U);
  ASSERT_EQ(canvas.begin_frame(), Status::OK);
  InputFrame frame;
  EXPECT_EQ(
      frame.draw(theme, canvas,
                 Canvas::Rect{.x = 1U, .y = 20U, .width = 39U, .height = 5U}),
      Status::INVALID_DIMENSIONS);
  EXPECT_EQ(
      frame.draw(theme, canvas,
                 Canvas::Rect{.x = 0U, .y = 17U, .width = 40U, .height = 8U}),
      Status::INVALID_DIMENSIONS);
  EXPECT_EQ(canvas.cancel_frame(), Status::OK);
}

TEST(InputFrameEditingTest, InsertsMovesDeletesAndJoinsLogicalLines) {
  InputFrame frame;
  type(frame, "hello");
  key(frame, terminal::NamedKey::LEFT);
  key(frame, terminal::NamedKey::LEFT);
  key(frame, terminal::NamedKey::BACKSPACE);
  EXPECT_EQ(frame.snapshot().input_text, "helo");
  EXPECT_EQ(frame.snapshot().cursor, (InputCursor{.line = 0U, .column = 2U}));

  key(frame, terminal::NamedKey::DELETE_KEY);
  EXPECT_EQ(frame.snapshot().input_text, "heo");
  key(frame, terminal::NamedKey::ENTER,
      terminal::Modifiers{terminal::Modifier::SHIFT});
  type(frame, "X");
  EXPECT_EQ(frame.snapshot().input_text, "he\nXo");
  EXPECT_EQ(frame.snapshot().cursor, (InputCursor{.line = 1U, .column = 1U}));

  key(frame, terminal::NamedKey::HOME);
  key(frame, terminal::NamedKey::BACKSPACE);
  EXPECT_EQ(frame.snapshot().input_text, "heXo");
  EXPECT_EQ(frame.snapshot().cursor, (InputCursor{.line = 0U, .column = 2U}));
}

TEST(InputFrameEditingTest, PlainEnterIsReservedAndShiftEnterAddsANewline) {
  InputFrame frame;
  type(frame, "draft");
  key(frame, terminal::NamedKey::ENTER);
  EXPECT_EQ(frame.snapshot().input_text, "draft");
  EXPECT_EQ(frame.snapshot().cursor, (InputCursor{.line = 0U, .column = 5U}));

  key(frame, terminal::NamedKey::ENTER,
      terminal::Modifiers{terminal::Modifier::SHIFT});
  EXPECT_EQ(frame.snapshot().input_text, "draft\n");
  EXPECT_EQ(frame.snapshot().cursor, (InputCursor{.line = 1U, .column = 0U}));
}

TEST(InputFrameEditingTest, TreatsUnicodeScalarsAsCaretAndDeletionUnits) {
  InputFrame frame;
  type(frame, "h\xc3\xa9\xf0\x9f\x98\x80");
  EXPECT_EQ(frame.snapshot().cursor.column, 3U);
  key(frame, terminal::NamedKey::BACKSPACE);
  EXPECT_EQ(frame.snapshot().input_text, "h\xc3\xa9");
  EXPECT_EQ(frame.snapshot().cursor.column, 2U);
}

TEST(InputFrameEditingTest, UsesAssociatedTextFromEnhancedPrintableKeys) {
  InputFrame frame;
  ASSERT_EQ(frame.handle_event(terminal::Event{terminal::KeyEvent{
                .key         = U'a',
                .modifiers   = terminal::Modifier::SHIFT,
                .shifted_key = U'A',
                .text        = "\xc3\xa5",
            }}),
            Status::OK);

  EXPECT_EQ(frame.snapshot().input_text, "\xc3\xa5");
  EXPECT_EQ(frame.snapshot().cursor.column, 1U);
}

TEST(InputFramePasteTest,
     StreamsSplitUtf8NormalizesNewlinesAndRollsBackCancel) {
  InputFrame frame;
  type(frame, "before ");
  EXPECT_EQ(frame.handle_event(terminal::Event{
                terminal::PasteEvent{.phase = terminal::PastePhase::BEGIN}}),
            Status::OK);
  EXPECT_EQ(frame.handle_event(terminal::Event{terminal::PasteEvent{
                .phase = terminal::PastePhase::DATA, .data = "A\xf0\x9f"}}),
            Status::OK);
  EXPECT_EQ(frame.handle_event(terminal::Event{terminal::PasteEvent{
                .phase = terminal::PastePhase::DATA, .data = "\x98\x80\r\nB"}}),
            Status::OK);
  EXPECT_EQ(frame.handle_event(terminal::Event{
                terminal::PasteEvent{.phase = terminal::PastePhase::END}}),
            Status::OK);
  EXPECT_EQ(frame.snapshot().input_text, "before A\xf0\x9f\x98\x80\nB");

  EXPECT_EQ(frame.handle_event(terminal::Event{
                terminal::PasteEvent{.phase = terminal::PastePhase::BEGIN}}),
            Status::OK);
  EXPECT_EQ(frame.handle_event(terminal::Event{terminal::PasteEvent{
                .phase = terminal::PastePhase::DATA, .data = " discarded"}}),
            Status::OK);
  EXPECT_EQ(frame.handle_event(terminal::Event{
                terminal::PasteEvent{.phase = terminal::PastePhase::CANCEL}}),
            Status::OK);
  EXPECT_EQ(frame.snapshot().input_text, "before A\xf0\x9f\x98\x80\nB");
}

TEST(InputFrameNavigationTest, AppliesConfiguredWordRowAndBufferCommands) {
  InputFrame frame;
  type(frame, "one two\nabc\ndef");

  command(frame, terminal::Command::MOVE_BUFFER_START);
  EXPECT_EQ(frame.snapshot().cursor, InputCursor{});
  command(frame, terminal::Command::MOVE_WORD_RIGHT);
  EXPECT_EQ(frame.snapshot().cursor, (InputCursor{.line = 0U, .column = 4U}));
  command(frame, terminal::Command::MOVE_ROW_END);
  EXPECT_EQ(frame.snapshot().cursor, (InputCursor{.line = 0U, .column = 7U}));
  command(frame, terminal::Command::MOVE_WORD_LEFT);
  EXPECT_EQ(frame.snapshot().cursor, (InputCursor{.line = 0U, .column = 4U}));
  command(frame, terminal::Command::MOVE_BUFFER_END);
  EXPECT_EQ(frame.snapshot().cursor, (InputCursor{.line = 2U, .column = 3U}));
  command(frame, terminal::Command::MOVE_ROW_START);
  EXPECT_EQ(frame.snapshot().cursor, (InputCursor{.line = 2U, .column = 0U}));
}

TEST(InputFrameNavigationTest, AppliesModifiedArrowFallbacks) {
  InputFrame frame;
  type(frame, "one two\nabc\ndef");

  key(frame, terminal::NamedKey::UP,
      terminal::Modifiers{terminal::Modifier::SUPER});
  EXPECT_EQ(frame.snapshot().cursor, InputCursor{});
  key(frame, terminal::NamedKey::RIGHT,
      terminal::Modifiers{terminal::Modifier::ALT});
  EXPECT_EQ(frame.snapshot().cursor, (InputCursor{.line = 0U, .column = 4U}));
  key(frame, terminal::NamedKey::RIGHT,
      terminal::Modifiers{terminal::Modifier::SUPER});
  EXPECT_EQ(frame.snapshot().cursor, (InputCursor{.line = 0U, .column = 7U}));
  key(frame, terminal::NamedKey::LEFT,
      terminal::Modifiers{terminal::Modifier::ALT});
  EXPECT_EQ(frame.snapshot().cursor, (InputCursor{.line = 0U, .column = 4U}));
  key(frame, terminal::NamedKey::DOWN,
      terminal::Modifiers{terminal::Modifier::META});
  EXPECT_EQ(frame.snapshot().cursor, (InputCursor{.line = 2U, .column = 3U}));
  key(frame, terminal::NamedKey::LEFT,
      terminal::Modifiers{terminal::Modifier::SUPER});
  EXPECT_EQ(frame.snapshot().cursor, (InputCursor{.line = 2U, .column = 0U}));
}

TEST(InputFrameEscapeTest,
     ClearsOnlyForTwoEscapesWithinFiveHundredMilliseconds) {
  InputFrame frame;
  const auto start = InputFrame::Clock::time_point{} + 1s;
  type(frame, "keep", start);
  key(frame, terminal::NamedKey::ESCAPE, {}, start + 10ms);
  EXPECT_TRUE(frame.snapshot().escape_armed);
  EXPECT_EQ(frame.snapshot().input_text, "keep");

  // Exactly 500 ms is the timeout boundary, so this Escape starts a new
  // interval instead of clearing the draft.
  key(frame, terminal::NamedKey::ESCAPE, {}, start + 510ms);
  EXPECT_EQ(frame.snapshot().input_text, "keep");
  EXPECT_TRUE(frame.snapshot().escape_armed);
  key(frame, terminal::NamedKey::ESCAPE, {}, start + 1009ms);
  EXPECT_TRUE(frame.snapshot().input_text.empty());
  EXPECT_FALSE(frame.snapshot().escape_armed);
}

TEST(InputFrameEscapeTest, AltEscapeRepresentsDecoderNormalizedDoubleEscape) {
  InputFrame frame;
  type(frame, "clear me");
  key(frame, terminal::NamedKey::ESCAPE,
      terminal::Modifiers{terminal::Modifier::ALT});
  EXPECT_TRUE(frame.snapshot().input_text.empty());
}

TEST(InputFrameEscapeTest, OneEscapePromptExpiresWithoutFurtherInput) {
  const Theme theme = input_theme();
  Canvas canvas(40U, 25U);
  InputFrame frame;
  const auto start = InputFrame::Clock::time_point{} + 1s;
  type(frame, "keep", start);
  key(frame, terminal::NamedKey::ESCAPE, {}, start + 1ms);
  Canvas::Rect rect = draw(frame, canvas, theme);
  EXPECT_EQ(at(canvas, 2U, rect.y + 3U).character, U'h');

  frame.advance_time(start + 501ms);
  EXPECT_FALSE(frame.snapshot().escape_armed);
  rect = draw(frame, canvas, theme);
  EXPECT_EQ(at(canvas, 2U, rect.y + 3U).character, U'─');
  EXPECT_EQ(frame.snapshot().input_text, "keep");
}

TEST(InputFrameCommandModeTest, UsesANewGreenBufferAndPreservesNormalInput) {
  InputFrame frame;
  const auto start = InputFrame::Clock::time_point{} + 1s;
  type(frame, "preserved", start);
  command(frame, terminal::Command::ENTER_COMMAND_MODE, start + 1ms);
  type(frame, "status", start + 2ms);
  EXPECT_EQ(frame.snapshot().mode, InputMode::COMMAND);
  EXPECT_EQ(frame.snapshot().input_text, "preserved");
  EXPECT_EQ(frame.snapshot().command_text, "status");

  key(frame, terminal::NamedKey::ESCAPE, {}, start + 3ms);
  key(frame, terminal::NamedKey::ESCAPE, {}, start + 502ms);
  EXPECT_EQ(frame.snapshot().mode, InputMode::NORMAL);
  EXPECT_EQ(frame.snapshot().input_text, "preserved");

  command(frame, terminal::Command::ENTER_COMMAND_MODE, start + 504ms);
  EXPECT_TRUE(frame.snapshot().command_text.empty());
}

TEST(InputFrameCommandModeTest, EscapeColonFallbackEntersCommandMode) {
  InputFrame frame;
  const auto start = InputFrame::Clock::time_point{} + 1s;
  type(frame, "normal", start);
  key(frame, terminal::NamedKey::ESCAPE, {}, start + 1ms);
  type(frame, ":go", start + 2ms);

  EXPECT_EQ(frame.snapshot().mode, InputMode::COMMAND);
  EXPECT_EQ(frame.snapshot().input_text, "normal");
  EXPECT_EQ(frame.snapshot().command_text, "go");
}

TEST(InputFrameCommandModeTest,
     EscapeColonFallbackAcceptsEnhancedAssociatedText) {
  InputFrame frame;
  const auto start = InputFrame::Clock::time_point{} + 1s;
  type(frame, "normal", start);
  key(frame, terminal::NamedKey::ESCAPE, {}, start + 1ms);
  key(frame, terminal::NamedKey::LEFT_SHIFT,
      terminal::Modifiers{terminal::Modifier::SHIFT}, start + 2ms);
  ASSERT_EQ(frame.handle_event(terminal::Event{terminal::KeyEvent{
                                   .key         = U';',
                                   .modifiers   = terminal::Modifier::SHIFT,
                                   .shifted_key = U':',
                                   .text        = ":",
                               }},
                               start + 3ms),
            Status::OK);

  EXPECT_EQ(frame.snapshot().mode, InputMode::COMMAND);
  EXPECT_EQ(frame.snapshot().input_text, "normal");
  EXPECT_TRUE(frame.snapshot().command_text.empty());
  EXPECT_FALSE(frame.snapshot().escape_armed);
}

TEST(InputFrameCommandModeTest,
     EscapeColonFallbackAcceptsEnhancedShiftedKeyWithoutText) {
  InputFrame frame;
  const auto start = InputFrame::Clock::time_point{} + 1s;
  type(frame, "normal", start);
  key(frame, terminal::NamedKey::ESCAPE, {}, start + 1ms);
  ASSERT_EQ(frame.handle_event(terminal::Event{terminal::KeyEvent{
                                   .key         = U';',
                                   .modifiers   = terminal::Modifier::SHIFT,
                                   .shifted_key = U':',
                               }},
                               start + 2ms),
            Status::OK);

  EXPECT_EQ(frame.snapshot().mode, InputMode::COMMAND);
  EXPECT_EQ(frame.snapshot().input_text, "normal");
  EXPECT_TRUE(frame.snapshot().command_text.empty());
}

TEST(InputFrameCommandModeTest, EscapeColonDoesNotFireAtTimeoutBoundary) {
  InputFrame frame;
  const auto start = InputFrame::Clock::time_point{} + 1s;
  type(frame, "normal", start);
  key(frame, terminal::NamedKey::ESCAPE, {}, start + 1ms);
  frame.advance_time(start + 501ms);
  ASSERT_EQ(frame.handle_event(terminal::Event{terminal::KeyEvent{
                                   .key         = U';',
                                   .modifiers   = terminal::Modifier::SHIFT,
                                   .shifted_key = U':',
                                   .text        = ":",
                               }},
                               start + 501ms),
            Status::OK);

  EXPECT_EQ(frame.snapshot().mode, InputMode::NORMAL);
  EXPECT_EQ(frame.snapshot().input_text, "normal:");
  EXPECT_FALSE(frame.snapshot().escape_armed);
}

TEST(InputFrameTerminalModeTest,
     EscapeGreaterThanFallbackPreservesNormalInputAndRequestsASession) {
  InputFrame frame;
  const auto start = InputFrame::Clock::time_point{} + 1s;
  type(frame, "normal", start);
  key(frame, terminal::NamedKey::ESCAPE, {}, start + 1ms);
  key(frame, terminal::NamedKey::LEFT_SHIFT,
      terminal::Modifiers{terminal::Modifier::SHIFT}, start + 2ms);
  ASSERT_EQ(frame.handle_event(terminal::Event{terminal::KeyEvent{
                                   .key         = U'.',
                                   .modifiers   = terminal::Modifier::SHIFT,
                                   .shifted_key = U'>',
                                   .text        = ">",
                               }},
                               start + 3ms),
            Status::OK);

  const InputFrameSnapshot state = frame.snapshot();
  EXPECT_EQ(state.mode, InputMode::TERMINAL);
  EXPECT_EQ(state.input_text, "normal");
  EXPECT_TRUE(state.terminal_session_active);
  EXPECT_EQ(state.terminal_generation, 1U);
  EXPECT_FALSE(state.escape_armed);
}

TEST(InputFrameTerminalModeTest,
     EscapeGreaterThanFallbackAlsoTransitionsFromCommandMode) {
  InputFrame frame;
  const auto start = InputFrame::Clock::time_point{} + 1s;
  command(frame, terminal::Command::ENTER_COMMAND_MODE, start);
  type(frame, "qu", start + 1ms);
  key(frame, terminal::NamedKey::ESCAPE, {}, start + 2ms);
  type(frame, ">", start + 3ms);

  const InputFrameSnapshot state = frame.snapshot();
  EXPECT_EQ(state.mode, InputMode::TERMINAL);
  EXPECT_EQ(state.command_text, "qu");
  EXPECT_TRUE(state.terminal_session_active);
  EXPECT_FALSE(state.escape_armed);
}

TEST(InputFrameTerminalModeTest,
     DoubleEscapeReturnsToInputWithoutClosingThePersistentSession) {
  InputFrame frame;
  const auto start = InputFrame::Clock::time_point{} + 1s;
  type(frame, "preserved", start);
  command(frame, terminal::Command::ENTER_TERMINAL_MODE, start + 1ms);
  const std::size_t generation = frame.snapshot().terminal_generation;

  key(frame, terminal::NamedKey::ESCAPE, {}, start + 2ms);
  EXPECT_EQ(frame.snapshot().mode, InputMode::TERMINAL);
  EXPECT_TRUE(frame.snapshot().escape_armed);
  key(frame, terminal::NamedKey::ESCAPE, {}, start + 3ms);

  const InputFrameSnapshot state = frame.snapshot();
  EXPECT_EQ(state.mode, InputMode::NORMAL);
  EXPECT_EQ(state.input_text, "preserved");
  EXPECT_TRUE(state.terminal_session_active);
  EXPECT_EQ(state.terminal_generation, generation);
  EXPECT_FALSE(state.escape_armed);
  EXPECT_EQ(frame.minimum_height(), InputFrame::kMinimumHeight);
}

TEST(InputFrameTerminalModeTest,
     DecoderNormalizedDoubleEscapeReturnsDirectlyToInput) {
  InputFrame frame;
  command(frame, terminal::Command::ENTER_TERMINAL_MODE);
  key(frame, terminal::NamedKey::ESCAPE,
      terminal::Modifiers{terminal::Modifier::ALT});

  EXPECT_EQ(frame.snapshot().mode, InputMode::NORMAL);
  EXPECT_TRUE(frame.snapshot().terminal_session_active);
}

TEST(InputFrameTerminalModeTest,
     CloseReturnsToPreservedNormalAndTheNextOpenStartsANewGeneration) {
  InputFrame frame;
  type(frame, "preserved");
  command(frame, terminal::Command::ENTER_TERMINAL_MODE);
  const std::size_t first_generation = frame.snapshot().terminal_generation;
  type(frame, "not editor input");
  EXPECT_EQ(frame.snapshot().input_text, "preserved");

  frame.close_terminal();
  InputFrameSnapshot state = frame.snapshot();
  EXPECT_EQ(state.mode, InputMode::NORMAL);
  EXPECT_FALSE(state.terminal_session_active);
  EXPECT_EQ(state.input_text, "preserved");

  command(frame, terminal::Command::ENTER_TERMINAL_MODE);
  state = frame.snapshot();
  EXPECT_TRUE(state.terminal_session_active);
  EXPECT_GT(state.terminal_generation, first_generation);
}

TEST(InputFrameTerminalModeTest,
     ResetKeepsTheSessionWhileStartNewAdvancesItsGeneration) {
  InputFrame frame;
  command(frame, terminal::Command::ENTER_TERMINAL_MODE);
  const std::size_t first_generation = frame.snapshot().terminal_generation;
  frame.reset_terminal();
  EXPECT_TRUE(frame.snapshot().terminal_session_active);
  EXPECT_EQ(frame.snapshot().terminal_generation, first_generation);

  frame.start_new_terminal();
  EXPECT_EQ(frame.snapshot().mode, InputMode::TERMINAL);
  EXPECT_TRUE(frame.snapshot().terminal_session_active);
  EXPECT_GT(frame.snapshot().terminal_generation, first_generation);
}

TEST(InputFrameTerminalModeTest, UsesTheLowerSixtyPercentOfTheScreen) {
  EXPECT_EQ(InputFrame::terminal_height(0U), 0U);
  EXPECT_EQ(InputFrame::terminal_height(5U), 5U);
  EXPECT_EQ(InputFrame::terminal_height(10U), 6U);
  EXPECT_EQ(InputFrame::terminal_height(25U), 15U);
  EXPECT_EQ(InputFrame::terminal_height(40U), 24U);

  InputFrame frame;
  command(frame, terminal::Command::ENTER_TERMINAL_MODE);
  EXPECT_EQ(frame.minimum_height(), InputFrame::kTerminalMinimumHeight);
  EXPECT_EQ(frame.preferred_height(80U, 40U), 24U);
  frame.close_terminal();
  EXPECT_EQ(frame.minimum_height(), InputFrame::kMinimumHeight);
}

TEST(InputFrameTerminalModeTest,
     RendersLibTmtOutputAnsiColorsPurpleBorderAndCursor) {
  const Theme theme = input_theme();
  Canvas canvas(40U, 25U);
  InputFrame frame;
  command(frame, terminal::Command::ENTER_TERMINAL_MODE);
  ASSERT_EQ(frame.write_terminal("\x1b[31mR\x1b[0m"), Status::OK);
  const Canvas::Rect rect = draw(frame, canvas, theme, 15U);

  EXPECT_EQ(at(canvas, 0U, rect.y + 1U).foreground_color, 9U);
  EXPECT_EQ(at(canvas, 4U, rect.y + 2U).character, U'R');
  EXPECT_EQ(at(canvas, 4U, rect.y + 2U).foreground_color, 11U);
  EXPECT_EQ(at(canvas, 5U, rect.y + 2U).background_color, 9U);
  EXPECT_EQ(frame.snapshot().terminal_rows, 11U);
  EXPECT_EQ(frame.snapshot().terminal_columns, 35U);
}

TEST(InputFrameTerminalModeTest, ResizesThePersistentLibTmtSurface) {
  const Theme theme = input_theme();
  InputFrame frame;
  command(frame, terminal::Command::ENTER_TERMINAL_MODE);
  ASSERT_EQ(frame.write_terminal("kept"), Status::OK);

  Canvas first(40U, 25U);
  static_cast<void>(draw(frame, first, theme, 15U));
  EXPECT_EQ(frame.snapshot().terminal_rows, 11U);
  EXPECT_EQ(frame.snapshot().terminal_columns, 35U);

  Canvas second(50U, 30U);
  const Canvas::Rect rect = draw(frame, second, theme, 18U);
  EXPECT_EQ(frame.snapshot().terminal_rows, 14U);
  EXPECT_EQ(frame.snapshot().terminal_columns, 45U);
  EXPECT_EQ(at(second, 4U, rect.y + 2U).character, U'k');
}

TEST(InputFrameTerminalModeTest, ExposesRepliesForTheOwningPtyProcess) {
  const Theme theme = input_theme();
  Canvas canvas(40U, 25U);
  InputFrame frame;
  command(frame, terminal::Command::ENTER_TERMINAL_MODE);
  ASSERT_EQ(frame.write_terminal("\x1b[6n"), Status::OK);
  static_cast<void>(draw(frame, canvas, theme, 15U));

  EXPECT_EQ(frame.take_terminal_responses(), "\x1b[1;1R");
  EXPECT_TRUE(frame.take_terminal_responses().empty());
}

TEST(InputFrameTerminalModeTest, TerminalSurfaceDoesNotExposeEditorSelection) {
  InputFrame frame;
  command(frame, terminal::Command::ENTER_TERMINAL_MODE);
  EXPECT_FALSE(frame.is_selectable());
  EXPECT_FALSE(frame.accepts_cursor_placement());
  std::string selected = "stale";
  EXPECT_EQ(frame.selected_text(selected), Status::FRAME_NOT_SELECTABLE);
  EXPECT_TRUE(selected.empty());
  EXPECT_EQ(frame.update_selection(
                SelectionEvent{.type = SelectionEventType::SELECT_ALL}),
            Status::FRAME_NOT_SELECTABLE);
  EXPECT_EQ(frame.place_cursor({.x = 4, .y = 2}), Status::FRAME_NOT_SELECTABLE);
}

TEST(InputFrameSelectionTest, ExtractsAndReplacesLogicalMouseSelection) {
  const Theme theme = input_theme();
  Canvas canvas(40U, 25U);
  InputFrame frame;
  type(frame, "hello world");
  static_cast<void>(draw(frame, canvas, theme));

  ASSERT_EQ(frame.update_selection(SelectionEvent{
                .type   = SelectionEventType::SELECT_AND_EXTEND,
                .anchor = {.x = 4, .y = 2},
                .extent = {.x = 8, .y = 2},
            }),
            Status::OK);
  ASSERT_EQ(frame.update_selection(SelectionEvent{
                .type   = SelectionEventType::END_SELECT_AND_EXTEND,
                .anchor = {.x = 4, .y = 2},
                .extent = {.x = 8, .y = 2},
            }),
            Status::OK);
  std::string selected;
  EXPECT_EQ(frame.selected_text(selected), Status::OK);
  EXPECT_EQ(selected, "hello");

  type(frame, "X");
  EXPECT_EQ(frame.snapshot().input_text, "X world");
  EXPECT_EQ(frame.selected_text(selected), Status::NO_SELECTION);
}

TEST(InputFrameSelectionTest,
     ScreenSelectAllUsesTheLogicalSelectionAndTargetsOnlyActiveInput) {
  multithreading::JobQueue workers;
  Screen screen(-1, -1, workers);
  auto frame = std::make_shared<InputFrame>();
  type(*frame, "normal text");
  command(*frame, terminal::Command::ENTER_COMMAND_MODE);
  type(*frame, "run\nstatus");

  ASSERT_EQ(screen.select_all("input", frame), Status::OK);
  EXPECT_EQ(screen.selection_phase(), SelectionPhase::COMPLETE);
  EXPECT_EQ(screen.selected_frame_id(), std::optional<std::string>{"input"});
  std::string selected;
  ASSERT_EQ(screen.selected_text(selected), Status::OK);
  EXPECT_EQ(selected, "run\nstatus");

  type(*frame, "help");
  EXPECT_EQ(frame->snapshot().command_text, "help");
  EXPECT_EQ(frame->snapshot().input_text, "normal text");
}

TEST(InputFrameSelectionTest, ScreenSelectAllRejectsAnEmptyInput) {
  multithreading::JobQueue workers;
  Screen screen(-1, -1, workers);
  auto frame = std::make_shared<InputFrame>();

  EXPECT_EQ(screen.select_all("input", frame), Status::NO_SELECTION);
  EXPECT_EQ(screen.selection_phase(), SelectionPhase::NONE);
}

TEST(InputFrameSelectionTest, ScreenRoutesStationaryClickToCaretPlacement) {
  const Theme theme = input_theme();
  Canvas canvas(40U, 25U);
  auto frame = std::make_shared<InputFrame>();
  type(*frame, "hello");
  const Canvas::Rect rect = draw(*frame, canvas, theme);

  multithreading::JobQueue workers;
  Screen screen(-1, -1, workers);
  ZBuffer z_buffer;
  ASSERT_EQ(z_buffer.add("input", frame), Status::OK);
  const std::map<std::string, Canvas::Rect> layouts{{"input", rect}};
  const terminal::CellPosition position{
      .x = rect.x + 5U,
      .y = rect.y + 2U,
  };
  ASSERT_EQ(screen.handle_mouse_event(
                terminal::MouseEvent{
                    .position = position,
                    .button   = terminal::MouseButton::LEFT,
                    .action   = terminal::MouseAction::PRESS,
                },
                z_buffer, layouts),
            Status::OK);
  ASSERT_EQ(screen.handle_mouse_event(
                terminal::MouseEvent{
                    .position = position,
                    .button   = terminal::MouseButton::LEFT,
                    .action   = terminal::MouseAction::RELEASE,
                },
                z_buffer, layouts),
            Status::OK);
  EXPECT_EQ(frame->snapshot().cursor, (InputCursor{.line = 0U, .column = 1U}));
  EXPECT_EQ(screen.selection_phase(), SelectionPhase::NONE);
}

TEST(InputFrameScrollTest, ScrollsVerticallyButIgnoresHorizontalWheelMotion) {
  const Theme theme = input_theme();
  Canvas canvas(40U, 25U);
  InputFrame frame;
  type(frame, "0\n1\n2\n3\n4\n5\n6\n7\n8\n9");
  const Canvas::Rect rect = draw(frame, canvas, theme);
  ASSERT_GT(frame.snapshot().scroll_row, 0U);
  const std::size_t initial = frame.snapshot().scroll_row;

  EXPECT_EQ(frame.handle_event(terminal::Event{terminal::ScrollEvent{
                .position = {.x = rect.x + 5U, .y = rect.y + 2U},
                .delta_y  = 2,
            }}),
            Status::OK);
  EXPECT_EQ(frame.snapshot().scroll_row, initial - 2U);
  static_cast<void>(draw(frame, canvas, theme));
  EXPECT_EQ(frame.snapshot().scroll_row, initial - 2U);
  EXPECT_EQ(frame.handle_event(terminal::Event{terminal::ScrollEvent{
                .position = {.x = rect.x + 5U, .y = rect.y + 2U},
                .delta_x  = 20,
            }}),
            Status::OK);
  EXPECT_EQ(frame.snapshot().scroll_row, initial - 2U);
}

TEST(InputFrameRenderingTest, MaterializesAnOverflowingWordAsANumberedLine) {
  const Theme theme = input_theme();
  Canvas canvas(40U, 25U);
  InputFrame frame;
  type(frame, "alpha " + std::string(31U, 'b'));
  const Canvas::Rect rect = draw(frame, canvas, theme, 6U);

  // The 35-column inner frame could hold 29 of the b characters after
  // "alpha ". The separating space becomes a logical newline, so the complete
  // word moves to a separately numbered row.
  EXPECT_EQ(at(canvas, 9U, rect.y + 2U).character, U' ');
  EXPECT_EQ(at(canvas, 10U, rect.y + 2U).character, U' ');
  EXPECT_EQ(at(canvas, 2U, rect.y + 3U).character, U'2');
  EXPECT_EQ(at(canvas, 4U, rect.y + 3U).character, U'b');
  EXPECT_EQ(frame.snapshot().input_text, "alpha\n" + std::string(31U, 'b'));
}

TEST(InputFrameRenderingTest, KeepsAWordThatExactlyFitsTheCurrentVisualRow) {
  const Theme theme = input_theme();
  Canvas canvas(40U, 25U);
  InputFrame frame;
  type(frame, std::string(29U, 'a') + " short tail");
  const Canvas::Rect rect = draw(frame, canvas, theme, 6U);

  // Inner width is 35. The first 29-character word, one separator, and the
  // five-character word "short" fit exactly and must remain on row one.
  EXPECT_EQ(at(canvas, 37U, rect.y + 2U).character, U'r');
  EXPECT_EQ(at(canvas, 38U, rect.y + 2U).character, U't');
  EXPECT_EQ(at(canvas, 2U, rect.y + 3U).character, U'2');
  EXPECT_EQ(at(canvas, 4U, rect.y + 3U).character, U't');
  EXPECT_EQ(frame.snapshot().input_text,
            std::string(29U, 'a') + " short\ntail");
}

TEST(InputFrameRenderingTest,
     HardWrapsAnOverlongWordWithoutAddingContinuationLineNumbers) {
  const Theme theme = input_theme();
  Canvas canvas(40U, 25U);
  InputFrame frame;
  type(frame, "prefix " + std::string(40U, 'u'));
  const Canvas::Rect rect = draw(frame, canvas, theme, 7U);

  EXPECT_EQ(frame.snapshot().input_text, "prefix\n" + std::string(40U, 'u'));
  EXPECT_EQ(at(canvas, 2U, rect.y + 2U).character, U'1');
  EXPECT_EQ(at(canvas, 2U, rect.y + 3U).character, U'2');
  EXPECT_EQ(at(canvas, 2U, rect.y + 4U).character, U' ');
  EXPECT_EQ(at(canvas, 4U, rect.y + 3U).character, U'u');
  EXPECT_EQ(at(canvas, 4U, rect.y + 4U).character, U'u');
}

TEST(InputFrameRenderingTest, DrawsStackedRegionsNumbersCursorAndNotification) {
  const Theme theme = input_theme();
  Canvas canvas(40U, 25U);
  InputFrame frame;
  type(frame, "a");
  frame.set_notification("ready");
  const Canvas::Rect rect = draw(frame, canvas, theme);

  EXPECT_EQ(at(canvas, 0U, rect.y).character, U' ');
  EXPECT_EQ(at(canvas, 0U, rect.y).background_color, 2U);
  EXPECT_EQ(at(canvas, 0U, rect.y + 1U).character, U'┌');
  EXPECT_EQ(at(canvas, 0U, rect.y + 1U).foreground_color, 1U);
  EXPECT_EQ(at(canvas, 2U, rect.y + 2U).character, U'1');
  EXPECT_EQ(at(canvas, 2U, rect.y + 2U).foreground_color, 5U);
  EXPECT_EQ(at(canvas, 3U, rect.y + 2U).character, U' ');
  EXPECT_EQ(at(canvas, 4U, rect.y + 2U).character, U'a');
  EXPECT_EQ(at(canvas, 4U, rect.y + 2U).foreground_color, 4U);
  EXPECT_EQ(at(canvas, 4U, rect.y + 2U).background_color, 3U);
  EXPECT_EQ(at(canvas, 5U, rect.y + 2U).background_color, 1U);
  EXPECT_EQ(at(canvas, 0U, rect.y + 4U).character, U'r');
}

TEST(InputFrameRenderingTest, NumbersNewlineOnlyRowsButNotUnusedRows) {
  const Theme theme = input_theme();
  Canvas canvas(40U, 25U);
  InputFrame frame;
  key(frame, terminal::NamedKey::ENTER,
      terminal::Modifiers{terminal::Modifier::SHIFT});
  const Canvas::Rect rect = draw(frame, canvas, theme, 7U);

  EXPECT_EQ(frame.snapshot().input_text, "\n");
  EXPECT_EQ(at(canvas, 2U, rect.y + 2U).character, U'1');
  EXPECT_EQ(at(canvas, 2U, rect.y + 3U).character, U'2');
  EXPECT_EQ(at(canvas, 2U, rect.y + 4U).character, U' ');
}

TEST(InputFrameRenderingTest, CommandModeMakesBorderTextAndCursorGreen) {
  const Theme theme = input_theme();
  Canvas canvas(40U, 25U);
  InputFrame frame;
  command(frame, terminal::Command::ENTER_COMMAND_MODE);
  type(frame, "x");
  const Canvas::Rect rect = draw(frame, canvas, theme);

  EXPECT_EQ(at(canvas, 0U, rect.y + 1U).foreground_color, 6U);
  EXPECT_EQ(at(canvas, 4U, rect.y + 2U).foreground_color, 6U);
  EXPECT_EQ(at(canvas, 5U, rect.y + 2U).background_color, 6U);
}

TEST(InputFrameRenderingTest,
     CommandHelpExpandsPastTheNormalEditorCapAndShowsEveryAlias) {
  const Theme theme = input_theme();
  Canvas canvas(80U, 25U);
  InputFrame frame;
  command(frame, terminal::Command::ENTER_COMMAND_MODE);
  frame.set_command_completions(
      "",
      {
          CmdCompletion{.command = "q", .description = "Quit puc."},
          CmdCompletion{.command = "quit", .description = "Quit puc."},
          CmdCompletion{.command = "exit", .description = "Quit puc."},
          CmdCompletion{.command     = "config",
                        .description = "View the configuration."},
      },
      0U);

  EXPECT_EQ(frame.preferred_height(80U, 25U), 9U);
  const Canvas::Rect rect = draw(frame, canvas, theme, 9U);
  EXPECT_EQ(at(canvas, 4U, rect.y + 2U).character, U'e');
  EXPECT_EQ(frame.snapshot().command_help.size(), 4U);
}

TEST(InputFrameRenderingTest, HundredthLogicalLineExpandsTheGutter) {
  const Theme theme = input_theme();
  Canvas canvas(40U, 25U);
  InputFrame frame;
  std::string lines;
  for (std::size_t line = 0U; line < 100U; ++line) {
    if (line != 0U) {
      lines.push_back('\n');
    }
    lines.push_back('x');
  }
  type(frame, std::move(lines));
  const Canvas::Rect rect = draw(frame, canvas, theme);

  EXPECT_EQ(at(canvas, 1U, rect.y + 2U).character, U'1');
  EXPECT_EQ(at(canvas, 2U, rect.y + 2U).character, U'0');
  EXPECT_EQ(at(canvas, 3U, rect.y + 2U).character, U'0');
  EXPECT_EQ(at(canvas, 4U, rect.y + 2U).character, U' ');
  EXPECT_EQ(at(canvas, 5U, rect.y + 2U).character, U'x');
}

}  // namespace
}  // namespace puc::tui
