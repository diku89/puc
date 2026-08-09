/**
 * @file selection_test.cpp
 * @brief Unit tests for selection transitions, hit testing, and pointer
 * capture.
 */

#include "puc-cli/tui/selection.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "puc-cli/terminal/event.hpp"
#include "puc-cli/tui/canvas.hpp"
#include "puc-cli/tui/frame.hpp"
#include "puc-cli/tui/screen.hpp"
#include "puc-cli/tui/theme.hpp"
#include "puc-cli/tui/zbuf.hpp"
#include "utils/multithreading/job_queue.hpp"

namespace puc::tui {
namespace {

/** Selectable or blocking Frame double with observable semantic operations. */
class SelectionFrame final : public Frame {
 public:
  explicit SelectionFrame(std::string name, bool selectable = true)
      : Frame(std::move(name)), selectable_(selectable) {}

  Status draw(const Theme&, Canvas&, const Canvas::Rect&) override {
    return Status::OK;
  }

  bool is_selectable() const noexcept override { return selectable_; }

  Status update_selection(const SelectionEvent& event) override {
    if (rejected_event_.has_value() && *rejected_event_ == event.type) {
      return rejection_status_;
    }
    events.push_back(event);
    has_selection_ = event.type != SelectionEventType::RESET;
    return Status::OK;
  }

  Status selected_text(std::string& output) const override {
    output.clear();
    if (!has_selection_) {
      return Status::NO_SELECTION;
    }
    output = text;
    return Status::OK;
  }

  void reject(SelectionEventType type,
              Status status = Status::INVALID_ARGUMENT) {
    rejected_event_   = type;
    rejection_status_ = status;
  }

  void accept_all() { rejected_event_.reset(); }

  std::vector<SelectionEvent> events;
  std::string text = "selected text";

