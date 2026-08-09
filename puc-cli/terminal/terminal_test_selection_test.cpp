/**
 * @file terminal_test_selection_test.cpp
 * @brief Unit tests for the terminal conformance frame's logical selection.
 */

#include "puc-cli/terminal/terminal_test_selection.hpp"

#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "puc-cli/tui/selection.hpp"
#include "puc-cli/tui/status.hpp"

namespace puc::terminal {
namespace {

using tui::SelectionEvent;
using tui::SelectionEventType;
using tui::SelectionPosition;
using tui::Status;

/** Build one selection event without hiding its signed cell coordinates. */
constexpr SelectionEvent range_event(SelectionEventType type,
                                     SelectionPosition anchor,
                                     SelectionPosition extent) noexcept {
  return SelectionEvent{.type = type, .anchor = anchor, .extent = extent};
}

TEST(TerminalTestSelectionTest, EmptyContentCannotCreateOrExtractASelection) {
  TerminalTestSelection selection;
  std::string output = "stale";

  EXPECT_EQ(selection.update(range_event(SelectionEventType::SELECT_WORD,
                                         {.x = 0, .y = 0}, {.x = 0, .y = 0})),
            Status::NO_SELECTION);
  EXPECT_TRUE(selection.highlight_spans().empty());
  EXPECT_EQ(selection.selected_text(output), Status::NO_SELECTION);
  EXPECT_TRUE(output.empty());
}

TEST(TerminalTestSelectionTest,
     ForwardDragExtractsLogicalTextWithoutOriginsOrVisualPadding) {
  TerminalTestSelection selection;
  selection.set_lines({
      {.x = 3, .y = 5, .text = "alpha"},
      {.x = 4, .y = 7, .text = "beta"},
  });

  ASSERT_EQ(
      selection.update(range_event(SelectionEventType::END_SELECT_AND_EXTEND,
                                   {.x = 5, .y = 5}, {.x = 5, .y = 7})),
      Status::OK);
  EXPECT_EQ(selection.highlight_spans(),
            (std::vector<HighlightSpan>{
                {.line = 0U, .first = 2U, .last = 4U},
                {.line = 1U, .first = 0U, .last = 1U},
            }));
  std::string output;
  ASSERT_EQ(selection.selected_text(output), Status::OK);
  EXPECT_EQ(output, "pha\nbe");
}

TEST(TerminalTestSelectionTest, ReverseDragProducesTheSameReadingOrderRange) {
  TerminalTestSelection selection;
  selection.set_lines({
      {.x = 3, .y = 5, .text = "alpha"},
      {.x = 4, .y = 7, .text = "beta"},
  });

  ASSERT_EQ(selection.update(range_event(SelectionEventType::SELECT_AND_EXTEND,
                                         {.x = 5, .y = 7}, {.x = 5, .y = 5})),
            Status::OK);
  std::string output;
  ASSERT_EQ(selection.selected_text(output), Status::OK);
  EXPECT_EQ(output, "pha\nbe");
}

TEST(TerminalTestSelectionTest,
     PositionsOutsideTheFrameClampToRealFirstAndLastCharacters) {
  TerminalTestSelection selection;
  selection.set_lines({
      {.x = 10, .y = 10, .text = "first"},
      {.x = 20, .y = 20, .text = "last"},
  });

  ASSERT_EQ(selection.update(
                range_event(SelectionEventType::END_SELECT_AND_EXTEND,
                            {.x = -500, .y = -500}, {.x = 500, .y = 500})),
            Status::OK);
  std::string output;
  ASSERT_EQ(selection.selected_text(output), Status::OK);
  EXPECT_EQ(output, "first\nlast");
}

TEST(TerminalTestSelectionTest,
     SelectWordExpandsTheConformanceTokenButNotAdjacentPunctuation) {
  TerminalTestSelection selection;
  selection.set_lines(
      {{.x = 10, .y = 3, .text = "Select PUC-clipboard-42, then"}});

  ASSERT_EQ(selection.update(range_event(SelectionEventType::SELECT_WORD,
                                         {.x = 24, .y = 3}, {.x = 24, .y = 3})),
            Status::OK);
  std::string output;
  ASSERT_EQ(selection.selected_text(output), Status::OK);
  EXPECT_EQ(output, "PUC-clipboard-42");

  ASSERT_EQ(selection.update(range_event(SelectionEventType::SELECT_WORD,
                                         {.x = 33, .y = 3}, {.x = 33, .y = 3})),
            Status::OK);
  ASSERT_EQ(selection.selected_text(output), Status::OK);
  EXPECT_EQ(output, ",");
}

TEST(TerminalTestSelectionTest, SelectLineExtractsExactlyOneLogicalLine) {
  TerminalTestSelection selection;
  selection.set_lines({
      {.x = 7, .y = 2, .text = "first line"},
      {.x = 9, .y = 4, .text = "second line"},
  });

  ASSERT_EQ(
      selection.update(range_event(SelectionEventType::SELECT_LINE,
                                   {.x = 100, .y = 4}, {.x = 100, .y = 4})),
      Status::OK);
  std::string output;
  ASSERT_EQ(selection.selected_text(output), Status::OK);
  EXPECT_EQ(output, "second line");
  EXPECT_EQ(selection.highlight_spans(),
            (std::vector<HighlightSpan>{
                {.line = 1U, .first = 0U, .last = 10U},
            }));
}

TEST(TerminalTestSelectionTest, ResetClearsRangeAndExtractedOutput) {
  TerminalTestSelection selection;
  selection.set_lines({{.x = 0, .y = 0, .text = "selected"}});
  ASSERT_EQ(selection.update(range_event(SelectionEventType::SELECT_LINE,
                                         {.x = 0, .y = 0}, {.x = 0, .y = 0})),
            Status::OK);

  ASSERT_EQ(selection.update(SelectionEvent{.type = SelectionEventType::RESET}),
            Status::OK);
  EXPECT_TRUE(selection.highlight_spans().empty());
  std::string output = "stale";
  EXPECT_EQ(selection.selected_text(output), Status::NO_SELECTION);
  EXPECT_TRUE(output.empty());
}

TEST(TerminalTestSelectionTest,
     IdenticalNormalizedLinesPreserveRangeButContentChangesClearIt) {
  TerminalTestSelection selection;
  selection.set_lines({
      {.x = 1, .y = 1, .text = "stable"},
      {.x = 1, .y = 2, .text = ""},
  });
  ASSERT_EQ(selection.update(range_event(SelectionEventType::SELECT_LINE,
                                         {.x = 1, .y = 1}, {.x = 1, .y = 1})),
            Status::OK);

  selection.set_lines({{.x = 1, .y = 1, .text = "stable"}});
  std::string output;
  ASSERT_EQ(selection.selected_text(output), Status::OK);
  EXPECT_EQ(output, "stable");

  selection.set_lines({{.x = 1, .y = 1, .text = "changed"}});
  EXPECT_EQ(selection.selected_text(output), Status::NO_SELECTION);
  EXPECT_TRUE(output.empty());
}

TEST(TerminalTestSelectionTest, EmptyVisualLinesAreNotSelectableContent) {
  TerminalTestSelection selection;
  selection.set_lines({
      {.x = 0, .y = 0, .text = ""},
      {.x = 0, .y = 1, .text = "real"},
      {.x = 0, .y = 2, .text = ""},
  });

  ASSERT_EQ(
      selection.update(range_event(SelectionEventType::SELECT_LINE,
                                   {.x = 0, .y = -100}, {.x = 0, .y = -100})),
      Status::OK);
  std::string output;
  ASSERT_EQ(selection.selected_text(output), Status::OK);
  EXPECT_EQ(output, "real");
}

}  // namespace
}  // namespace puc::terminal
