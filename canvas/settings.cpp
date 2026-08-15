/** @file settings.cpp @brief Canvas TOML loading and path resolution. */

#include "canvas/settings.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <variant>

#include "properties/properties.hpp"

namespace puc::canvas {

bool load_settings(properties::Properties& properties, Settings& settings) {
  constexpr std::string_view kSource = "canvas";
  constexpr std::string_view kPath   = "canvas.toml";
  const properties::Status loaded    = properties.load_mutable_defaults(
      std::string{kSource}, std::filesystem::path{kPath});
  if (!properties::is_ok(loaded)) return false;
  properties::Property property;
  if (!properties::is_ok(properties.get("canvas.database.path", property))) {
    return false;
  }
  const auto* configured = std::get_if<std::string>(&property.value);
  if (configured == nullptr || configured->empty()) return false;

  std::filesystem::path resolved;
  constexpr std::string_view kHomePrefix = "$HOME/";
  if (configured->starts_with(kHomePrefix)) {
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') return false;
    resolved =
        std::filesystem::path{home} / configured->substr(kHomePrefix.size());
  } else {
    resolved = *configured;
  }
  if (!resolved.is_absolute()) return false;
  settings.database_path = resolved.lexically_normal();
  return true;
}

}  // namespace puc::canvas
