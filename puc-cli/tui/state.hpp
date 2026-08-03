#pragma once

/**
 * @file state.hpp
 * @brief Shared per-frame terminal metrics and application rendering state.
 */

#include <cstddef>

namespace puc {
namespace tui {

/**
 * Relative physical dimensions of one terminal character cell.
 *
 * The values form a ratio rather than an absolute pixel size. For example,
 * `{1, 2}` describes a cell twice as tall as it is wide. Layout uses this
 * ratio to translate visual aspect ratios into integer columns and rows.
 */
struct CellDimensions {
  size_t width  = 1; /**< Relative physical width of a cell. */
  size_t height = 2; /**< Relative physical height of a cell. */

  /** Compare both relative dimensions. */
  bool operator==(const CellDimensions&) const = default;
};

/**
 * Immutable-per-draw state shared with every Frame.
 *
 * The application refreshes this structure before asking Layout to draw.
 * Frames receive it by const reference, which gives every frame one consistent
 * view of terminal dimensions and sampled application metrics.
 */
struct State {
  size_t screen_width  = 0; /**< Current terminal width in characters. */
  size_t screen_height = 0; /**< Current terminal height in characters. */
  CellDimensions cell_dimensions{}; /**< Physical character-cell proportions. */
  double frames_per_second = 0;     /**< Recently measured frame rate. */
};

}  // namespace tui
}  // namespace puc
