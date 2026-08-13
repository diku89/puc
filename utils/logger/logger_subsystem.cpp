/**
 * @file logger_subsystem.cpp
 * @brief Process-wide logger subsystem implementation.
 */

#include "utils/logger/logger_subsystem.hpp"

#include <utility>

#include "utils/logger/logger.hpp"

namespace puc::app {

LoggerSubsystem::LoggerSubsystem(logger::LoggerConf configuration)
    : AppSubsystem("logger"), configuration_(std::move(configuration)) {}

Status LoggerSubsystem::initialize(AppState& app) {
  static_cast<void>(app);
  if (installed_logger_ != nullptr) {
    return Status::OK;
  }
  logger::init_logger(configuration_);
  installed_logger_ = logger::get_logger();
  return installed_logger_ == nullptr ? Status::SUBSYSTEM_FAILURE : Status::OK;
}

Status LoggerSubsystem::start(AppState& app) {
  static_cast<void>(app);
  return installed_logger_ == nullptr ? Status::SUBSYSTEM_FAILURE : Status::OK;
}

Status LoggerSubsystem::stop(AppState& app) noexcept {
  static_cast<void>(app);
  return Status::OK;
}

Status LoggerSubsystem::terminate(AppState& app) noexcept {
  static_cast<void>(app);
  release_logger();
  return Status::OK;
}

void LoggerSubsystem::release_logger() noexcept {
  static_cast<void>(logger::clear_logger(installed_logger_));
  installed_logger_.reset();
}

}  // namespace puc::app
