/**
 * @file properties_subsystem.cpp
 * @brief Application properties lifecycle implementation.
 */

#include "properties/properties_subsystem.hpp"

#include <memory>
#include <utility>

#include "properties/properties.hpp"
#include "utils/logger/logger_subsystem.hpp"

namespace puc::app {

PropertiesSubsystem::PropertiesSubsystem(PropertiesSubsystemOptions options)
    : AppSubsystem("properties", subsystem_dependencies<LoggerSubsystem>()),
      options_(std::move(options)) {}

PropertiesSubsystem::~PropertiesSubsystem() = default;

Status PropertiesSubsystem::initialize(AppState& app) {
  static_cast<void>(app);
  if (properties_ == nullptr) {
    properties_ = std::make_unique<properties::Properties>(
        options_.primary_root, options_.user_overrides_root);
  }
  return Status::OK;
}

Status PropertiesSubsystem::start(AppState& app) {
  static_cast<void>(app);
  return properties_ == nullptr ? Status::SUBSYSTEM_FAILURE : Status::OK;
}

Status PropertiesSubsystem::stop(AppState& app) noexcept {
  static_cast<void>(app);
  return Status::OK;
}

Status PropertiesSubsystem::terminate(AppState& app) noexcept {
  static_cast<void>(app);
  properties_.reset();
  return Status::OK;
}

}  // namespace puc::app
