#pragma once

/**
 * @file bounding_frame.hpp
 * @brief Reusable colored box, margins, padding, and size constraints.
 */

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

#include "puc-cli/tui/frame.hpp"

namespace puc::tui {

/** Four independent terminal-cell insets. */
struct FrameMargins {
  std::size_t top    = 0U; /**< Rows above the bounded box or child. */
  std::size_t bottom = 0U; /**< Rows below the bounded box or child. */
  std::size_t left   = 0U; /**< Columns left of the bounded box or child. */
  std::size_t right  = 0U; /**< Columns right of the bounded box or child. */

  /** Compare every inset. */
  constexpr bool operator==(const FrameMargins&) const noexcept = default;
};

/** Static total-rectangle constraints enforced before a child is drawn. */
struct FrameSizeConstraints {
  std::size_t minimum_width  = 1U; /**< Smallest accepted total width. */
  std::size_t minimum_height = 1U; /**< Smallest accepted total height. */
  std::optional<std::size_t> maximum_width;  /**< Optional largest width. */
  std::optional<std::size_t> maximum_height; /**< Optional largest height. */
  bool require_full_canvas_width = false; /**< Require x=0 and canvas width. */

  /** Compare all constraint values. */
  constexpr bool operator==(const FrameSizeConstraints&) const noexcept =
      default;
};

/** Complete geometry and color policy for BoundingFrame. */
struct BoundingFrameConfiguration {
  FrameMargins outer_margins; /**< Space between assigned rect and box. */
  FrameMargins inner_margins; /**< Space between box interior and child. */
  std::optional<Theme::ColorTypes> outside_box_color =
      Theme::ColorTypes::BACKGROUND; /**< Optional fill. */
  Theme::ColorTypes inside_color =
      Theme::ColorTypes::SECONDARY; /**< Box-interior background. */
  std::optional<Theme::ColorTypes> border_color =
      Theme::ColorTypes::TEXT;           /**< Nullopt disables border glyphs. */
  FrameSizeConstraints size_constraints; /**< Total assigned-rect limits. */
};

/**
 * Decorates one child Frame with margins, an optional box, and color fills.
 *
 * Outer margins remain outside the border and can be painted with
 * `outside_box_color` or left untouched. Inner margins remain inside the box
 * and use `inside_color`. Selection and caret positions are translated through
 * both layers before being delegated to the child.
 */
class BoundingFrame final : public Frame {
 public:
  /** Construct a configured bounding decorator around a required child. */
  BoundingFrame(std::string name, std::shared_ptr<Frame> child,
                BoundingFrameConfiguration configuration = {});

  BoundingFrame(const BoundingFrame&)            = delete;
  BoundingFrame& operator=(const BoundingFrame&) = delete;
  BoundingFrame(BoundingFrame&&)                 = delete;
  BoundingFrame& operator=(BoundingFrame&&)      = delete;

  /** Destroy configuration state and shared child ownership. */
  ~BoundingFrame() override;

  /** Atomically replace geometry, colors, and size constraints. */
  void set_configuration(BoundingFrameConfiguration configuration);

  /** Return a copy of the active configuration. */
  BoundingFrameConfiguration configuration() const;

  /** Replace only the size constraints, preserving geometry and colors. */
  void set_size_constraints(FrameSizeConstraints constraints);

  /** Compute the absolute bordered-box rectangle, if it can exist. */
  std::optional<Canvas::Rect> box_rect(const Canvas::Rect& rect) const;

  /** Compute the absolute child rectangle, if every inset can fit. */
  std::optional<Canvas::Rect> content_rect(const Canvas::Rect& rect) const;

  /** Draw the colored bounding regions and then the child. */
  Status draw(const Theme& theme, Canvas& canvas,
              const Canvas::Rect& rect) override;

  /** Return the child Frame's selection capability. */
  bool is_selectable() const noexcept override;

  /** Translate frame-local coordinates and delegate selection. */
  Status update_selection(const SelectionEvent& event) override;

  /** Delegate selected-text extraction to the child. */
  Status selected_text(std::string& output) const override;

  /** Return the child Frame's caret-placement capability. */
  bool accepts_cursor_placement() const noexcept override;

  /** Translate a frame-local cell and delegate caret placement. */
  Status place_cursor(SelectionPosition position) override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_; /**< Hidden synchronized decorator state. */
};

}  // namespace puc::tui
