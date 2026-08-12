/**
 * @file theme_subsystem.cpp
 * @brief Property-backed application theme lifecycle implementation.
 */

#include "themes/theme_subsystem.hpp"

#include <memory>

#include "properties/properties.hpp"
#include "properties/properties_subsystem.hpp"
#include "puc-cli/tui/rendering/theme.hpp"
#include "themes/themes.hpp"

namespace puc::app {

ThemeSubsystem::ThemeSubsystem()
    : AppSubsystem("theme", subsystem_dependencies<PropertiesSubsystem>()) {}

ThemeSubsystem::~ThemeSubsystem() = default;

Status ThemeSubsystem::initialize(AppState& app) {
  PropertiesSubsystem* properties = app.get_subsystem<PropertiesSubsystem>();
  if (properties == nullptr || properties->properties() == nullptr) {
    return Status::SUBSYSTEM_FAILURE;
  }
  theme_status_ = themes::register_default_dark(*properties->properties());
  if (!themes::is_ok(theme_status_)) {
    return Status::SUBSYSTEM_FAILURE;
  }
  theme_        = std::make_unique<tui::Theme>();
  theme_status_ = themes::apply(*properties->properties(), *theme_);
  return themes::is_ok(theme_status_) ? Status::OK : Status::SUBSYSTEM_FAILURE;
}

Status ThemeSubsystem::start(AppState& app) {
  PropertiesSubsystem* properties = app.get_subsystem<PropertiesSubsystem>();
  if (theme_ == nullptr || properties == nullptr ||
      properties->properties() == nullptr) {
    return Status::SUBSYSTEM_FAILURE;
  }
  theme_status_ = themes::apply(*properties->properties(), *theme_);
  return themes::is_ok(theme_status_) ? Status::OK : Status::SUBSYSTEM_FAILURE;
}

Status ThemeSubsystem::stop(AppState& app) noexcept {
  static_cast<void>(app);
  return Status::OK;
}

Status ThemeSubsystem::terminate(AppState& app) noexcept {
  static_cast<void>(app);
  theme_.reset();
  return Status::OK;
}

}  // namespace puc::app
