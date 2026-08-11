#pragma once

/**
 * @file commands.hpp
 * @brief Lifecycle adapter for command registration and dispatch.
 */

#include <memory>

#include "commands/command.hpp"
#include "puc-cli/state/state.hpp"

namespace puc::app {

/**
 * Own CommandDispatcher for the initialized application lifetime.
 *
 * The registry survives AppState stop/start cycles, while its borrowed common
 * services are resolved from the currently running adapter generation for each
 * invocation. CommandNotificationChannelSubsystem owns transport lifetime;
 * CommandDispatcher remains concerned only with names, metadata, and calls.
 */
class CommandSubsystem final : public AppSubsystem {
 public:
  /** Declare notification and Screen service dependencies. */
  CommandSubsystem();

  /** Construct the persistent command registry. */
  Status initialize(AppState& app) override;

  /** Validate the running services supplied to command invocations. */
  Status start(AppState& app) override;

  /** Quiesce command entry while retaining registered commands for restart. */
  Status stop(AppState& app) noexcept override;

  /** Release the registry and every registered command implementation. */
  Status terminate(AppState& app) noexcept override;

  /** Return the initialized dispatcher, or nullptr outside its lifetime. */
  command::CommandDispatcher* dispatcher() noexcept {
    return dispatcher_.get();
  }

  /** Return the initialized dispatcher, or nullptr outside its lifetime. */
  const command::CommandDispatcher* dispatcher() const noexcept {
    return dispatcher_.get();
  }

  /** Resolve borrowed services for one synchronous command invocation. */
  command::CommonCommandArgs common_args(AppState& app) const noexcept;

 private:
  std::unique_ptr<command::CommandDispatcher>
      dispatcher_; /**< Registry retained until application termination. */
};

}  // namespace puc::app
