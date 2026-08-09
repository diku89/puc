/**
 * @file selection.cpp
 * @brief Screen-owned semantic text-selection state transitions.
 */

#include "puc-cli/tui/selection.hpp"

#include <memory>
#include <string>
#include <string_view>

#include "puc-cli/tui/frame.hpp"
#include "utils/logger/logger.hpp"

/** @cond TUI_LOGGER_MODULE */
LOGGER_MODULE("TUI Selection");
/** @endcond */

namespace puc::tui {

bool SelectionStateMachine::is_active_target(
    std::string_view frame_id,
    const std::shared_ptr<Frame>& frame) const noexcept {
  return active_frame_ != nullptr && active_frame_ == frame &&
         active_frame_id_ == frame_id;
}

Status SelectionStateMachine::reset() {
  if (phase_ == SelectionPhase::NONE) {
    active_frame_id_.clear();
    active_frame_.reset();
    return Status::OK;
  }
  if (active_frame_ == nullptr) {
    Logger<ERROR> << "Selection phase retained no active frame";
    return Status::INVALID_SELECTION_TRANSITION;
  }

  const SelectionEvent reset_event{.type = SelectionEventType::RESET};
  const Status status = active_frame_->update_selection(reset_event);
  if (!is_ok(status)) {
    Logger<ERROR> << "Frame " << active_frame_id_
                  << " rejected selection reset: " << status_message(status);
    return status;
  }

  Logger<DEBUG> << "Reset selection in frame " << active_frame_id_;
  phase_ = SelectionPhase::NONE;
  active_frame_id_.clear();
  active_frame_.reset();
  return Status::OK;
}

Status SelectionStateMachine::apply(std::string_view frame_id,
                                    const std::shared_ptr<Frame>& frame,
                                    const SelectionEvent& event) {
  if (event.type == SelectionEventType::RESET) {
    return reset();
  }
  if (frame_id.empty() || frame == nullptr) {
    return Status::INVALID_ARGUMENT;
  }
  if (!frame->is_selectable()) {
    return Status::FRAME_NOT_SELECTABLE;
  }

  switch (event.type) {
    case SelectionEventType::SELECT_AND_EXTEND:
      if (phase_ == SelectionPhase::COMPLETE) {
        const Status status = reset();
        if (!is_ok(status)) {
          return status;
        }
      } else if (phase_ == SelectionPhase::IN_PROGRESS &&
                 !is_active_target(frame_id, frame)) {
        return Status::INVALID_SELECTION_TRANSITION;
      }
      break;

    case SelectionEventType::END_SELECT_AND_EXTEND:
      if (phase_ != SelectionPhase::IN_PROGRESS ||
          !is_active_target(frame_id, frame)) {
        return Status::INVALID_SELECTION_TRANSITION;
      }
      break;

    case SelectionEventType::SELECT_WORD:
    case SelectionEventType::SELECT_LINE:
      if (phase_ != SelectionPhase::NONE) {
        const Status status = reset();
        if (!is_ok(status)) {
          return status;
        }
      }
      break;

    case SelectionEventType::RESET:
      break;
  }

  const Status status = frame->update_selection(event);
  if (!is_ok(status)) {
    Logger<ERROR> << "Frame " << frame_id
                  << " rejected selection event: " << status_message(status);
    return status;
  }

  active_frame_id_ = frame_id;
  active_frame_    = frame;
  phase_           = event.type == SelectionEventType::SELECT_AND_EXTEND
                         ? SelectionPhase::IN_PROGRESS
                         : SelectionPhase::COMPLETE;
  return Status::OK;
}

std::optional<std::string> SelectionStateMachine::active_frame_id() const {
  if (phase_ == SelectionPhase::NONE || active_frame_ == nullptr) {
    return std::nullopt;
  }
  return active_frame_id_;
}

Status SelectionStateMachine::selected_text(std::string& output) const {
  output.clear();
  if (phase_ != SelectionPhase::COMPLETE || active_frame_ == nullptr) {
    return Status::NO_SELECTION;
  }
  return active_frame_->selected_text(output);
}

}  // namespace puc::tui
