#pragma once

/**
 * @file builtin_commands.hpp
 * @brief Lifecycle registration of commands shipped with PUC.
 */

#include "state/state.hpp"

namespace puc::command {
class CommandDispatcher;
}

namespace puc::app {

/** Register built-in commands for one initialized CommandSubsystem registry. */
class BuiltinCommandSubsystem final : public AppSubsystem {
 public:
  /** Declare the durable command-registry dependency. */
  BuiltinCommandSubsystem();

  /** Register every built-in command and alias exactly once. */
  Status initialize(AppState& app) override;

  /** Validate that the persistent built-in catalog remains registered. */
  Status start(AppState& app) override;

  /** Retain registrations across stop/start cycles. */
  Status stop(AppState& app) noexcept override;

  /** Drop the registry binding before CommandSubsystem terminates. */
  Status terminate(AppState& app) noexcept override;

 private:
  command::CommandDispatcher* dispatcher_ =
      nullptr; /**< Registry owned by CommandSubsystem. */
};

}  // namespace puc::app
