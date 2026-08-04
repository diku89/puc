#pragma once

/**
 * @file state.hpp
 * @brief Shared per-frame terminal metrics and application rendering state.
 */

#include <cstddef>

namespace puc {
namespace tui {

/**
 * Immutable-per-draw state shared with every Frame.
 *
 * The application refreshes this structure before asking Layout to draw.
 * Frames receive it by const reference, which gives every frame one consistent
 * view of terminal dimensions and sampled application metrics.
 */
struct State {
  size_t screen_width      = 0; /**< Current terminal width in characters. */
  size_t screen_height     = 0; /**< Current terminal height in characters. */
  double frames_per_second = 0; /**< Recently measured frame rate. */
};

}  // namespace tui
}  // namespace puc
