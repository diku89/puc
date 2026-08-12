/**
 * @file input_test_runtime.cpp
 * @brief Primary lifecycle-owned logic for the InputFrame manual test app.
 */

#include "puc-cli/tui/input_test_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "puc-cli/state/application_control.hpp"
#include "puc-cli/state/command_mode.hpp"
#include "puc-cli/state/control.hpp"
#include "puc-cli/state/embedded_terminal.hpp"
#include "puc-cli/state/input.hpp"
#include "puc-cli/state/presentation.hpp"
#include "puc-cli/state/screen.hpp"
#include "puc-cli/state/terminal.hpp"
#include "puc-cli/state/theme.hpp"
#include "puc-cli/terminal/event.hpp"
#include "puc-cli/terminal/status.hpp"
#include "puc-cli/tui/canvas.hpp"
#include "puc-cli/tui/input_frame.hpp"
#include "puc-cli/tui/layout.hpp"
#include "puc-cli/tui/message_frame.hpp"
#include "puc-cli/tui/renderer.hpp"
#include "puc-cli/tui/screen.hpp"
#include "puc-cli/tui/status.hpp"
#include "puc-cli/tui/theme.hpp"
#include "utils/logger/logger.hpp"
#include "utils/timer/deadline.hpp"

/** @cond INPUT_TEST_RUNTIME_LOGGER_MODULE */
LOGGER_MODULE("Input Test Runtime");
/** @endcond */

namespace puc::app {
namespace {

using tui::Canvas;
using tui::CellDimensions;
using tui::InputFrame;
using tui::Layout;
using tui::ParallelRenderer;
using tui::Screen;
using tui::Theme;

constexpr std::chrono::milliseconds kFrameDelay{16};
constexpr std::string_view kInputFrameId = "input";
constexpr std::string_view kScreenTooSmall =
    "Input frame needs at least 40 columns (terminal mode needs 6 rows)";
constexpr std::string_view kNotification =
    "Enter reserved | Shift+Enter newline | Esc Esc clear | Esc+: command | "
    "Esc+> terminal | mouse selects | Ctrl+C quits";

Canvas::Cell cell(char32_t character, std::uint32_t foreground,
                  std::uint32_t background) {
  return Canvas::Cell{.character        = character,
                      .foreground_color = foreground,
                      .background_color = background};
}

bool requests_exit(const terminal::Event& event) noexcept {
  const auto* key = std::get_if<terminal::KeyEvent>(&event);
  if (key == nullptr || key->action == terminal::KeyAction::RELEASE ||
      !key->modifiers.contains(terminal::Modifier::CONTROL)) {
    return false;
  }
  const auto* character = std::get_if<char32_t>(&key->key.value);
  return character != nullptr && (*character == U'c' || *character == U'C');
}

/** One running app generation over lifecycle-owned mechanisms. */
class InputTestApplication final {
 public:
  InputTestApplication(TerminalSubsystem& terminal,
                       std::shared_ptr<InputFrame> input_frame, Screen& screen,
                       ParallelRenderer& renderer,
                       CommandModeSubsystem& command_mode,
                       EmbeddedTerminalSubsystem& embedded_terminal,
                       Theme& theme, ApplicationControl& control)
      : terminal_(&terminal),
        input_frame_(std::move(input_frame)),
        screen_(&screen),
        renderer_(&renderer),
        command_mode_(&command_mode),
        embedded_terminal_(&embedded_terminal),
        theme_(&theme),
        control_(&control) {}

  ~InputTestApplication() { static_cast<void>(shutdown()); }

