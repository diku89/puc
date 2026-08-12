/**
 * @file theme.cpp
 * @brief Semantic Theme palette storage and color-role resolution.
 */

#include "puc-cli/tui/rendering/theme.hpp"

#include "utils/logger/logger.hpp"

/** @cond TUI_LOGGER_MODULE */
LOGGER_MODULE("TUI Theme");
/** @endcond */

namespace puc {
namespace tui {

void Theme::load_colors(const Colors& colors) {
  colors_ = colors;
  Logger<DEBUG> << "Loaded terminal UI theme";
}

Theme::Colors Theme::get_colors() const { return colors_; }

uint32_t Theme::get_color(ColorTypes color_type) const noexcept {
  switch (color_type) {
    case ColorTypes::PRIMARY:
      return colors_.primary;
    case ColorTypes::SECONDARY:
      return colors_.secondary;
    case ColorTypes::TERTIARY:
      return colors_.tertiary;
    case ColorTypes::HIGHLIGHT_BACKGROUND:
      return colors_.highlight_background;
    case ColorTypes::HIGHLIGHT_TEXT:
      return colors_.highlight_text;
    case ColorTypes::TEXT:
      return colors_.text;
    case ColorTypes::TEXT_SECONDARY:
      return colors_.text_secondary;
    case ColorTypes::TEXT_TERTIARY:
      return colors_.text_tertiary;
    case ColorTypes::TEXT_MUTED:
      return colors_.text_muted;
    case ColorTypes::TEXT_DISABLED:
      return colors_.text_disabled;
    case ColorTypes::TEXT_ERROR:
      return colors_.text_error;
    case ColorTypes::TEXT_WARNING:
      return colors_.text_warning;
    case ColorTypes::TEXT_ALERT:
      return colors_.text_alert;
    case ColorTypes::TEXT_SUCCESS:
      return colors_.text_success;
    case ColorTypes::TEXT_INFO:
      return colors_.text_info;
    case ColorTypes::TEXT_LINK:
      return colors_.text_link;
    case ColorTypes::TEXT_EMPHASIS:
      return colors_.text_emphasis;
    case ColorTypes::TEXT_CODE:
      return colors_.text_code;
    case ColorTypes::ALERT_TEXT_PRIMARY:
      return colors_.alert_text_primary;
    case ColorTypes::ALERT_TEXT_SECONDARY:
      return colors_.alert_text_secondary;
    case ColorTypes::DIFF_ADDED_TEXT_PRIMARY:
      return colors_.diff_added_text_primary;
    case ColorTypes::DIFF_ADDED_TEXT_SECONDARY:
      return colors_.diff_added_text_secondary;
    case ColorTypes::DIFF_ADDED_TEXT_BACKGROUND:
      return colors_.diff_added_text_background;
    case ColorTypes::DIFF_REMOVED_TEXT_PRIMARY:
      return colors_.diff_removed_text_primary;
    case ColorTypes::DIFF_REMOVED_TEXT_SECONDARY:
      return colors_.diff_removed_text_secondary;
    case ColorTypes::DIFF_REMOVED_TEXT_BACKGROUND:
      return colors_.diff_removed_text_background;
    case ColorTypes::BACKGROUND:
      return colors_.background;
  }
  return 0;
}

}  // namespace tui
}  // namespace puc
