#pragma once

/**
 * @file themes.hpp
 * @brief Property-backed semantic TUI theme loading.
 */

#include <string_view>

namespace puc::properties {
class Properties;
}

namespace puc::tui {
class Theme;
}

namespace puc::themes {

/** Properties source namespace for the active default theme. */
inline constexpr std::string_view kDefaultThemeSource = "theme";

/** Config-root-relative path of the packaged default dark theme. */
inline constexpr std::string_view kDefaultDarkPath = "themes/default-dark.toml";

/** Result of registering or applying a property-backed theme. */
enum class Status {
  OK,               /**< Registration or application succeeded. */
  PROPERTIES_ERROR, /**< The properties source could not be loaded. */
  MISSING_COLOR,    /**< A required semantic color is absent. */
  INVALID_COLOR,    /**< A color is not an integer in 0x000000..0xffffff. */
};

/** Return whether a theme operation succeeded. */
constexpr bool is_ok(Status status) noexcept { return status == Status::OK; }

/** Return stable human-readable text for a theme result. */
constexpr std::string_view status_message(Status status) noexcept {
  switch (status) {
    case Status::OK:
      return "success";
    case Status::PROPERTIES_ERROR:
      return "theme properties could not be loaded";
    case Status::MISSING_COLOR:
      return "theme is missing a required semantic color";
    case Status::INVALID_COLOR:
      return "theme color must be an integer from 0x000000 to 0xffffff";
  }
  return "unknown theme status";
}

/** Register the packaged dark palette as user-mutable defaults. */
Status register_default_dark(properties::Properties& properties);

/** Apply all current `theme.colors.*` properties to a Theme. */
Status apply(properties::Properties& properties, tui::Theme& theme);

}  // namespace puc::themes
