/**
 * @file themes.cpp
 * @brief Property-backed semantic TUI theme loading implementation.
 */

#include "themes/themes.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

#include "properties/properties.hpp"
#include "puc-cli/tui/rendering/theme.hpp"

namespace puc::themes {
namespace {

using ColorMember = std::uint32_t tui::Theme::Colors::*;

/** One required property suffix and its destination palette field. */
struct ColorBinding {
  std::string_view name;
  ColorMember member;
};

constexpr std::array<ColorBinding, 27U> kColorBindings{{
    {"primary", &tui::Theme::Colors::primary},
    {"secondary", &tui::Theme::Colors::secondary},
    {"tertiary", &tui::Theme::Colors::tertiary},
    {"highlight_background", &tui::Theme::Colors::highlight_background},
    {"highlight_text", &tui::Theme::Colors::highlight_text},
    {"text", &tui::Theme::Colors::text},
    {"text_secondary", &tui::Theme::Colors::text_secondary},
    {"text_tertiary", &tui::Theme::Colors::text_tertiary},
    {"text_muted", &tui::Theme::Colors::text_muted},
    {"text_disabled", &tui::Theme::Colors::text_disabled},
    {"text_error", &tui::Theme::Colors::text_error},
    {"text_warning", &tui::Theme::Colors::text_warning},
    {"text_alert", &tui::Theme::Colors::text_alert},
    {"text_success", &tui::Theme::Colors::text_success},
    {"text_info", &tui::Theme::Colors::text_info},
    {"text_link", &tui::Theme::Colors::text_link},
    {"text_emphasis", &tui::Theme::Colors::text_emphasis},
    {"text_code", &tui::Theme::Colors::text_code},
    {"alert_text_primary", &tui::Theme::Colors::alert_text_primary},
    {"alert_text_secondary", &tui::Theme::Colors::alert_text_secondary},
    {"diff_added_text_primary", &tui::Theme::Colors::diff_added_text_primary},
    {"diff_added_text_secondary",
     &tui::Theme::Colors::diff_added_text_secondary},
    {"diff_added_text_background",
     &tui::Theme::Colors::diff_added_text_background},
    {"diff_removed_text_primary",
     &tui::Theme::Colors::diff_removed_text_primary},
    {"diff_removed_text_secondary",
     &tui::Theme::Colors::diff_removed_text_secondary},
    {"diff_removed_text_background",
     &tui::Theme::Colors::diff_removed_text_background},
    {"background", &tui::Theme::Colors::background},
}};

}  // namespace

Status register_default_dark(properties::Properties& properties) {
  const properties::Status status = properties.load_mutable_defaults(
      std::string{kDefaultThemeSource}, std::string{kDefaultDarkPath});
  return properties::is_ok(status) ? Status::OK : Status::PROPERTIES_ERROR;
}

Status apply(properties::Properties& properties, tui::Theme& theme) {
  tui::Theme::Colors colors{};
  for (const ColorBinding& binding : kColorBindings) {
    const std::string property_name = std::string{kDefaultThemeSource} +
                                      ".colors." + std::string{binding.name};
    properties::Property property;
    const properties::Status status = properties.get(property_name, property);
    if (status == properties::Status::NOT_FOUND) {
      return Status::MISSING_COLOR;
    }
    if (!properties::is_ok(status)) {
      return Status::PROPERTIES_ERROR;
    }
    const auto* value = std::get_if<std::int64_t>(&property.value);
    if (value == nullptr || *value < 0 || *value > 0xffffff) {
      return Status::INVALID_COLOR;
    }
    colors.*(binding.member) = static_cast<std::uint32_t>(*value);
  }
  theme.load_colors(colors);
  return Status::OK;
}

}  // namespace puc::themes
