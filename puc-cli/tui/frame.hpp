#pragma once

/**
 * @file frame.hpp
 * @brief Abstract interface implemented by independently laid-out TUI views.
 */

#include <string>
#include <utility>

#include "puc-cli/tui/canvas.hpp"
#include "puc-cli/tui/state.hpp"
#include "puc-cli/tui/theme.hpp"

namespace puc {
namespace tui {

/**
 * A renderable view assigned to a rectangular region by Layout.
 *
 * A Frame owns its descriptive name but not its location. Layout computes the
 * rectangle for each draw and invokes frames in ZBuffer order. Implementations
 * should restrict writes to the supplied rectangle and return a Status instead
 * of using exceptions for expected rendering failures.
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
   * The canvas must already have an active frame transaction. Implementations
   * may read shared UI state and semantic colors but should only mutate cells
   * inside `rect`.
   *
   * @param[in] state Current terminal and application state.
   * @param[in] theme Active semantic color theme.
   * @param[in,out] canvas Canvas receiving this frame's cells.
   * @param[in] rect Rectangle assigned to this frame, in canvas coordinates.
   * @return Status::OK on success, or an error status.
   */
  virtual Status draw(const State& state, const Theme& theme, Canvas& canvas,
                      const Canvas::Rect& rect) = 0;

  /**
   * Report whether Layout should invoke `draw()` for the next frame.
   *
   * Because Canvas preserves the previously published image at the start of a
   * transaction, returning `false` leaves cells untouched by this frame. Frames
   * drawn later in Z-order may still overwrite those cells.
   *
   * @return `true` when the frame needs to be redrawn; otherwise `false`.
   */
  virtual bool needs_update() const = 0;

 protected:
  /** Human-readable name supplied by the concrete frame. */
  std::string name_;
};

}  // namespace tui
}  // namespace puc
