#pragma once

/**
 * @file logger.hpp
 * @brief Application lifecycle adapter for the process-wide logger.
 */

#include <memory>

#include "state/state.hpp"
#include "utils/logger/logger.hpp"

namespace puc::app {

/**
 * Install and release the process-wide logger for one initialized application.
 *
 * Other adapters that need complete lifecycle diagnostics declare this type
 * as a dependency. On termination, the adapter clears only the exact Logger
 * instance it installed, preserving any deliberate replacement made by
 * another owner. The logger remains available while AppState is stopped.
 */
class LoggerSubsystem final : public AppSubsystem {
 public:
  /** Construct an adapter retaining an immutable logger configuration. */
  explicit LoggerSubsystem(logger::LoggerConf configuration = {});

  /** Construct and install the global Logger before dependents initialize. */
  Status initialize(AppState& app) override;

  /** Verify that initialization retained a Logger instance. */
  Status start(AppState& app) override;

  /** Keep logging available while the initialized application is stopped. */
  Status stop(AppState& app) noexcept override;

  /** Release any Logger retained after partial lifecycle progress. */
  Status terminate(AppState& app) noexcept override;

  /** Return this adapter's installed Logger, or nullptr before/after use. */
  std::shared_ptr<logger::Logger> logger() const noexcept {
    return installed_logger_;
  }

  /** Return the immutable configuration used during initialization. */
  const logger::LoggerConf& configuration() const noexcept {
    return configuration_;
  }

 private:
  /** Relinquish the installed global logger; safe to call repeatedly. */
  void release_logger() noexcept;

  logger::LoggerConf configuration_; /**< Configuration retained for restart. */
  std::shared_ptr<logger::Logger>
      installed_logger_; /**< Exact instance owned while initialized. */
};

}  // namespace puc::app