 private:
  bool selectable_    = true;
  bool has_selection_ = false;
  std::optional<SelectionEventType> rejected_event_;
  Status rejection_status_ = Status::INVALID_ARGUMENT;
};

/** Construct a semantic event with concise, distinct positions. */
SelectionEvent selection_event(SelectionEventType type,
                               SelectionPosition anchor = {.x = 1, .y = 2},
                               SelectionPosition extent = {.x = 3, .y = 4}) {
  return SelectionEvent{.type = type, .anchor = anchor, .extent = extent};
}

/** Construct one normalized terminal mouse event. */
terminal::MouseEvent mouse(
    terminal::MouseAction action, std::size_t x, std::size_t y,
    terminal::MouseButton button  = terminal::MouseButton::LEFT,
    terminal::Modifiers modifiers = {}) {
  return terminal::MouseEvent{
      .position  = {.x = x, .y = y},
      .button    = button,
      .action    = action,
      .modifiers = modifiers,
  };
}

/** Send one stationary left click. */
void click(Screen& screen, const ZBuffer& z_buffer,
           const std::map<std::string, Canvas::Rect>& layouts, std::size_t x,
           std::size_t y, terminal::Modifiers modifiers = {}) {
  ASSERT_EQ(
      screen.handle_mouse_event(mouse(terminal::MouseAction::PRESS, x, y,
                                      terminal::MouseButton::LEFT, modifiers),
                                z_buffer, layouts),
      Status::OK);
  ASSERT_EQ(
      screen.handle_mouse_event(mouse(terminal::MouseAction::RELEASE, x, y,
                                      terminal::MouseButton::LEFT, modifiers),
                                z_buffer, layouts),
      Status::OK);
}

TEST(FrameSelectionTest, BaseFrameIsAnExplicitNonselectableBarrier) {
  class PlainFrame final : public Frame {
   public:
    PlainFrame() : Frame("plain") {}
    Status draw(const Theme&, Canvas&, const Canvas::Rect&) override {
      return Status::OK;
    }
  } frame;
  std::string output = "stale";

  EXPECT_FALSE(frame.is_selectable());
  EXPECT_EQ(
      frame.update_selection(selection_event(SelectionEventType::SELECT_WORD)),
      Status::FRAME_NOT_SELECTABLE);
  EXPECT_EQ(frame.selected_text(output), Status::FRAME_NOT_SELECTABLE);
  EXPECT_TRUE(output.empty());
}

TEST(SelectionStateMachineTest, DragTransitionsAndSelfLoopPreserveTarget) {
  SelectionStateMachine machine;
  const auto frame = std::make_shared<SelectionFrame>("document");
  const SelectionEvent begin =
      selection_event(SelectionEventType::SELECT_AND_EXTEND);
  const SelectionEvent extend =
      selection_event(SelectionEventType::SELECT_AND_EXTEND, {.x = 1, .y = 2},
                      {.x = 8, .y = 9});
  const SelectionEvent end =
      selection_event(SelectionEventType::END_SELECT_AND_EXTEND,
                      {.x = 1, .y = 2}, {.x = 10, .y = 11});

  EXPECT_EQ(machine.phase(), SelectionPhase::NONE);
  ASSERT_EQ(machine.apply("document", frame, begin), Status::OK);
  EXPECT_EQ(machine.phase(), SelectionPhase::IN_PROGRESS);
  EXPECT_EQ(machine.active_frame_id(), std::optional<std::string>{"document"});
  ASSERT_EQ(machine.apply("document", frame, extend), Status::OK);
  EXPECT_EQ(machine.phase(), SelectionPhase::IN_PROGRESS);
  ASSERT_EQ(machine.apply("document", frame, end), Status::OK);
  EXPECT_EQ(machine.phase(), SelectionPhase::COMPLETE);
  EXPECT_EQ(frame->events, (std::vector<SelectionEvent>{begin, extend, end}));
}

TEST(SelectionStateMachineTest, ResetLeavesNoneFromEveryActivePhase) {
  const auto frame = std::make_shared<SelectionFrame>("document");

  SelectionStateMachine in_progress;
  ASSERT_EQ(
      in_progress.apply("document", frame,
                        selection_event(SelectionEventType::SELECT_AND_EXTEND)),
      Status::OK);
  ASSERT_EQ(in_progress.reset(), Status::OK);
  EXPECT_EQ(in_progress.phase(), SelectionPhase::NONE);
  EXPECT_EQ(in_progress.active_frame_id(), std::nullopt);
  ASSERT_EQ(frame->events.back().type, SelectionEventType::RESET);

  SelectionStateMachine complete;
  ASSERT_EQ(complete.apply("document", frame,
                           selection_event(SelectionEventType::SELECT_WORD)),
            Status::OK);
  ASSERT_EQ(complete.reset(), Status::OK);
  EXPECT_EQ(complete.phase(), SelectionPhase::NONE);
  EXPECT_EQ(complete.reset(), Status::OK);
}

TEST(SelectionStateMachineTest, WordAndLineResetACompletedSelectionFirst) {
  SelectionStateMachine machine;
  const auto frame          = std::make_shared<SelectionFrame>("document");
  const SelectionEvent word = selection_event(SelectionEventType::SELECT_WORD);
  const SelectionEvent line = selection_event(SelectionEventType::SELECT_LINE);

  ASSERT_EQ(machine.apply("document", frame, word), Status::OK);
  EXPECT_EQ(machine.phase(), SelectionPhase::COMPLETE);
  ASSERT_EQ(machine.apply("document", frame, line), Status::OK);
  EXPECT_EQ(machine.phase(), SelectionPhase::COMPLETE);
  ASSERT_EQ(frame->events.size(), 3U);
  EXPECT_EQ(frame->events[0], word);
  EXPECT_EQ(frame->events[1].type, SelectionEventType::RESET);
  EXPECT_EQ(frame->events[2], line);
}

TEST(SelectionStateMachineTest, NewDragResetsACompletedSelectionFirst) {
  SelectionStateMachine machine;
  const auto frame          = std::make_shared<SelectionFrame>("document");
  const SelectionEvent word = selection_event(SelectionEventType::SELECT_WORD);
  const SelectionEvent drag =
      selection_event(SelectionEventType::SELECT_AND_EXTEND);

  ASSERT_EQ(machine.apply("document", frame, word), Status::OK);
  ASSERT_EQ(machine.apply("document", frame, drag), Status::OK);
  EXPECT_EQ(machine.phase(), SelectionPhase::IN_PROGRESS);
  ASSERT_EQ(frame->events.size(), 3U);
  EXPECT_EQ(frame->events[0], word);
  EXPECT_EQ(frame->events[1].type, SelectionEventType::RESET);
  EXPECT_EQ(frame->events[2], drag);
}

TEST(SelectionStateMachineTest, ReplacementOnAnotherFrameResetsTheOldFrame) {
  SelectionStateMachine machine;
  const auto first  = std::make_shared<SelectionFrame>("first");
  const auto second = std::make_shared<SelectionFrame>("second");

  ASSERT_EQ(machine.apply("first", first,
                          selection_event(SelectionEventType::SELECT_WORD)),
            Status::OK);
  ASSERT_EQ(machine.apply("second", second,
                          selection_event(SelectionEventType::SELECT_LINE)),
            Status::OK);

  ASSERT_EQ(first->events.size(), 2U);
  EXPECT_EQ(first->events.back().type, SelectionEventType::RESET);
  ASSERT_EQ(second->events.size(), 1U);
  EXPECT_EQ(second->events.front().type, SelectionEventType::SELECT_LINE);
  EXPECT_EQ(machine.active_frame_id(), std::optional<std::string>{"second"});
}

TEST(SelectionStateMachineTest, InvalidEndAndCrossFrameDragAreRejected) {
  SelectionStateMachine machine;
  const auto first  = std::make_shared<SelectionFrame>("first");
  const auto second = std::make_shared<SelectionFrame>("second");
  const SelectionEvent end =
      selection_event(SelectionEventType::END_SELECT_AND_EXTEND);

  EXPECT_EQ(machine.apply("first", first, end),
            Status::INVALID_SELECTION_TRANSITION);
  ASSERT_EQ(
      machine.apply("first", first,
                    selection_event(SelectionEventType::SELECT_AND_EXTEND)),
      Status::OK);
  EXPECT_EQ(
      machine.apply("second", second,
                    selection_event(SelectionEventType::SELECT_AND_EXTEND)),
      Status::INVALID_SELECTION_TRANSITION);
  EXPECT_EQ(machine.phase(), SelectionPhase::IN_PROGRESS);
  EXPECT_EQ(machine.active_frame_id(), std::optional<std::string>{"first"});
}

TEST(SelectionStateMachineTest, ValidationAndFrameErrorsDoNotAdvanceState) {
  SelectionStateMachine machine;
  const auto selectable = std::make_shared<SelectionFrame>("selectable");
  const auto blocker    = std::make_shared<SelectionFrame>("blocker", false);

  EXPECT_EQ(machine.apply("", selectable,
                          selection_event(SelectionEventType::SELECT_WORD)),
            Status::INVALID_ARGUMENT);
  EXPECT_EQ(machine.apply("missing", nullptr,
                          selection_event(SelectionEventType::SELECT_WORD)),
            Status::INVALID_ARGUMENT);
  EXPECT_EQ(machine.apply("blocker", blocker,
                          selection_event(SelectionEventType::SELECT_WORD)),
            Status::FRAME_NOT_SELECTABLE);

  selectable->reject(SelectionEventType::SELECT_WORD);
  EXPECT_EQ(machine.apply("selectable", selectable,
                          selection_event(SelectionEventType::SELECT_WORD)),
            Status::INVALID_ARGUMENT);
  EXPECT_EQ(machine.phase(), SelectionPhase::NONE);
}

TEST(SelectionStateMachineTest, FailedResetRetainsTheCompletedSelection) {
  SelectionStateMachine machine;
  const auto frame = std::make_shared<SelectionFrame>("document");
  ASSERT_EQ(machine.apply("document", frame,
                          selection_event(SelectionEventType::SELECT_WORD)),
            Status::OK);
  frame->reject(SelectionEventType::RESET);

  EXPECT_EQ(machine.reset(), Status::INVALID_ARGUMENT);
  EXPECT_EQ(machine.phase(), SelectionPhase::COMPLETE);
  EXPECT_EQ(machine.active_frame_id(), std::optional<std::string>{"document"});
}

TEST(SelectionStateMachineTest, ExtractsTextOnlyFromACompletedSelection) {
  SelectionStateMachine machine;
  const auto frame   = std::make_shared<SelectionFrame>("document");
  frame->text        = "ನಮಸ್ಕಾರ";
  std::string output = "stale";

  EXPECT_EQ(machine.selected_text(output), Status::NO_SELECTION);
  EXPECT_TRUE(output.empty());
  ASSERT_EQ(
      machine.apply("document", frame,
                    selection_event(SelectionEventType::SELECT_AND_EXTEND)),
      Status::OK);
  EXPECT_EQ(machine.selected_text(output), Status::NO_SELECTION);
  ASSERT_EQ(
      machine.apply("document", frame,
                    selection_event(SelectionEventType::END_SELECT_AND_EXTEND)),
      Status::OK);
  EXPECT_EQ(machine.selected_text(output), Status::OK);
  EXPECT_EQ(output, "ನಮಸ್ಕಾರ");
}

TEST(ScreenSelectionTest, CapturedDragUsesSignedCoordinatesOnOriginalFrame) {
  multithreading::JobQueue workers;
  Screen screen(-1, -1, workers);
  ZBuffer z_buffer;
  const auto document = std::make_shared<SelectionFrame>("document");
  const auto overlay  = std::make_shared<SelectionFrame>("overlay");
  ASSERT_EQ(z_buffer.add("document", document), Status::OK);
  ASSERT_EQ(z_buffer.add("overlay", overlay), Status::OK);
  const std::map<std::string, Canvas::Rect> layouts{
      {"document", {.x = 10U, .y = 10U, .width = 5U, .height = 3U}},
      {"overlay", {.x = 18U, .y = 18U, .width = 4U, .height = 4U}},
  };
  ASSERT_EQ(
      screen.handle_mouse_event(mouse(terminal::MouseAction::PRESS, 11U, 11U),
                                z_buffer, layouts),
      Status::OK);
  ASSERT_EQ(screen.handle_mouse_event(
                mouse(terminal::MouseAction::DRAG, 9U, 9U), z_buffer, layouts),
            Status::OK);
  EXPECT_EQ(screen.selection_phase(), SelectionPhase::IN_PROGRESS);
  ASSERT_EQ(
      screen.handle_mouse_event(mouse(terminal::MouseAction::RELEASE, 20U, 20U),
                                z_buffer, layouts),
      Status::OK);

  EXPECT_EQ(screen.selection_phase(), SelectionPhase::COMPLETE);
  EXPECT_EQ(screen.selected_frame_id(), std::optional<std::string>{"document"});
  ASSERT_EQ(document->events.size(), 2U);
  EXPECT_EQ(document->events[0],
            (SelectionEvent{
                .type   = SelectionEventType::SELECT_AND_EXTEND,
                .anchor = {.x = 1, .y = 1},
                .extent = {.x = -1, .y = -1},
            }));
  EXPECT_EQ(document->events[1],
            (SelectionEvent{
                .type   = SelectionEventType::END_SELECT_AND_EXTEND,
                .anchor = {.x = 1, .y = 1},
                .extent = {.x = 10, .y = 10},
            }));
  EXPECT_TRUE(overlay->events.empty());
}

TEST(ScreenSelectionTest, ReleaseMovementSynthesizesACompleteDrag) {
  multithreading::JobQueue workers;
  Screen screen(-1, -1, workers);
  ZBuffer z_buffer;
  const auto document = std::make_shared<SelectionFrame>("document");
  ASSERT_EQ(z_buffer.add("document", document), Status::OK);
  const std::map<std::string, Canvas::Rect> layouts{
      {"document", {.x = 2U, .y = 3U, .width = 10U, .height = 5U}},
  };
  ASSERT_EQ(screen.handle_mouse_event(
                mouse(terminal::MouseAction::PRESS, 3U, 4U), z_buffer, layouts),
            Status::OK);
  ASSERT_EQ(
      screen.handle_mouse_event(mouse(terminal::MouseAction::RELEASE, 6U, 5U),
                                z_buffer, layouts),
      Status::OK);

  ASSERT_EQ(document->events.size(), 2U);
  EXPECT_EQ(document->events.front().type,
            SelectionEventType::SELECT_AND_EXTEND);
  EXPECT_EQ(document->events.back().type,
            SelectionEventType::END_SELECT_AND_EXTEND);
  EXPECT_EQ(screen.selection_phase(), SelectionPhase::COMPLETE);
}

TEST(ScreenSelectionTest, DoubleClickSelectsWordAndTripleClickSelectsLine) {
  multithreading::JobQueue workers;
  Screen screen(-1, -1, workers);
  ZBuffer z_buffer;
  const auto document = std::make_shared<SelectionFrame>("document");
  ASSERT_EQ(z_buffer.add("document", document), Status::OK);
  const std::map<std::string, Canvas::Rect> layouts{
      {"document", {.x = 4U, .y = 5U, .width = 10U, .height = 5U}},
  };
  click(screen, z_buffer, layouts, 7U, 6U);
  EXPECT_EQ(screen.selection_phase(), SelectionPhase::NONE);
  click(screen, z_buffer, layouts, 7U, 6U);
  EXPECT_EQ(screen.selection_phase(), SelectionPhase::COMPLETE);
  ASSERT_EQ(document->events.size(), 1U);
  EXPECT_EQ(document->events[0], (SelectionEvent{
                                     .type   = SelectionEventType::SELECT_WORD,
                                     .anchor = {.x = 3, .y = 1},
                                     .extent = {.x = 3, .y = 1},
                                 }));

  click(screen, z_buffer, layouts, 7U, 6U);
  EXPECT_EQ(screen.selection_phase(), SelectionPhase::COMPLETE);
  ASSERT_EQ(document->events.size(), 3U);
  EXPECT_EQ(document->events[1].type, SelectionEventType::RESET);
  EXPECT_EQ(document->events[2].type, SelectionEventType::SELECT_LINE);
}

TEST(ScreenSelectionTest, SingleClickAfterSelectionClearsIt) {
  multithreading::JobQueue workers;
  Screen screen(-1, -1, workers);
  ZBuffer z_buffer;
  const auto document = std::make_shared<SelectionFrame>("document");
  ASSERT_EQ(z_buffer.add("document", document), Status::OK);
  const std::map<std::string, Canvas::Rect> layouts{
      {"document", {.x = 0U, .y = 0U, .width = 20U, .height = 10U}},
  };
  click(screen, z_buffer, layouts, 2U, 2U);
  click(screen, z_buffer, layouts, 2U, 2U);
  ASSERT_EQ(screen.selection_phase(), SelectionPhase::COMPLETE);

  const auto timeout = screen.pending_selection_timeout();
  ASSERT_TRUE(timeout.has_value());
  ASSERT_EQ(screen.handle_selection_timeout(*timeout), Status::OK);
  EXPECT_EQ(screen.selection_phase(), SelectionPhase::COMPLETE);
  EXPECT_EQ(screen.selected_frame_id(), std::optional<std::string>{"document"});
  EXPECT_EQ(screen.pending_selection_timeout(), std::nullopt);
  click(screen, z_buffer, layouts, 8U, 2U);
  EXPECT_EQ(screen.selection_phase(), SelectionPhase::NONE);
  EXPECT_EQ(screen.selected_frame_id(), std::nullopt);
  EXPECT_EQ(document->events.back().type, SelectionEventType::RESET);
}

TEST(ScreenSelectionTest, StaleClickTimeoutCannotBreakANewerClickChain) {
  multithreading::JobQueue workers;
  Screen screen(-1, -1, workers);
  ZBuffer z_buffer;
  const auto document = std::make_shared<SelectionFrame>("document");
  ASSERT_EQ(z_buffer.add("document", document), Status::OK);
  const std::map<std::string, Canvas::Rect> layouts{
      {"document", {.x = 0U, .y = 0U, .width = 20U, .height = 10U}},
  };

  click(screen, z_buffer, layouts, 2U, 2U);
  const auto first_timeout = screen.pending_selection_timeout();
  ASSERT_TRUE(first_timeout.has_value());
  click(screen, z_buffer, layouts, 2U, 2U);
  const auto second_timeout = screen.pending_selection_timeout();
  ASSERT_TRUE(second_timeout.has_value());
  EXPECT_NE(first_timeout, second_timeout);

  ASSERT_EQ(screen.handle_selection_timeout(*first_timeout), Status::OK);
  EXPECT_EQ(screen.pending_selection_timeout(), second_timeout);
  click(screen, z_buffer, layouts, 2U, 2U);

  ASSERT_EQ(document->events.size(), 3U);
  EXPECT_EQ(document->events[0].type, SelectionEventType::SELECT_WORD);
  EXPECT_EQ(document->events[1].type, SelectionEventType::RESET);
  EXPECT_EQ(document->events[2].type, SelectionEventType::SELECT_LINE);
}

TEST(ScreenSelectionTest, FrontmostNonselectableFrameBlocksSelectionThroughIt) {
  multithreading::JobQueue workers;
  Screen screen(-1, -1, workers);
  ZBuffer z_buffer;
  const auto document   = std::make_shared<SelectionFrame>("document");
  const auto decoration = std::make_shared<SelectionFrame>("decoration", false);
  ASSERT_EQ(z_buffer.add("document", document), Status::OK);
  ASSERT_EQ(z_buffer.add("decoration", decoration), Status::OK);
  const std::map<std::string, Canvas::Rect> layouts{
      {"document", {.x = 0U, .y = 0U, .width = 20U, .height = 10U}},
      {"decoration", {.x = 5U, .y = 0U, .width = 5U, .height = 5U}},
  };
  click(screen, z_buffer, layouts, 2U, 2U);
  click(screen, z_buffer, layouts, 2U, 2U);
  ASSERT_EQ(screen.selection_phase(), SelectionPhase::COMPLETE);

  click(screen, z_buffer, layouts, 6U, 2U);
  EXPECT_EQ(screen.selection_phase(), SelectionPhase::NONE);
  EXPECT_EQ(document->events.back().type, SelectionEventType::RESET);
  EXPECT_TRUE(decoration->events.empty());
}

TEST(ScreenSelectionTest, FrontmostSelectableFrameWinsOverOverlappingFrame) {
  multithreading::JobQueue workers;
  Screen screen(-1, -1, workers);
  ZBuffer z_buffer;
  const auto back  = std::make_shared<SelectionFrame>("back");
  const auto front = std::make_shared<SelectionFrame>("front");
  ASSERT_EQ(z_buffer.add("back", back), Status::OK);
  ASSERT_EQ(z_buffer.add("front", front), Status::OK);
  const std::map<std::string, Canvas::Rect> layouts{
      {"back", {.x = 0U, .y = 0U, .width = 10U, .height = 10U}},
      {"front", {.x = 2U, .y = 2U, .width = 5U, .height = 5U}},
  };
  ASSERT_EQ(screen.handle_mouse_event(
                mouse(terminal::MouseAction::PRESS, 3U, 3U), z_buffer, layouts),
            Status::OK);
  ASSERT_EQ(screen.handle_mouse_event(
                mouse(terminal::MouseAction::DRAG, 4U, 4U), z_buffer, layouts),
            Status::OK);
  EXPECT_TRUE(back->events.empty());
  ASSERT_EQ(front->events.size(), 1U);
  EXPECT_EQ(screen.selected_frame_id(), std::optional<std::string>{"front"});
}

TEST(ScreenSelectionTest, DifferentCellsModifiersAndSlowClicksDoNotCombine) {
  multithreading::JobQueue workers;
  Screen screen(-1, -1, workers);
  ZBuffer z_buffer;
  const auto document = std::make_shared<SelectionFrame>("document");
  ASSERT_EQ(z_buffer.add("document", document), Status::OK);
  const std::map<std::string, Canvas::Rect> layouts{
      {"document", {.x = 0U, .y = 0U, .width = 20U, .height = 10U}},
  };
  click(screen, z_buffer, layouts, 1U, 1U);
  click(screen, z_buffer, layouts, 2U, 1U);
  const auto timeout = screen.pending_selection_timeout();
  ASSERT_TRUE(timeout.has_value());
  ASSERT_EQ(screen.handle_selection_timeout(*timeout), Status::OK);
  click(screen, z_buffer, layouts, 2U, 1U);
  click(screen, z_buffer, layouts, 2U, 1U,
        terminal::Modifiers{terminal::Modifier::SHIFT});

  EXPECT_TRUE(document->events.empty());
  EXPECT_EQ(screen.selection_phase(), SelectionPhase::NONE);
}

TEST(ScreenSelectionTest, CompletedTextAndExplicitResetUseTheSelectedFrame) {
  multithreading::JobQueue workers;
  Screen screen(-1, -1, workers);
  ZBuffer z_buffer;
  const auto document = std::make_shared<SelectionFrame>("document");
  document->text      = "logical text\nwithout padding";
  ASSERT_EQ(z_buffer.add("document", document), Status::OK);
  const std::map<std::string, Canvas::Rect> layouts{
      {"document", {.x = 0U, .y = 0U, .width = 20U, .height = 10U}},
  };
  click(screen, z_buffer, layouts, 1U, 1U);
  click(screen, z_buffer, layouts, 1U, 1U);

  std::string output;
  EXPECT_EQ(screen.selected_text(output), Status::OK);
  EXPECT_EQ(output, document->text);
  EXPECT_EQ(screen.reset_selection(), Status::OK);
  EXPECT_EQ(screen.selection_phase(), SelectionPhase::NONE);
  EXPECT_EQ(screen.selected_text(output), Status::NO_SELECTION);
  EXPECT_TRUE(output.empty());
}

TEST(ScreenSelectionTest, IrrelevantButtonsMotionAndReleaseAreNoOps) {
  multithreading::JobQueue workers;
  Screen screen(-1, -1, workers);
  ZBuffer z_buffer;
  const auto document = std::make_shared<SelectionFrame>("document");
  ASSERT_EQ(z_buffer.add("document", document), Status::OK);
  const std::map<std::string, Canvas::Rect> layouts{
      {"document", {.x = 0U, .y = 0U, .width = 20U, .height = 10U}},
  };
  EXPECT_EQ(screen.handle_mouse_event(mouse(terminal::MouseAction::PRESS, 1U,
                                            1U, terminal::MouseButton::RIGHT),
                                      z_buffer, layouts),
            Status::OK);
  EXPECT_EQ(screen.handle_mouse_event(mouse(terminal::MouseAction::MOVE, 2U, 2U,
                                            terminal::MouseButton::NONE),
                                      z_buffer, layouts),
            Status::OK);
  EXPECT_EQ(
      screen.handle_mouse_event(mouse(terminal::MouseAction::RELEASE, 2U, 2U),
                                z_buffer, layouts),
      Status::OK);
  EXPECT_TRUE(document->events.empty());
  EXPECT_EQ(screen.selection_phase(), SelectionPhase::NONE);
}

}  // namespace
}  // namespace puc::tui