  bool setup() {
    if (terminal_ == nullptr || input_frame_ == nullptr || screen_ == nullptr ||
        renderer_ == nullptr || command_mode_ == nullptr ||
        embedded_terminal_ == nullptr || theme_ == nullptr ||
        control_ == nullptr) {
      return false;
    }
    if (input_frame_->snapshot().notification.empty()) {
      input_frame_->set_notification(std::string{kNotification});
    }

    timer::Deadline<> geometry_timeout;
    geometry_timeout.arm(std::chrono::seconds{2});
    tui::Status status = tui::Status::OK;
    while (true) {
      status = screen_->get_dimensions(width_, height_, cell_dimensions_);
      if (tui::is_ok(status)) {
        break;
      }
      if (status != tui::Status::TERMINAL_QUERY_FAILED ||
          geometry_timeout.due()) {
        Logger<ERROR> << "Could not observe terminal geometry: "
                      << tui::status_message(status);
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }

    if (!attach_canvas(width_, height_) || !setup_small_layout() ||
        !update_layout()) {
      return false;
    }

    return true;
  }

  bool draw() {
    std::size_t current_width  = 0U;
    std::size_t current_height = 0U;
    CellDimensions current_cells;
    tui::Status status =
        screen_->get_dimensions(current_width, current_height, current_cells);
    if (!tui::is_ok(status)) {
      Logger<ERROR> << "Could not refresh terminal geometry: "
                    << tui::status_message(status);
      return false;
    }

    const bool dimensions_changed =
        current_width != width_ || current_height != height_;
    if (dimensions_changed || current_cells != cell_dimensions_) {
      width_           = current_width;
      height_          = current_height;
      cell_dimensions_ = current_cells;
      if (dimensions_changed && !attach_canvas(width_, height_)) {
        return false;
      }
      if (!update_layout()) {
        return false;
      }
    }

    screen_too_small_ = width_ < InputFrame::kMinimumWidth ||
                        height_ < input_frame_->minimum_height();
    if (!poll_input()) {
      return false;
    }
    input_frame_->advance_time(InputFrame::Clock::now());
    screen_too_small_ = width_ < InputFrame::kMinimumWidth ||
                        height_ < input_frame_->minimum_height();
    if (!update_layout() ||
        !is_ok(embedded_terminal_->synchronize(width_, height_))) {
      return false;
    }
    screen_too_small_ = width_ < InputFrame::kMinimumWidth ||
                        height_ < input_frame_->minimum_height();
    if (!update_layout()) {
      return false;
    }

    const auto& description =
        screen_too_small_ ? small_layout_description_ : layout_description_;
    const Layout::AbsoluteLayout& absolute =
        screen_too_small_ ? small_absolute_layout_ : absolute_layout_;
    const Theme::Colors colors = theme_->get_colors();
    status = renderer_->start(description, absolute, *theme_, canvas_,
                              cell(U' ', colors.text, colors.background));
    if (!tui::is_ok(status)) {
      Logger<ERROR> << "Could not schedule input frame: "
                    << tui::status_message(status);
      return false;
    }
    status = renderer_->wait();
    if (!tui::is_ok(status)) {
      Logger<ERROR> << "Input frame rendering failed: "
                    << tui::status_message(status);
      return false;
    }
    status = screen_->draw();
    if (!tui::is_ok(status)) {
      Logger<ERROR> << "Could not present input frame: "
                    << tui::status_message(status);
      return false;
    }
    std::this_thread::sleep_for(kFrameDelay);
    return true;
  }

  bool shutdown() noexcept {
    bool quiesced = true;
    if (renderer_ != nullptr) {
      quiesced = tui::is_ok(renderer_->wait());
    }
    input_frame_.reset();
    terminal_          = nullptr;
    screen_            = nullptr;
    renderer_          = nullptr;
    command_mode_      = nullptr;
    embedded_terminal_ = nullptr;
    theme_             = nullptr;
    control_           = nullptr;
    return quiesced;
  }

 private:
  bool attach_canvas(std::size_t width, std::size_t height) {
    auto next = std::make_shared<Canvas>(width, height);
    if (!tui::is_ok(next->get_status())) {
      return false;
    }
    const tui::Status status = screen_->set_canvas(next);
    if (!tui::is_ok(status)) {
      return false;
    }
    canvas_ = std::move(next);
    return true;
  }

  bool setup_small_layout() {
    small_layout_description_ =
        layout_.make_layout_description("input-frame-too-small");
    const tui::Status status =
        layout_.add_frame(small_layout_description_, "screen-too-small",
                          std::make_shared<tui::MessageFrame>(
                              "screen-too-small", std::string{kScreenTooSmall}),
                          {});
    return tui::is_ok(status);
  }

  bool rebuild_input_layout(std::size_t input_height) {
    auto next = layout_.make_layout_description("input-frame-manual-test");
    const tui::Status status = layout_.add_frame(
        next, std::string{kInputFrameId}, input_frame_,
        {Layout::make_character_constraint(Layout::ConstraintType::MIN_WIDTH,
                                           InputFrame::kMinimumWidth),
         Layout::make_character_constraint(Layout::ConstraintType::MIN_HEIGHT,
                                           input_height),
         Layout::make_character_constraint(Layout::ConstraintType::MAX_HEIGHT,
                                           input_height),
         Layout::make_character_constraint(Layout::ConstraintType::LEFT_ANCHOR,
                                           0U),
         Layout::make_character_constraint(
             Layout::ConstraintType::BOTTOM_ANCHOR, 0U)});
    if (!tui::is_ok(status)) {
      return false;
    }
    layout_description_ = std::move(next);
    input_height_       = input_height;
    return true;
  }

  bool update_layout() {
    const std::size_t minimum_height = input_frame_->minimum_height();
    const bool can_fit =
        width_ >= InputFrame::kMinimumWidth && height_ >= minimum_height;
    const std::size_t desired_height =
        can_fit ? input_frame_->preferred_height(width_, height_)
                : minimum_height;
    if (layout_description_ == nullptr || desired_height != input_height_) {
      if (!rebuild_input_layout(desired_height)) {
        return false;
      }
    }
    tui::Status status =
        layout_.compute_absolute_layout(layout_description_, width_, height_,
                                        cell_dimensions_, absolute_layout_);
    if (!tui::is_ok(status)) {
      return false;
    }
    status = layout_.compute_absolute_layout(small_layout_description_, width_,
                                             height_, cell_dimensions_,
                                             small_absolute_layout_);
    return tui::is_ok(status);
  }

  bool poll_input() {
    bool end_of_input = false;
    if (!is_ok(terminal_->poll_input(std::chrono::milliseconds{0},
                                     end_of_input))) {
      Logger<ERROR> << "Could not poll normalized terminal input: "
                    << terminal::status_message(terminal_->terminal_status());
      return false;
    }
    if (end_of_input) {
      control_->request_exit();
    }

    std::vector<terminal::Event> events;
    const tui::Status input_status = screen_->drain_input_events(events);
    if (!tui::is_ok(input_status)) {
      Logger<ERROR> << "Could not consume normalized terminal input: "
                    << tui::status_message(input_status);
      return false;
    }
    try {
      for (const terminal::Event& event : events) {
        if (requests_exit(event)) {
          control_->request_exit();
        } else if (!screen_too_small_ && !route_event(event)) {
          return false;
        }
        refresh_selection_timeout();
      }
    } catch (...) {
      Logger<ERROR> << "Could not apply normalized terminal input";
      return false;
    }
    return resolve_selection_timeout();
  }

  bool route_event(const terminal::Event& event) {
    const tui::InputMode mode = input_frame_->snapshot().mode;
    if (mode == tui::InputMode::TERMINAL) {
      const auto* command = std::get_if<terminal::CommandEvent>(&event);
      const bool frame_command =
          command != nullptr &&
          (command->command == terminal::Command::ENTER_COMMAND_MODE ||
           command->command == terminal::Command::ENTER_TERMINAL_MODE);
      const bool selection_command =
          command != nullptr &&
          (command->command == terminal::Command::COPY ||
           command->command == terminal::Command::SELECT_ALL);
      if (!frame_command && !selection_command &&
          !std::holds_alternative<terminal::MouseEvent>(event) &&
          !std::holds_alternative<terminal::ScrollEvent>(event)) {
        return is_ok(embedded_terminal_->send_event(event));
      }
    }

    if (const auto* mouse = std::get_if<terminal::MouseEvent>(&event)) {
      const tui::Status status =
          screen_->handle_mouse_event(*mouse, layout_description_->z_buffer,
                                      absolute_layout_.frame_layouts);
      if (!tui::is_ok(status) && status != tui::Status::NO_SELECTION) {
        return false;
      }
    }

    if (const auto* command = std::get_if<terminal::CommandEvent>(&event)) {
      if (command->command == terminal::Command::COPY) {
        const tui::Status status = screen_->copy_selection();
        return tui::is_ok(status) || status == tui::Status::NO_SELECTION ||
               status == tui::Status::FRAME_NOT_SELECTABLE;
      }
      if (command->command == terminal::Command::SELECT_ALL) {
        const tui::Status status =
            screen_->select_all(kInputFrameId, input_frame_);
        return tui::is_ok(status) || status == tui::Status::NO_SELECTION ||
               status == tui::Status::FRAME_NOT_SELECTABLE;
      }
    }

    return tui::is_ok(
        command_mode_->handle_event(event, InputFrame::Clock::now()));
  }

  void refresh_selection_timeout() {
    selection_timeout_.synchronize(
        screen_->pending_selection_timeout(),
        terminal_->timeout_settings().multiple_click);
  }

  bool resolve_selection_timeout() {
    refresh_selection_timeout();
    const std::optional<terminal::TimeoutInput> due =
        selection_timeout_.take_if_due();
    if (!due.has_value()) {
      return true;
    }
    const tui::Status status = screen_->handle_selection_timeout(*due);
    refresh_selection_timeout();
    return tui::is_ok(status);
  }

  TerminalSubsystem* terminal_ = nullptr;
  std::shared_ptr<InputFrame> input_frame_;
  Screen* screen_                               = nullptr;
  ParallelRenderer* renderer_                   = nullptr;
  CommandModeSubsystem* command_mode_           = nullptr;
  EmbeddedTerminalSubsystem* embedded_terminal_ = nullptr;
  Theme* theme_                                 = nullptr;
  ApplicationControl* control_                  = nullptr;
  timer::TokenDeadline<terminal::TimeoutInput> selection_timeout_;
  std::shared_ptr<Canvas> canvas_;
  std::shared_ptr<Layout::LayoutDescription> layout_description_;
  std::shared_ptr<Layout::LayoutDescription> small_layout_description_;
  Layout::AbsoluteLayout absolute_layout_;
  Layout::AbsoluteLayout small_absolute_layout_;
  Layout layout_;
  CellDimensions cell_dimensions_ = tui::kDefaultCellDimensions;
  std::size_t width_              = 0U;
  std::size_t height_             = 0U;
  std::size_t input_height_       = 0U;
  bool screen_too_small_          = false;
};

}  // namespace

class InputTestRuntimeSubsystem::Impl final {
 public:
  std::shared_ptr<InputTestApplication> application;
};

InputTestRuntimeSubsystem::InputTestRuntimeSubsystem()
    : AppSubsystem(
          "input-test-runtime",
          subsystem_dependencies<ApplicationControlSubsystem, TerminalSubsystem,
                                 ScreenSubsystem, PresentationSubsystem,
                                 InputSubsystem, CommandModeSubsystem,
                                 EmbeddedTerminalSubsystem, ThemeSubsystem>()),
      impl_(std::make_unique<Impl>()) {}

InputTestRuntimeSubsystem::~InputTestRuntimeSubsystem() = default;

Status InputTestRuntimeSubsystem::initialize(AppState& app) {
  static_cast<void>(app);
  return impl_ == nullptr ? Status::SUBSYSTEM_FAILURE : Status::OK;
}

Status InputTestRuntimeSubsystem::start(AppState& app) {
  auto* control      = app.get_subsystem<ApplicationControlSubsystem>();
  auto* terminal     = app.get_subsystem<TerminalSubsystem>();
  auto* screen       = app.get_subsystem<ScreenSubsystem>();
  auto* presentation = app.get_subsystem<PresentationSubsystem>();
  auto* input        = app.get_subsystem<InputSubsystem>();
  auto* command_mode = app.get_subsystem<CommandModeSubsystem>();
  auto* embedded     = app.get_subsystem<EmbeddedTerminalSubsystem>();
  auto* theme        = app.get_subsystem<ThemeSubsystem>();
  if (control == nullptr || control->control() == nullptr ||
      terminal == nullptr || terminal->session() == nullptr ||
      terminal->decoder() == nullptr || screen == nullptr ||
      screen->screen() == nullptr || presentation == nullptr ||
      presentation->renderer() == nullptr || input == nullptr ||
      input->input_frame() == nullptr || command_mode == nullptr ||
      embedded == nullptr || theme == nullptr || theme->theme() == nullptr) {
    return Status::SUBSYSTEM_FAILURE;
  }

  auto application = std::make_shared<InputTestApplication>(
      *terminal, input->input_frame(), *screen->screen(),
      *presentation->renderer(), *command_mode, *embedded, *theme->theme(),
      *control->control());
  if (!application->setup()) {
    static_cast<void>(application->shutdown());
    return Status::SUBSYSTEM_FAILURE;
  }
  impl_->application = std::move(application);
  return Status::OK;
}

Status InputTestRuntimeSubsystem::stop(AppState& app) noexcept {
  static_cast<void>(app);
  if (impl_ == nullptr || impl_->application == nullptr) {
    return Status::OK;
  }
  const bool stopped = impl_->application->shutdown();
  impl_->application.reset();
  return stopped ? Status::OK : Status::SUBSYSTEM_FAILURE;
}

Status InputTestRuntimeSubsystem::terminate(AppState& app) noexcept {
  const Status status = stop(app);
  impl_.reset();
  return status;
}

bool InputTestRuntimeSubsystem::draw() {
  return impl_ != nullptr && impl_->application != nullptr &&
         impl_->application->draw();
}

}  // namespace puc::app
