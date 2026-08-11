/**
 * @file command.cpp
 * @brief Command registry and dispatcher implementation.
 */

#include "commands/command.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "msgs/cmdframe_msgs.hpp"
#include "msgs/status.hpp"
#include "utils/ipc/directory.hpp"
#include "utils/ipc/status.hpp"
#include "utils/logger/logger.hpp"

/** @cond COMMAND_DISPATCHER_LOGGER_MODULE */
LOGGER_MODULE("Command Dispatcher");
/** @endcond */

namespace puc::command {
namespace {

/** Return whether one spelling can be represented by the command tokenizer. */
bool valid_command_name(std::string_view name) noexcept {
  if (name.empty()) {
    return false;
  }
  for (const char character : name) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte <= 0x20U || byte == 0x7fU) {
      return false;
    }
  }
  return true;
}

/** Convert a borrowed spelling to the Trie's byte-sequence representation. */
std::vector<char> command_key(std::string_view name) {
  return {name.begin(), name.end()};
}

}  // namespace

Status send_notification(const CommonCommandArgs& common_args,
                         std::string text) {
  if (common_args.directory == nullptr) {
    return Status::INVALID_ARGUMENT;
  }

  std::vector<std::uint8_t> payload;
  const msg::Status encoding = msg::CmdFrameNotificationCodec{}.serialize(
      msg::CmdFrameNotification{.text = std::move(text)}, payload);
  if (!msg::is_ok(encoding)) {
    Logger<ERROR> << "Could not encode command notification: "
                  << msg::status_message(encoding);
    return Status::MESSAGE_ENCODING_FAILED;
  }

  const ipc::TransferResult transfer =
      common_args.directory->transmit(msg::kCmdFrameNotifyChannel, payload);
  if (!ipc::is_ok(transfer.status) || transfer.bytes != payload.size()) {
    Logger<WARN> << "Command notification was not accepted: "
                 << ipc::status_message(transfer.status);
    return Status::NOTIFICATION_FAILED;
  }
  return Status::OK;
}

Status CommandDispatcher::register_command(
    std::string name, std::vector<std::string> aliases,
    std::shared_ptr<CommandApp> command) {
  if (command == nullptr || !valid_command_name(name)) {
    return Status::INVALID_ARGUMENT;
  }

  std::vector<std::string> spellings;
  spellings.reserve(1U + aliases.size());
  spellings.push_back(std::move(name));
  for (std::string& alias : aliases) {
    if (!valid_command_name(alias)) {
      return Status::INVALID_ARGUMENT;
    }
    spellings.push_back(std::move(alias));
  }

  for (std::size_t spelling = 0U; spelling < spellings.size(); ++spelling) {
    for (std::size_t earlier = 0U; earlier < spelling; ++earlier) {
      if (spellings[spelling] == spellings[earlier]) {
        return Status::DUPLICATE_COMMAND_NAME;
      }
    }
  }

  std::vector<std::vector<char>> keys;
  keys.reserve(spellings.size());
  for (const std::string& spelling : spellings) {
    keys.push_back(command_key(spelling));
  }

  const std::unique_lock lock(mutex_);
  for (const std::vector<char>& key : keys) {
    if (command_trie_.find(key) != nullptr) {
      return Status::DUPLICATE_COMMAND_NAME;
    }
  }
  for (const std::vector<char>& key : keys) {
    command_trie_.insert(key, command);
  }
  return Status::OK;
}

Status CommandDispatcher::dispatch(std::string_view name,
                                   CommonCommandArgs common_args,
                                   std::span<const std::string> args) const {
  if (!valid_command_name(name)) {
    return Status::INVALID_ARGUMENT;
  }
  std::shared_ptr<CommandApp> command = find_command(name);
  return command == nullptr ? Status::COMMAND_NOT_FOUND
                            : command->run(std::move(common_args), args);
}

bool CommandDispatcher::contains(std::string_view name) const {
  return valid_command_name(name) && find_command(name) != nullptr;
}

std::vector<std::string> CommandDispatcher::list_completions(
    std::string_view prefix) const {
  if (!prefix.empty() && !valid_command_name(prefix)) {
    return {};
  }

  const std::vector<char> prefix_key = command_key(prefix);
  const std::shared_lock lock(mutex_);
  const std::vector<std::vector<char>> keys =
      command_trie_.completions(prefix_key);

  std::vector<std::string> completions;
  completions.reserve(keys.size());
  for (const std::vector<char>& key : keys) {
    completions.emplace_back(key.begin(), key.end());
  }
  return completions;
}

std::string CommandDispatcher::get_command_description(
    std::string_view name) const {
  std::shared_ptr<CommandApp> command = find_command(name);
  return command == nullptr ? std::string{} : command->get_description();
}

std::string CommandDispatcher::get_command_usage(std::string_view name) const {
  std::shared_ptr<CommandApp> command = find_command(name);
  return command == nullptr ? std::string{} : command->get_usage();
}

std::shared_ptr<CommandApp> CommandDispatcher::find_command(
    std::string_view name) const {
  const std::vector<char> key = command_key(name);
  const std::shared_lock lock(mutex_);
  const std::shared_ptr<CommandApp>* command = command_trie_.find(key);
  return command == nullptr ? nullptr : *command;
}

}  // namespace puc::command
