#pragma once

/**
 * @file selection.hpp
 * @brief Semantic text-selection events and their screen-owned state machine.
 */

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "puc-cli/tui/rendering/status.hpp"

namespace puc::tui {

class Frame;

/** Lifecycle phase of the one selection owned by a Screen. */
enum class SelectionPhase {
  NONE,        /**< No frame currently contains selected text. */
  IN_PROGRESS, /**< A captured pointer gesture is extending a selection. */
  COMPLETE,    /**< A stable selection remains available for copying. */
};

/** Semantic operations that a selectable Frame can apply to its content. */
enum class SelectionEventType {
  SELECT_AND_EXTEND,     /**< Begin or update a character-range drag. */
  END_SELECT_AND_EXTEND, /**< Apply the release position and finish a drag. */
  SELECT_WORD,           /**< Replace the range with the word at `extent`. */
  SELECT_LINE,           /**< Replace the range with the line at `extent`. */
  SELECT_ALL,            /**< Replace the range with all target-frame text. */
  RESET,                 /**< Remove the frame's current selection. */
};

/**
 * A signed position in one Frame's local terminal-cell coordinate system.
 *
 * Coordinates outside the frame remain meaningful during pointer capture.
 * For example, a negative y coordinate asks a vertically scrollable frame to
 * extend above its visible content rather than retargeting another frame.
 */
struct SelectionPosition {
  std::int64_t x = 0; /**< Column relative to the frame's left edge. */
  std::int64_t y = 0; /**< Row relative to the frame's top edge. */

  /** Compare both frame-local coordinates. */
  constexpr bool operator==(const SelectionPosition&) const noexcept = default;
};

/** One semantic selection update dispatched by Screen to a Frame. */
struct SelectionEvent {
  SelectionEventType type =
      SelectionEventType::RESET; /**< Operation represented by this event. */
  SelectionPosition anchor; /**< Original press position for drag operations. */
  SelectionPosition extent; /**< Latest pointer or click position. */

  /** Compare the operation and both local positions. */
  constexpr bool operator==(const SelectionEvent&) const noexcept = default;
};

/**
 * Maintain the selection lifecycle while a Screen performs input routing.
 *
 * The state machine owns the selected Frame strongly so a later RESET always
 * reaches the same object even if its layout is replaced. It owns no logical
 * range: the Frame interprets SelectionEvent positions against its own text,
 * updates its typed application state, and renders the resulting highlight.
 *
 * SelectionStateMachine is not internally synchronized. Screen serializes its
 * terminal-input calls before invoking this object. Frame callbacks must make
 * their own selection data safe against concurrent rendering.
 */
class SelectionStateMachine {
 public:
  /** Construct an empty state machine in SelectionPhase::NONE. */
  SelectionStateMachine() = default;

  SelectionStateMachine(const SelectionStateMachine&)            = delete;
  SelectionStateMachine& operator=(const SelectionStateMachine&) = delete;
  SelectionStateMachine(SelectionStateMachine&&)                 = delete;
  SelectionStateMachine& operator=(SelectionStateMachine&&)      = delete;

  /** Release retained Frame ownership without invoking application code. */
  ~SelectionStateMachine() = default;

  /**
   * Apply one semantic operation to a selectable frame.
   *
   * `SELECT_AND_EXTEND` enters or remains in IN_PROGRESS.
   * `END_SELECT_AND_EXTEND` requires the same in-progress frame and enters
   * COMPLETE. `SELECT_WORD`, `SELECT_LINE`, and `SELECT_ALL` reset any existing
   * selection and enter COMPLETE from NONE. A `SELECT_AND_EXTEND` received in
   * COMPLETE similarly resets the old selection before beginning anew.
   * RESET ignores the supplied target and delegates to `reset()`.
   *
   * Frame handlers must reject an event without partially mutating their
   * selection so that a failed operation leaves this state machine coherent.
   *
   * @param[in] frame_id Stable id of the target in its current Z-buffer.
   * @param[in] frame Shared ownership of the target Frame.
   * @param[in] event Frame-local semantic operation to apply.
   * @return Status::OK on success, or a validation, transition, or Frame error.
   */
  Status apply(std::string_view frame_id, const std::shared_ptr<Frame>& frame,
               const SelectionEvent& event);

  /**
   * Remove the active selection and return to NONE.
   *
   * RESET is idempotent. When a Frame is active, the phase and retained frame
   * change only after that Frame accepts its RESET event.
   */
  Status reset();

  /** Return the current lifecycle phase. */
  SelectionPhase phase() const noexcept { return phase_; }

  /** Return the active frame id, or no value while the phase is NONE. */
  std::optional<std::string> active_frame_id() const;

  /**
   * Ask the completed Frame to extract its selected UTF-8 text.
   *
   * Clipboard transport deliberately remains outside Frame. A caller can pass
   * the returned bytes to Screen's terminal-session boundary later.
   */
  Status selected_text(std::string& output) const;

 private:
  /** Test whether a target is the frame retained by the active phase. */
  bool is_active_target(std::string_view frame_id,
                        const std::shared_ptr<Frame>& frame) const noexcept;

  SelectionPhase phase_ = SelectionPhase::NONE; /**< Current lifecycle phase. */
  std::string active_frame_id_; /**< Stable id of the active selected frame. */
  std::shared_ptr<Frame>
      active_frame_; /**< Target retained until successful RESET. */
};

}  // namespace puc::tui
