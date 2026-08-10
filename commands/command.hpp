#pragma once

/**
 * @file command.hpp
 * @brief Command registration, completion, metadata, and dispatch.
 */

#include <memory>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "utils/containers/trie.hpp"

namespace puc::ipc {
class Channel;
class Directory;
}  // namespace puc::ipc

namespace puc::multithreading {
class JobQueue;
}

namespace puc::app {
class AppState;
}

namespace puc::tui {
class Screen;
}  // namespace puc::tui

namespace puc::command {

/** Shared application services available to every command invocation. */
struct CommonCommandArgs {
  multithreading::JobQueue* workers = nullptr; /**< Borrowed worker pool. */
  tui::Screen* screen = nullptr; /**< Borrowed active terminal presentation. */
  ipc::Directory* directory = nullptr; /**< Borrowed channel directory. */
  app::AppState* state      = nullptr; /**< Borrowed application lifecycle. */
};

/** Result of command registration, lookup, or execution. */
enum class Status {
  OK,               /**< The requested operation completed. */
  INVALID_ARGUMENT, /**< A name, alias, command, or argument is invalid. */
  DUPLICATE_COMMAND_NAME,  /**< A name or alias is already registered. */
  COMMAND_NOT_FOUND,       /**< No command owns the requested spelling. */
  MESSAGE_ENCODING_FAILED, /**< Notification text could not be encoded. */
  NOTIFICATION_FAILED,     /**< The notification channel rejected a message. */
  NOT_ALLOWED,             /**< Current application policy forbids execution. */
  INTERNAL_ERROR,          /**< Command execution failed internally. */
};

/** Return whether a command operation succeeded. */
constexpr bool is_ok(Status status) noexcept { return status == Status::OK; }

/** Return stable human-readable text for a command result. */
constexpr std::string_view status_message(Status status) noexcept {
  switch (status) {
    case Status::OK:
      return "success";
    case Status::INVALID_ARGUMENT:
      return "invalid command argument";
    case Status::DUPLICATE_COMMAND_NAME:
      return "command name or alias is already registered";
    case Status::COMMAND_NOT_FOUND:
      return "command was not found";
    case Status::MESSAGE_ENCODING_FAILED:
      return "command notification could not be encoded";
    case Status::NOTIFICATION_FAILED:
      return "command notification could not be delivered";
    case Status::NOT_ALLOWED:
      return "command is not allowed";
    case Status::INTERNAL_ERROR:
      return "command failed internally";
  }
  return "unknown command status";
}

/**
 * Encode and publish one notification through `common_args.directory`.
 *
 * CommandNotificationChannelSubsystem must have opened the canonical route in
 * the same Directory. Empty text is valid and clears an earlier notification
 * when consumed.
 *
 * @return Status::OK, Status::INVALID_ARGUMENT when no Directory is supplied,
 *         Status::MESSAGE_ENCODING_FAILED for malformed UTF-8, or
 *         Status::NOTIFICATION_FAILED when the channel rejects the payload.
 */
Status send_notification(const CommonCommandArgs& common_args,
                         std::string text);

/**
 * One operation callable from command mode or another application component.
 *
 * Implementations receive arguments after the command name. Descriptions are
 * displayed next to command-name completions; nonempty usage text is displayed
 * after a command spelling has been completed.
 */
class CommandApp {
 public:
  /** Destroy a command through the shared type-erased interface. */
  virtual ~CommandApp() = default;

  /** Execute with shared services and arguments excluding the command name. */
  virtual Status run(CommonCommandArgs common_args,
                     std::span<const std::string> args) = 0;

  /** Return the short text displayed beside a completion candidate. */
  virtual std::string get_description() const = 0;

  /** Return argument and subcommand help, or an empty string for none. */
  virtual std::string get_usage() const = 0;
};

/**
 * Thread-safe registry of command names, aliases, and shared implementations.
 *
 * Names are case-sensitive, nonempty tokens containing no ASCII whitespace or
 * control bytes. Aliases behave exactly like canonical names for completion,
 * metadata, and execution. Registration never replaces an existing spelling,
 * while prefix relationships such as `q` and `quit` are valid.
 *
 * All registry access is synchronized. `dispatch()` retains a shared command
 * reference and releases the registry lock before calling user code, allowing
 * a command to perform long-running or reentrant work without blocking lookup.
 * Channel lifetime is deliberately external: CommandDispatcher owns only the
 * command Trie and shared command implementations.
 */
class CommandDispatcher {
 public:
  /** Construct an empty registry. */
  CommandDispatcher() = default;

  CommandDispatcher(const CommandDispatcher&)            = delete;
  CommandDispatcher& operator=(const CommandDispatcher&) = delete;
  CommandDispatcher(CommandDispatcher&&)                 = delete;
  CommandDispatcher& operator=(CommandDispatcher&&)      = delete;

  /** Release registered command implementations and Trie storage. */
  ~CommandDispatcher() = default;

  /**
   * Register one canonical name and zero or more aliases atomically.
   *
   * @return Status::OK, Status::INVALID_ARGUMENT for a null command or invalid
   *         spelling, or Status::DUPLICATE_COMMAND_NAME when any supplied
   *         spelling is repeated or already registered. No spelling is added
   *         on either validation failure.
   */
  Status register_command(std::string name, std::vector<std::string> aliases,
                          std::shared_ptr<CommandApp> command);

  /**
   * Run the command registered under a canonical name or alias.
   *
   * @return Status::INVALID_ARGUMENT for an invalid spelling,
   *         Status::COMMAND_NOT_FOUND for an unregistered spelling, or the
   *         result returned by the command.
   */
  Status dispatch(std::string_view name, CommonCommandArgs common_args,
                  std::span<const std::string> args) const;

  /** Return whether an exact canonical name or alias is registered. */
  bool contains(std::string_view name) const;

  /**
   * Return every registered spelling beginning with `prefix`.
   *
   * Results retain the trie's stable branch-insertion order. An empty prefix
   * lists every spelling; an invalid or absent prefix produces an empty list.
   */
  std::vector<std::string> list_completions(std::string_view prefix = {}) const;

  /** Return completion-list text, or an empty string when name is absent. */
  std::string get_command_description(std::string_view name) const;

  /** Return argument help, or an empty string when absent or undocumented. */
  std::string get_command_usage(std::string_view name) const;

 private:
  using CommandTrie =
      containers::Trie<char, std::shared_ptr<CommandApp>>; /**< Name index. */

  /** Copy a registered command reference while holding the registry lock. */
  std::shared_ptr<CommandApp> find_command(std::string_view name) const;

  mutable std::shared_mutex mutex_; /**< Synchronizes registry state. */
  CommandTrie command_trie_;        /**< Character-keyed spelling registry. */
};

}  // namespace puc::command
