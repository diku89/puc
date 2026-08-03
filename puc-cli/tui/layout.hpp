#pragma once

/**
 * @file layout.hpp
 * @brief Constraint-based frame placement and ordered TUI composition.
 */

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "puc-cli/tui/canvas.hpp"
#include "puc-cli/tui/state.hpp"
#include "puc-cli/tui/theme.hpp"
#include "puc-cli/tui/zbuf.hpp"

namespace puc {
namespace tui {

/**
 * Builds, solves, and draws constraint-based arrangements of Frame objects.
 *
 * A LayoutDescription owns a ZBuffer, which in turn shares ownership of its
 * frames and defines their back-to-front drawing order. Each frame id maps to
 * an independent set of size, margin, and placement constraints.
 *
 * Percentage values are fractions of the corresponding full-screen dimension
 * in the inclusive range `[0.0, 1.0]`. Maximums cap a frame's resolved size;
 * absent maximums allow it to fill the bounds remaining after margins.
 * Minimums contribute to `compute_minimum_dimensions()` and let an application
 * select a small-screen fallback. Screen bounds and maximums still win during
 * actual resolution when the current terminal is too small.
 *
 * Aspect ratios are visual `width : height` ratios rather than raw
 * column-to-row ratios. CellDimensions compensates for character cells that are
 * usually taller than they are wide. Named edge anchors introduce dependencies
 * between frames; the solver resolves them recursively and rejects cycles.
 */
class Layout {
 public:
  /**
   * Kinds of size, bounds, and placement constraints.
   *
   * Anchor offsets are measured inward from the named screen edge. Horizontal
   * and vertical center offsets move the centered frame right and down,
   * respectively. A zero center offset centers the frame itself.
   */
  enum class ConstraintType {
    ASPECT_RATIO,      /**< Preserve a visual width-to-height ratio. */
    MIN_WIDTH,         /**< Minimum required terminal columns for the frame. */
    MIN_HEIGHT,        /**< Minimum required terminal rows for the frame. */
    MAX_WIDTH,         /**< Cap the resolved frame width. */
    MAX_HEIGHT,        /**< Cap the resolved frame height. */
    LEFT_MARGIN,       /**< Inset the frame's available left bound. */
    RIGHT_MARGIN,      /**< Inset the frame's available right bound. */
    TOP_MARGIN,        /**< Inset the frame's available top bound. */
    BOTTOM_MARGIN,     /**< Inset the frame's available bottom bound. */
    LEFT_ANCHOR,       /**< Place relative to a left edge. */
    RIGHT_ANCHOR,      /**< Place relative to a right edge. */
    TOP_ANCHOR,        /**< Place relative to a top edge. */
    BOTTOM_ANCHOR,     /**< Place relative to a bottom edge. */
    HORIZONTAL_CENTER, /**< Center frame horizontally, plus its offset. */
    VERTICAL_CENTER,   /**< Center frame vertically, plus its offset. */
  };

  /**
   * Value representation used by a Constraint.
   */
  enum class Unit {
    CHARACTERS, /**< Absolute number of columns or rows. */
    PERCENT,    /**< Fraction of the corresponding screen dimension. */
    RATIO,      /**< Visual AspectRatio value. */
    NAME,       /**< Id of a frame whose corresponding edge is aligned. */
  };

  /**
   * Positive visual width-to-height ratio for a frame.
   *
   * `{4, 3}` describes a landscape box four visual units wide and three
   * visual units tall. It does not request four columns by three rows.
   */
  struct AspectRatio {
    int32_t width;  /**< Relative visual width. */
    int32_t height; /**< Relative visual height. */
  };

  /**
   * One typed constraint and its unit-specific value.
   *
   * `CHARACTERS` stores `size_t`, `PERCENT` stores `float`, `RATIO` stores
   * AspectRatio, and `NAME` stores `std::string`. Invalid type/unit/value
   * combinations are rejected when the constraint is added or solved.
   */
  struct Constraint {
    ConstraintType type; /**< Behavior controlled by this constraint. */
    Unit unit;           /**< Representation selected in `value`. */
    std::variant<float, size_t, AspectRatio, std::string>
        value; /**< Value of the constraint. */
  };

  /**
   * Declarative frame ownership, ordering, and constraints for one layout.
   *
   * Use Layout mutation methods to preserve the invariant that every frame id
   * appears exactly once in `z_buffer` and has one entry in `constraints`.
   */
  struct LayoutDescription {
    std::string layout_name; /**< Human-readable layout name used in logs. */
    ZBuffer z_buffer;        /**< Frames in back-to-front drawing order. */
    std::map<std::string, std::vector<Constraint>>
        constraints; /**< Constraints indexed by frame id. */
  };

  /**
   * Solved rectangles for every frame in a LayoutDescription.
   */
  struct AbsoluteLayout {
    std::map<std::string, Canvas::Rect>
        frame_layouts; /**< Frame ids mapped to half-open canvas rectangles. */
  };

  /**
   * Construct a percentage-valued constraint.
   *
   * @param[in] type    The type of constraint.
   * @param[in] percent The percentage value for the constraint.
   *
   * @return An unvalidated constraint storing `percent` as Unit::PERCENT.
   */
  static Constraint make_percentage_constraint(ConstraintType type,
                                               float percent);

