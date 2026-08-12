/**
 * @file builtin_command_subsystem.cpp
 * @brief Built-in command lifecycle registration implementation.
 */

#include "commands/builtin_command_subsystem.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "commands/command.hpp"
#include "commands/command_subsystem.hpp"
#include "commands/config.hpp"
#include "commands/quit.hpp"

namespace puc::app {

BuiltinCommandSubsystem::BuiltinCommandSubsystem()
    : AppSubsystem("built-in-commands",
                   subsystem_dependencies<CommandSubsystem>()) {}

Status BuiltinCommandSubsystem::initialize(AppState& app) {
  CommandSubsystem* commands = app.get_subsystem<CommandSubsystem>();
  if (commands == nullptr || commands->dispatcher() == nullptr) {
    return Status::SUBSYSTEM_FAILURE;
  }
  dispatcher_ = commands->dispatcher();
  command::Status status =
      dispatcher_->register_command("quit", command::QuitCommand::get_aliases(),
                                    std::make_shared<command::QuitCommand>());
  if (!command::is_ok(status)) {
    dispatcher_ = nullptr;
    return Status::SUBSYSTEM_FAILURE;
  }
  status = dispatcher_->register_command(
      "config", command::ConfigCommand::get_aliases(),
      std::make_shared<command::ConfigCommand>());
  if (!command::is_ok(status)) {
    dispatcher_ = nullptr;
    return Status::SUBSYSTEM_FAILURE;
  }
  return Status::OK;
}

Status BuiltinCommandSubsystem::start(AppState& app) {
  static_cast<void>(app);
  return dispatcher_ != nullptr && dispatcher_->contains("quit") &&
                 dispatcher_->contains("config")
             ? Status::OK
             : Status::SUBSYSTEM_FAILURE;
}

Status BuiltinCommandSubsystem::stop(AppState& app) noexcept {
  static_cast<void>(app);
  return Status::OK;
}

Status BuiltinCommandSubsystem::terminate(AppState& app) noexcept {
  static_cast<void>(app);
  dispatcher_ = nullptr;
  return Status::OK;
}

}  // namespace puc::app
