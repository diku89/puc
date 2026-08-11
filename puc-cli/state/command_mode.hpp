#pragma once

/**
 * @file command_mode.hpp
 * @brief Lifecycle coordinator between command input and dispatch.
 */

#include <chrono>
#include <cstddef>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

#include "puc-cli/state/state.hpp"
#include "puc-cli/terminal/event.hpp"
#include "puc-cli/tui/status.hpp"

namespace puc::command {
class CommandDispatcher;
}

namespace puc::tui {
class InputFrame;
}

namespace puc::app {

/** One command-name completion and the description shown beside it. */
struct CommandCompletion {
  std::string command;     /**< Registered command spelling or alias. */
  std::string description; /**< Short command description. */

  /** Compare completion text and metadata. */
  bool operator==(const CommandCompletion&) const = default;
};

/** Read-only command-mode state for tests and presentation diagnostics. */
struct CommandModeSnapshot {
  std::vector<CommandCompletion> completions; /**< Current prefix matches. */
  std::string usage;                          /**< Exact-command help text. */
  std::size_t selected_completion  = 0U;      /**< Arrow-selected candidate. */
  bool waiting_for_acknowledgement = false;   /**< Command has returned. */
  bool active = false; /**< Running lifecycle generation. */
};

/**
 * Coordinate CmdFrame editing, completion metadata, and command execution.
 *
 * The durable controller binds InputSubsystem and CommandSubsystem during the
 * one initialized lifetime. Start enables event handling for the current app
 * generation; stop disables entry while preserving completion, result, and
 * editor state so a later start resumes the same interaction. Terminate alone
 * releases that durable state and its registry/frame bindings.
 */
class CommandModeSubsystem final : public AppSubsystem {
 public:
  /** Declare input-frame and command-dispatcher dependencies. */
  CommandModeSubsystem();

  /** Destroy released controller state. */
  ~CommandModeSubsystem() override;

  /** Bind the durable command dispatcher and InputFrame. */
  Status initialize(AppState& app) override;

  /** Enable command interaction for one running generation. */
  Status start(AppState& app) override;

  /** Pause command interaction while preserving its durable UI state. */
  Status stop(AppState& app) noexcept override;

  /** Release durable bindings to the dispatcher and frame. */
  Status terminate(AppState& app) noexcept override;

  /** Route one event through command-mode behavior or ordinary InputFrame. */
  tui::Status handle_event(const terminal::Event& event,
                           std::chrono::steady_clock::time_point now =
                               std::chrono::steady_clock::now());

  /** Return a consistent copy of completion and execution state. */
  CommandModeSnapshot snapshot() const;

 private:
  /** Recompute prefix candidates, usage, and visible help rows. */
  void refresh_help();

  /** Complete the currently selected or sole command spelling. */
  tui::Status autocomplete();

  /** Dispatch the current command buffer and enter acknowledgement state. */
  tui::Status dispatch();

  mutable std::shared_mutex mutex_; /**< Synchronizes controller state. */
  std::shared_ptr<tui::InputFrame>
      input_frame_; /**< Durable frame owned by InputSubsystem. */
  command::CommandDispatcher* dispatcher_ =
      nullptr;              /**< Durable registry owned by CommandSubsystem. */
  AppState* app_ = nullptr; /**< Current running generation for command args. */
  std::vector<CommandCompletion>
      completions_;                      /**< Current prefix candidates. */
  std::string usage_;                    /**< Exact-command usage text. */
  std::string prefix_;                   /**< Prefix behind candidates. */
  std::size_t selected_completion_ = 0U; /**< Arrow-selected candidate. */
  bool waiting_for_acknowledgement_ =
      false;            /**< Whether any key should exit. */
  bool active_ = false; /**< Whether events are accepted. */
};

}  // namespace puc::app
