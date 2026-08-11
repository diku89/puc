#pragma once

/**
 * Implement the config comand.
 *
 * \command config || View, edit, or reload the configuration.
 * \subcommand get <full.key.path> || Get the value of a configuration key.
 * \subcommand set <full.key.path> <value> || Set the value of a configuration
 * key. \subcommand list [partial.prefix] || List all configuration keys,
 * optionally filtered by a prefix. \subcommand reload || Reload all
 * configurations from config files.
 */

#include "commands/command.hpp"

namespace puc {
namespace command {

class ConfigCommand : public CommandApp {
 public:
  ConfigCommand()           = default;
  ~ConfigCommand() override = default;

  Status run(CommonCommandArgs common_args,
             std::span<const std::string> args) override;

  std::string get_description() const override {
    return "View, edit, or reload the configuration.";
  }

  std::string get_usage() const override {
    return "get <full.key.path> \n"
           "set <full.key.path> <value> \n"
           "list [partial.prefix] \n"
           "reload";
  }

  static std::vector<std::string> get_aliases() { return {}; }
};

}  // namespace command
}  // namespace puc
