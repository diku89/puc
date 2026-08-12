/**
 * @file command_mode.cpp
 * @brief Command-mode lifecycle and event coordination implementation.
 */

#include "puc-cli/state/command_mode.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "commands/command.hpp"
#include "puc-cli/state/builtin_commands.hpp"
#include "puc-cli/state/commands.hpp"
#include "puc-cli/state/input.hpp"
#include "puc-cli/tui/input_frame.hpp"

namespace puc::app {
namespace {

/** Return a non-release key action, or nullptr for every other event. */
const terminal::KeyEvent* pressed_key(const terminal::Event& event) noexcept {
  const auto* key = std::get_if<terminal::KeyEvent>(&event);
  if (key == nullptr || key->action == terminal::KeyAction::RELEASE) {
    return nullptr;
  }
  return key;
}

/** Split command text into simple ASCII-whitespace-delimited arguments. */
std::vector<std::string> tokenize(std::string_view text) {
  std::vector<std::string> tokens;
  std::size_t offset = 0U;
  while (offset < text.size()) {
    while (offset < text.size() &&
           std::isspace(static_cast<unsigned char>(text[offset])) != 0) {
      ++offset;
    }
    const std::size_t beginning = offset;
    while (offset < text.size() &&
           std::isspace(static_cast<unsigned char>(text[offset])) == 0) {
      ++offset;
    }
    if (offset != beginning) {
      tokens.emplace_back(text.substr(beginning, offset - beginning));
    }
  }
  return tokens;
}

/** Split newline-separated usage into independently rendered rows. */
std::vector<std::string> usage_rows(std::string_view usage) {
  std::vector<std::string> rows;
  std::size_t beginning = 0U;
  while (beginning <= usage.size()) {
    const std::size_t end = usage.find('\n', beginning);
    rows.emplace_back(usage.substr(beginning, end == std::string_view::npos
                                                  ? usage.size() - beginning
                                                  : end - beginning));
    if (end == std::string_view::npos) {
      break;
    }
    beginning = end + 1U;
  }
  return rows;
}

}  // namespace

CommandModeSubsystem::CommandModeSubsystem()
    : AppSubsystem("command-mode",
                   subsystem_dependencies<InputSubsystem, CommandSubsystem,
                                          BuiltinCommandSubsystem>()) {}

CommandModeSubsystem::~CommandModeSubsystem() = default;

Status CommandModeSubsystem::initialize(AppState& app) {
  InputSubsystem* input      = app.get_subsystem<InputSubsystem>();
  CommandSubsystem* commands = app.get_subsystem<CommandSubsystem>();
  if (input == nullptr || commands == nullptr ||
      input->input_frame() == nullptr || commands->dispatcher() == nullptr) {
    return Status::SUBSYSTEM_FAILURE;
  }
  input_frame_ = input->input_frame();
  dispatcher_  = commands->dispatcher();
  return Status::OK;
}

Status CommandModeSubsystem::start(AppState& app) {
  const std::unique_lock lock(mutex_);
  if (input_frame_ == nullptr || dispatcher_ == nullptr) {
    return Status::SUBSYSTEM_FAILURE;
  }
  app_    = &app;
  active_ = true;
  if (waiting_for_acknowledgement_) {
    input_frame_->set_command_help({});
  } else {
    refresh_help();
  }
  return Status::OK;
}

Status CommandModeSubsystem::stop(AppState& app) noexcept {
  static_cast<void>(app);
  const std::unique_lock lock(mutex_);
  active_ = false;
  app_    = nullptr;
  return Status::OK;
}

Status CommandModeSubsystem::terminate(AppState& app) noexcept {
  static_cast<void>(stop(app));
  const std::unique_lock lock(mutex_);
  waiting_for_acknowledgement_ = false;
  completions_.clear();
  usage_.clear();
  prefix_.clear();
  selected_completion_ = 0U;
  if (input_frame_ != nullptr) {
    input_frame_->set_command_help({});
  }
  input_frame_.reset();
  dispatcher_ = nullptr;
  return Status::OK;
}

tui::Status CommandModeSubsystem::handle_event(
    const terminal::Event& event, std::chrono::steady_clock::time_point now) {
  const std::unique_lock lock(mutex_);
  if (!active_ || app_ == nullptr || input_frame_ == nullptr ||
      dispatcher_ == nullptr) {
    return tui::Status::INVALID_ARGUMENT;
  }

  const terminal::KeyEvent* key = pressed_key(event);
  const terminal::NamedKey* named =
      key == nullptr ? nullptr
                     : std::get_if<terminal::NamedKey>(&key->key.value);
  if (waiting_for_acknowledgement_ && key != nullptr) {
    waiting_for_acknowledgement_ = false;
    input_frame_->leave_command_mode();
    completions_.clear();
    usage_.clear();
    prefix_.clear();
    input_frame_->set_command_help({});
    return tui::Status::OK;
  }

  const tui::InputFrameSnapshot before = input_frame_->snapshot();
  if (before.mode == tui::InputMode::COMMAND && named != nullptr) {
    if (*named == terminal::NamedKey::UP && !completions_.empty()) {
      selected_completion_ = selected_completion_ == 0U
                                 ? completions_.size() - 1U
                                 : selected_completion_ - 1U;
      refresh_help();
      return tui::Status::OK;
    }
    if (*named == terminal::NamedKey::DOWN && !completions_.empty()) {
      selected_completion_ = (selected_completion_ + 1U) % completions_.size();
      refresh_help();
      return tui::Status::OK;
    }
    if (*named == terminal::NamedKey::TAB) {
      return autocomplete();
    }
    if (*named == terminal::NamedKey::ENTER ||
        *named == terminal::NamedKey::KEYPAD_ENTER) {
      const std::vector<std::string> tokens = tokenize(before.command_text);
      if (tokens.empty() || !dispatcher_->contains(tokens.front())) {
        return completions_.empty() ? dispatch() : autocomplete();
      }
      return dispatch();
    }
  }

  const tui::Status status = input_frame_->handle_event(event, now);
  if (tui::is_ok(status)) {
    refresh_help();
  }
  return status;
}

CommandModeSnapshot CommandModeSubsystem::snapshot() const {
  const std::shared_lock lock(mutex_);
  return CommandModeSnapshot{
      .completions                 = completions_,
      .usage                       = usage_,
      .selected_completion         = selected_completion_,
      .waiting_for_acknowledgement = waiting_for_acknowledgement_,
      .active                      = active_,
  };
}

void CommandModeSubsystem::refresh_help() {
  if (input_frame_ == nullptr || dispatcher_ == nullptr) {
    return;
  }
  const tui::InputFrameSnapshot frame = input_frame_->snapshot();
  if (frame.mode != tui::InputMode::COMMAND) {
    completions_.clear();
    usage_.clear();
    prefix_.clear();
    selected_completion_ = 0U;
    input_frame_->set_command_help({});
    return;
  }

  const std::size_t separator   = frame.command_text.find_first_of(" \t\r\n");
  const std::string next_prefix = frame.command_text.substr(0U, separator);
  if (next_prefix != prefix_) {
    selected_completion_ = 0U;
    prefix_              = next_prefix;
  }

  completions_.clear();
  for (std::string spelling : dispatcher_->list_completions(prefix_)) {
    completions_.push_back(CommandCompletion{
        .command     = spelling,
        .description = dispatcher_->get_command_description(spelling),
    });
  }
  if (!completions_.empty()) {
    selected_completion_ =
        std::min(selected_completion_, completions_.size() - 1U);
  } else {
    selected_completion_ = 0U;
  }

  usage_ = dispatcher_->contains(prefix_)
               ? dispatcher_->get_command_usage(prefix_)
               : std::string{};
  std::vector<std::string> help;
  if (!usage_.empty()) {
    help = usage_rows(usage_);
  } else if (completions_.size() > 1U) {
    help.reserve(completions_.size());
    for (std::size_t index = 0U; index < completions_.size(); ++index) {
      std::string row = index == selected_completion_ ? "> " : "  ";
      row.append(completions_[index].command);
      if (!completions_[index].description.empty()) {
        row.append("    ");
        row.append(completions_[index].description);
      }
      help.push_back(std::move(row));
    }
  }
  input_frame_->set_command_help(std::move(help));
}

tui::Status CommandModeSubsystem::autocomplete() {
  if (input_frame_ == nullptr || completions_.empty()) {
    return tui::Status::OK;
  }
  selected_completion_ =
      std::min(selected_completion_, completions_.size() - 1U);
  const tui::Status status = input_frame_->replace_command_text(
      completions_[selected_completion_].command + " ");
  if (tui::is_ok(status)) {
    refresh_help();
  }
  return status;
}

tui::Status CommandModeSubsystem::dispatch() {
  if (input_frame_ == nullptr || dispatcher_ == nullptr || app_ == nullptr) {
    return tui::Status::INVALID_ARGUMENT;
  }
  const std::vector<std::string> tokens =
      tokenize(input_frame_->snapshot().command_text);
  if (tokens.empty()) {
    input_frame_->set_notification(std::string{
        command::status_message(command::Status::INVALID_ARGUMENT)});
    waiting_for_acknowledgement_ = true;
    return tui::Status::OK;
  }

  CommandSubsystem* commands = app_->get_subsystem<CommandSubsystem>();
  if (commands == nullptr) {
    return tui::Status::INVALID_ARGUMENT;
  }
  const command::Status result =
      dispatcher_->dispatch(tokens.front(), commands->common_args(*app_),
                            std::span<const std::string>{tokens}.subspan(1U));
  if (!command::is_ok(result)) {
    input_frame_->set_notification(
        std::string{command::status_message(result)});
  }
  waiting_for_acknowledgement_ = true;
  completions_.clear();
  usage_.clear();
  input_frame_->set_command_help({});
  return tui::Status::OK;
}

}  // namespace puc::app