  /**
   * Construct a character-cell-valued constraint.
   *
   * @param[in] type       The type of constraint.
   * @param[in] characters The character count for the constraint.
   *
   * @return An unvalidated constraint storing `characters` as Unit::CHARACTERS.
   */
  static Constraint make_character_constraint(ConstraintType type,
                                              size_t characters);

  /**
   * Construct a visual aspect-ratio constraint.
   *
   * @param[in] type Constraint type; only ASPECT_RATIO is valid when added.
   * @param[in] width Positive relative visual width.
   * @param[in] height Positive relative visual height.
   *
   * @return An unvalidated constraint storing `{width, height}` as Unit::RATIO.
   */
  static Constraint make_ratio_constraint(ConstraintType type, int32_t width,
                                          int32_t height);

  /**
   * Construct a named-frame edge-anchor constraint.
   *
   * @param[in] type Edge-anchor type to align with the referenced frame.
   * @param[in] name Frame id to reference during resolution.
   *
   * @return An unvalidated constraint storing `name` as Unit::NAME.
   */
  static Constraint make_name_constraint(ConstraintType type,
                                         const std::string& name);

  /**
   * Allocate an empty layout description.
   *
   * @param[in] layout_name The name of the layout.
   *
   * @return Shared ownership of an empty description named `layout_name`.
   */
  std::shared_ptr<LayoutDescription> make_layout_description(
      const std::string& layout_name) const;

  /**
   * Add a frame at the front of a layout's Z-buffer.
   *
   * @param[in] layout_description The layout description to add the frame to.
   * @param[in] frame_id Unique, non-empty id used by constraints.
   * @param[in] frame Frame implementation for which ownership is shared.
   *
   * @return Status::OK on success, Status::INVALID_ARGUMENT for a null layout,
   *         empty id, or null frame, or Status::DUPLICATE_FRAME_ID.
   */
  Status add_frame_to_layout_description(
      const std::shared_ptr<LayoutDescription>& layout_description,
      const std::string& frame_id, std::shared_ptr<Frame> frame);

  /**
   * Add a constraint to a frame in a layout description.
   *
   * @param[in] layout_description The layout description containing the frame.
   * @param[in] frame_id Id of the frame to constrain.
   * @param[in] constraint Valid constraint to append.
   *
   * A frame can have at most one constraint of each type and at most one
   * horizontal and one vertical placement constraint.
   *
   * @return Status::OK on success; Status::INVALID_ARGUMENT,
   *         Status::FRAME_NOT_FOUND, Status::INVALID_PERCENTAGE,
   *         Status::INVALID_RATIO, or Status::INVALID_CONSTRAINT otherwise.
   */
  Status add_constraint_to_frame(
      const std::shared_ptr<LayoutDescription>& layout_description,
      const std::string& frame_id, const Constraint& constraint);

  /**
   * Resolve every frame to an absolute canvas rectangle.
   *
   * @param[in] layout_description The layout description to compute the
   * absolute layout from.
   * @param[in] screen_width Screen width in columns.
   * @param[in] screen_height Screen height in rows.
   * @param[in] cell_dimensions Positive relative physical cell dimensions.
   * @param[out] absolute_layout Receives the computed frame rectangles.
   *
   * The output map is cleared before resolution and remains empty on error.
   *
   * @return Status::OK on success, or the first description, dimension,
   *         constraint, dependency, or overflow error encountered.
   */
  Status compute_absolute_layout(
      const std::shared_ptr<LayoutDescription>& layout_description,
      size_t screen_width, size_t screen_height,
      const CellDimensions& cell_dimensions,
      AbsoluteLayout& absolute_layout) const;

  /**
   * Compute the smallest terminal dimensions required by the description.
   *
   * Character margins, anchor offsets, aspect ratios, and percentage maximums
   * are included. Percentage minimums do not introduce an absolute minimum.
   *
   * @param[in] layout_description The layout to inspect.
   * @param[in] cell_dimensions Positive relative physical cell dimensions.
   * @param[out] minimum_width Receives the minimum terminal columns.
   * @param[out] minimum_height Receives the minimum terminal rows.
   *
   * Outputs are reset to zero before validation.
   *
   * @return Status::OK on success, or the first description, dimension,
   *         constraint, dependency, or overflow error encountered.
   */
  Status compute_minimum_dimensions(
      const std::shared_ptr<LayoutDescription>& layout_description,
      const CellDimensions& cell_dimensions, size_t& minimum_width,
      size_t& minimum_height) const;

  /**
   * Compute and draw a layout in Z-buffer order.
   *
   * The first Z-buffer entry is drawn first and the last is drawn last. Frames
   * whose `needs_update()` returns false are skipped. The canvas must have an
   * active frame transaction and should match `state`'s terminal dimensions.
   *
   * @param[in] layout_description The layout and frames to draw.
   * @param[in] state              The current terminal UI state.
   * @param[in] theme              The active color theme.
   * @param[in,out] canvas         The canvas receiving frame output.
   *
   * @return Status::OK on success, or the first frame error.
   */
  Status draw(const std::shared_ptr<LayoutDescription>& layout_description,
              const State& state, const Theme& theme, Canvas& canvas) const;
};

}  // namespace tui
}  // namespace puc
