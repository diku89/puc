/**
 * @file text_editor_utils_test.cpp
 * @brief Focused tests for reusable text-buffer mechanics.
 */

#include "puc-cli/tui/text_input/text_editor_utils.hpp"

#include <cstddef>
#include <string>

#include "gtest/gtest.h"
#include "puc-cli/tui/terminal/event.hpp"

namespace puc::tui {
namespace {

/** Apply one named key press to an editor. */
void key(TextEditor& editor, terminal::NamedKey named,
         terminal::Modifiers modifiers = {}) {
  editor.handle_key(terminal::KeyEvent{
      .key       = named,
      .modifiers = modifiers,
  });
}

TEST(TextEditorUtf8Test, RoundTripsScalarsAndReplacesMalformedInput) {
  const std::string source = "h\xc3\xa9\xf0\x9f\x98\x80";
  EXPECT_EQ(text_editor::encode_utf8(text_editor::decode_utf8(source)), source);
  EXPECT_EQ(text_editor::decode_utf8("\xf0\x28\x8c\x28"),
            (std::u32string{U'\ufffd', U'(', U'\ufffd', U'('}));

  std::string encoded;
  text_editor::append_utf8(static_cast<char32_t>(0x110000U), encoded);
  EXPECT_EQ(encoded, "\xef\xbf\xbd");
  EXPECT_TRUE(text_editor::is_word_character(U'_'));
  EXPECT_TRUE(text_editor::is_word_character(U'\xe9'));
  EXPECT_FALSE(text_editor::is_word_character(U' '));
}

TEST(TextEditorEditingTest, InsertsDeletesAndJoinsUnicodeLogicalLines) {
  TextEditor editor;
  editor.insert_utf8("h\xc3\xa9\xf0\x9f\x98\x80");
  EXPECT_EQ(editor.snapshot().cursor, (TextCursor{.line = 0U, .column = 3U}));
  key(editor, terminal::NamedKey::BACKSPACE);
  EXPECT_EQ(editor.text(), "h\xc3\xa9");

  key(editor, terminal::NamedKey::ENTER,
      terminal::Modifiers{terminal::Modifier::SHIFT});
  editor.insert_utf8("x");
  EXPECT_EQ(editor.text(), "h\xc3\xa9\nx");
  key(editor, terminal::NamedKey::HOME);
  key(editor, terminal::NamedKey::BACKSPACE);
  EXPECT_EQ(editor.text(), "h\xc3\xa9x");
  key(editor, terminal::NamedKey::DELETE_KEY);
  EXPECT_EQ(editor.text(), "h\xc3\xa9");
}

TEST(TextEditorEditingTest, ReservesPlainEnterAndExpandsTabs) {
  TextEditor editor(TextEditorOptions{.tab_width = 4U});
  editor.insert_utf8("a");
  key(editor, terminal::NamedKey::ENTER);
  EXPECT_EQ(editor.text(), "a");
  key(editor, terminal::NamedKey::TAB);
  EXPECT_EQ(editor.text(), "a   ");
  key(editor, terminal::NamedKey::ENTER,
      terminal::Modifiers{terminal::Modifier::SHIFT});
  EXPECT_EQ(editor.text(), "a   \n");

  TextEditor multiline(TextEditorOptions{.reserve_plain_enter = false});
  multiline.insert_utf8("a");
  key(multiline, terminal::NamedKey::ENTER);
  EXPECT_EQ(multiline.text(), "a\n");
}

TEST(TextEditorWrappingTest, MaterializesOnlyWordBoundaryWraps) {
  TextEditor editor(TextEditorOptions{.fallback_width = 10U});
  editor.insert_utf8("alpha beta");
  EXPECT_EQ(editor.text(), "alpha beta");
  editor.insert_utf8("x");
  EXPECT_EQ(editor.text(), "alpha\nbetax");
  EXPECT_EQ(editor.logical_line_count(), 2U);

  TextEditor long_token(TextEditorOptions{.fallback_width = 10U});
  long_token.insert_utf8("prefix " + std::string(15U, 'u'));
  EXPECT_EQ(long_token.text(), "prefix\n" + std::string(15U, 'u'));
  EXPECT_EQ(long_token.logical_line_count(), 2U);
  EXPECT_EQ(long_token.visual_row_count(10U), 3U);
}

TEST(TextEditorNavigationTest, MovesByWordsRowsBuffersAndPages) {
  TextEditor editor;
  editor.insert_utf8("one two\nabc\ndef");
  editor.set_viewport(20U, 2U);

  editor.apply_command(terminal::Command::MOVE_BUFFER_START);
  editor.apply_command(terminal::Command::MOVE_WORD_RIGHT);
  EXPECT_EQ(editor.snapshot().cursor, (TextCursor{.line = 0U, .column = 4U}));
  editor.apply_command(terminal::Command::MOVE_ROW_END);
  EXPECT_EQ(editor.snapshot().cursor, (TextCursor{.line = 0U, .column = 7U}));
  editor.apply_command(terminal::Command::MOVE_WORD_LEFT);
  EXPECT_EQ(editor.snapshot().cursor, (TextCursor{.line = 0U, .column = 4U}));
  editor.apply_command(terminal::Command::MOVE_BUFFER_END);
  EXPECT_EQ(editor.snapshot().cursor, (TextCursor{.line = 2U, .column = 3U}));
  editor.apply_command(terminal::Command::MOVE_PAGE_UP);
  EXPECT_EQ(editor.snapshot().cursor.line, 0U);
  editor.apply_command(terminal::Command::MOVE_PAGE_DOWN);
  EXPECT_EQ(editor.snapshot().cursor.line, 2U);
}

TEST(TextEditorSelectionTest, ShiftNavigationAndTypingReplaceOneSharedRange) {
  TextEditor editor;
  editor.insert_utf8("abc");
  key(editor, terminal::NamedKey::LEFT,
      terminal::Modifiers{terminal::Modifier::SHIFT});
  key(editor, terminal::NamedKey::LEFT,
      terminal::Modifiers{terminal::Modifier::SHIFT});

  std::string selected;
  ASSERT_EQ(editor.selected_text(selected), Status::OK);
  EXPECT_EQ(selected, "bc");
  editor.insert_utf8("X");
  EXPECT_EQ(editor.text(), "aX");
  EXPECT_EQ(editor.selected_text(selected), Status::NO_SELECTION);
}

TEST(TextEditorSelectionTest, SupportsWordLineDragAndSelectAllSemantics) {
  TextEditor editor;
  editor.insert_utf8("hello world\nsecond");
  editor.apply_command(terminal::Command::MOVE_BUFFER_START);
  editor.set_viewport(20U, 2U);

  ASSERT_EQ(editor.update_selection(SelectionEvent{
                .type   = SelectionEventType::SELECT_WORD,
                .extent = {.x = 7, .y = 0},
            }),
            Status::OK);
  std::string selected;
  ASSERT_EQ(editor.selected_text(selected), Status::OK);
  EXPECT_EQ(selected, "world");

  ASSERT_EQ(editor.update_selection(SelectionEvent{
                .type   = SelectionEventType::SELECT_LINE,
                .extent = {.x = 2, .y = 1},
            }),
            Status::OK);
  ASSERT_EQ(editor.selected_text(selected), Status::OK);
  EXPECT_EQ(selected, "second");

  ASSERT_EQ(editor.update_selection(
                SelectionEvent{.type = SelectionEventType::SELECT_ALL}),
            Status::OK);
  ASSERT_EQ(editor.selected_text(selected), Status::OK);
  EXPECT_EQ(selected, "hello world\nsecond");

  editor.clear();
  EXPECT_EQ(editor.update_selection(
                SelectionEvent{.type = SelectionEventType::SELECT_ALL}),
            Status::NO_SELECTION);
}

TEST(TextEditorPasteTest, StreamsUtf8NormalizesCrLfAndRollsBackCancel) {
  TextEditor editor;
  editor.insert_utf8("before ");
  EXPECT_EQ(editor.handle_paste(
                terminal::PasteEvent{.phase = terminal::PastePhase::DATA}),
            Status::INVALID_ARGUMENT);
  ASSERT_EQ(editor.handle_paste(
                terminal::PasteEvent{.phase = terminal::PastePhase::BEGIN}),
            Status::OK);
  EXPECT_EQ(editor.handle_paste(
                terminal::PasteEvent{.phase = terminal::PastePhase::BEGIN}),
            Status::INVALID_ARGUMENT);
  ASSERT_EQ(editor.handle_paste(terminal::PasteEvent{
                .phase = terminal::PastePhase::DATA, .data = "A\xf0\x9f"}),
            Status::OK);
  ASSERT_EQ(editor.handle_paste(terminal::PasteEvent{
                .phase = terminal::PastePhase::DATA,
                .data  = "\x98\x80\r\nB",
            }),
            Status::OK);
  ASSERT_EQ(editor.handle_paste(
                terminal::PasteEvent{.phase = terminal::PastePhase::END}),
            Status::OK);
  EXPECT_EQ(editor.text(), "before A\xf0\x9f\x98\x80\nB");

  ASSERT_EQ(editor.handle_paste(
                terminal::PasteEvent{.phase = terminal::PastePhase::BEGIN}),
            Status::OK);
  ASSERT_EQ(editor.handle_paste(terminal::PasteEvent{
                .phase = terminal::PastePhase::DATA, .data = " discarded"}),
            Status::OK);
  ASSERT_EQ(editor.handle_paste(
                terminal::PasteEvent{.phase = terminal::PastePhase::CANCEL}),
            Status::OK);
  EXPECT_EQ(editor.text(), "before A\xf0\x9f\x98\x80\nB");
}

TEST(TextEditorViewportTest, RetainsManualScrollAcrossUnchangedLayouts) {
  TextEditor editor;
  editor.insert_utf8("0\n1\n2\n3\n4\n5");
  editor.set_viewport(10U, 2U);
  EXPECT_EQ(editor.snapshot().scroll_row, 4U);
  editor.scroll_vertical(2);
  EXPECT_EQ(editor.snapshot().scroll_row, 2U);
  editor.set_viewport(10U, 2U);
  EXPECT_EQ(editor.snapshot().scroll_row, 2U);

  const TextEditorRenderState rendered = editor.render_state();
  ASSERT_EQ(rendered.rows.size(), 2U);
  EXPECT_EQ(rendered.rows[0].logical_line, 2U);
  EXPECT_EQ(rendered.rows[1].logical_line, 3U);
}

TEST(TextEditorViewportTest, MapsVisibleCellsToCaretAndDragSelections) {
  TextEditor editor;
  editor.insert_utf8("abc\ndef");
  editor.apply_command(terminal::Command::MOVE_BUFFER_START);
  editor.set_viewport(10U, 2U);
  ASSERT_EQ(editor.place_cursor({.x = 2, .y = 1}), Status::OK);
  EXPECT_EQ(editor.snapshot().cursor, (TextCursor{.line = 1U, .column = 2U}));

  ASSERT_EQ(editor.update_selection(SelectionEvent{
                .type   = SelectionEventType::SELECT_AND_EXTEND,
                .anchor = {.x = 1, .y = 0},
                .extent = {.x = 1, .y = 1},
            }),
            Status::OK);
  std::string selected;
  ASSERT_EQ(editor.selected_text(selected), Status::OK);
  EXPECT_EQ(selected, "bc\nde");
}

}  // namespace
}  // namespace puc::tui
