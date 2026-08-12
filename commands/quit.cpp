/**
 * @file quit.cpp
 * @brief Deferred application-exit command implementation.
 */

#include "commands/quit.hpp"

#include <span>
#include <string>

#include "puc-cli/application/application_control.hpp"

namespace puc::command {

Status QuitCommand::run(CommonCommandArgs common_args,
                        std::span<const std::string> args) {
  if (!args.empty() || common_args.control == nullptr) {
    return Status::INVALID_ARGUMENT;
  }
  common_args.control->request_exit();
  return Status::OK;
}

}  // namespace puc::command
