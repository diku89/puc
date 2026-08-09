#pragma once

/**
 * @file frame.hpp
 * @brief Abstract interface implemented by independently laid-out TUI views.
 */

#include <string>
#include <utility>

#include "puc-cli/tui/canvas.hpp"
#include "puc-cli/tui/selection.hpp"
#include "puc-cli/tui/theme.hpp"

namespace puc {
namespace tui {

/**
 * A renderable view assigned to a rectangular region by Layout.
 *
 * A Frame owns its descriptive name but not its location. Layout computes the
 * rectangle and supplies the active Canvas. Independent non-intersecting
 * frames may be invoked concurrently; implementations therefore keep mutable
 * frame-local state synchronized and strictly restrict writes to the supplied
 * rectangle. Expected rendering failures are returned as Status values.
 */
class Frame {
 public:
  /**
   * Construct a named frame.
   *
   * @param[in] name Human-readable frame name available to derived classes.
   */
  Frame(std::string name) : name_(std::move(name)) {}

  /** Destroy a frame through its abstract interface. */
  virtual ~Frame() = default;

  /**
   * Render this frame into its assigned canvas rectangle.
   *
   * The Canvas has an active transaction. A concrete Frame that needs
   * application data captures a typed state object in its constructor and
   * synchronizes that object itself. Writing outside `rect` violates the
   * parallel-rendering contract.
   *
   * @param[in] theme Active semantic color theme.
   * @param[in,out] canvas Canvas receiving cells within `rect`.
   * @param[in] rect Rectangle assigned to this frame, in canvas coordinates.
   * @return Status::OK on success, or an error status.
   */
  virtual Status draw(const Theme& theme, Canvas& canvas,
                      const Canvas::Rect& rect) = 0;

  /**
   * Return whether this Frame exposes logical text-selection operations.
   *
   * Nonselectable frames remain input barriers during hit testing: Screen does
   * not select through a frontmost decoration into a frame behind it.
   */
  virtual bool is_selectable() const noexcept;

  /**
   * Apply one semantic selection event expressed in frame-local coordinates.
   *
   * A selectable implementation owns its anchor, logical text range, click
   * granularity, and rendering synchronization. RESET removes that range.
   * Rejected operations must leave the previous range unchanged so Screen's
   * state machine remains synchronized with the Frame.
   *
   * @param[in] event Selection operation routed by Screen.
   * @return Status::OK on success, or an error without partial mutation.
   */
  virtual Status update_selection(const SelectionEvent& event);

  /**
   * Extract selected logical text without performing clipboard I/O.
   *
   * Implementations omit visual padding, decorations, and text owned by other
   * frames. Newline and wrapping policy therefore remain application logic.
   *
   * @param[out] output Receives selected UTF-8 bytes; cleared on failure.
   * @return Status::OK on success, Status::NO_SELECTION when no logical range
   *         exists, or Status::FRAME_NOT_SELECTABLE for the base behavior.
   */
  virtual Status selected_text(std::string& output) const;

 protected:
  /** Human-readable name supplied by the concrete frame. */
  std::string name_;
};

}  // namespace tui
}  // namespace puc
