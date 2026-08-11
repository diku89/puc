#pragma once

/**
 * Implement the quit command.
 *
 * \command quit || Quit the application.
 * \alias q
 * \alias exit
 */

#include "commands/command.hpp"

namespace puc {
namespace command {

class QuitCommand : public CommandApp {
 public:
  QuitCommand()           = default;
  ~QuitCommand() override = default;

  Status run(CommonCommandArgs common_args,
             std::span<const std::string> args) override;

  std::string get_description() const override { return "Quit puc."; }

  std::string get_usage() const override { return ""; }

  static std::vector<std::string> get_aliases() { return {"q", "exit"}; }
};

}  // namespace command
}  // namespace puc
