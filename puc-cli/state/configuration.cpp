/**
 * @file configuration.cpp
 * @brief Application configuration lifecycle implementation.
 */

#include "puc-cli/state/configuration.hpp"

#include <memory>
#include <utility>

#include "puc-cli/state/logger.hpp"
#include "utils/config/config.hpp"

namespace puc::app {

ConfigurationSubsystem::ConfigurationSubsystem(
    ConfigurationSubsystemOptions options)
    : AppSubsystem("configuration", subsystem_dependencies<LoggerSubsystem>()),
      options_(std::move(options)) {}

ConfigurationSubsystem::~ConfigurationSubsystem() = default;

Status ConfigurationSubsystem::initialize(AppState& app) {
  static_cast<void>(app);
  if (configuration_ != nullptr) {
    return Status::OK;
  }
  configuration_ = std::make_unique<config::Config>(
      options_.primary_root, options_.user_overrides_root);
  return Status::OK;
}

Status ConfigurationSubsystem::start(AppState& app) {
  static_cast<void>(app);
  return configuration_ == nullptr ? Status::SUBSYSTEM_FAILURE : Status::OK;
}

Status ConfigurationSubsystem::stop(AppState& app) noexcept {
  static_cast<void>(app);
  return Status::OK;
}

Status ConfigurationSubsystem::terminate(AppState& app) noexcept {
  static_cast<void>(app);
  configuration_.reset();
  return Status::OK;
}

}  // namespace puc::app
