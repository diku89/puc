/**
 * @file subsystems_test.cpp
 * @brief Integration tests for concrete application subsystem adapters.
 */

#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "canvas/canvas_subsystem.hpp"
#include "canvas/datastore_subsystem.hpp"
#include "canvas/session/orchestration_subsystem.hpp"
#include "commands/builtin_command_subsystem.hpp"
#include "commands/command.hpp"
#include "commands/command_mode_subsystem.hpp"
#include "commands/command_subsystem.hpp"
#include "gtest/gtest.h"
#include "msgs/cmdframe_msgs.hpp"
#include "msgs/screen_msgs.hpp"
#include "msgs/terminal_msgs.hpp"
#include "properties/properties.hpp"
#include "properties/properties_subsystem.hpp"
#include "puc-cli/application/application_control_subsystem.hpp"
#include "puc-cli/application/bootstrap.hpp"
#include "puc-cli/tui/frames/input_frame.hpp"
#include "puc-cli/tui/frames/input_subsystem.hpp"
#include "puc-cli/tui/rendering/presentation_subsystem.hpp"
#include "puc-cli/tui/rendering/screen.hpp"
#include "puc-cli/tui/rendering/screen_subsystem.hpp"
#include "puc-cli/tui/rendering/theme.hpp"
#include "puc-cli/tui/terminal/embedded_terminal_subsystem.hpp"
#include "puc-cli/tui/terminal/event.hpp"
#include "puc-cli/tui/terminal/session.hpp"
#include "puc-cli/tui/terminal/terminal_subsystem.hpp"
#include "state/lifecycle.hpp"
#include "state/state.hpp"
#include "themes/theme_subsystem.hpp"
#include "utils/ipc/channel.hpp"
#include "utils/ipc/channel_subsystems.hpp"
#include "utils/ipc/directory.hpp"
#include "utils/ipc/directory_subsystem.hpp"
#include "utils/ipc/smem_channel.hpp"
#include "utils/ipc/status.hpp"
#include "utils/logger/logger.hpp"
#include "utils/logger/logger_subsystem.hpp"
#include "utils/metronome/metronome_subsystem.hpp"
#include "utils/multithreading/job_queue.hpp"
#include "utils/multithreading/worker_subsystem.hpp"
#include "utils/timer/timer_subsystem.hpp"

namespace puc::app {
namespace {

using namespace std::chrono_literals;

constexpr std::size_t kExpectedConfiguredMessageBytes = 1073741824U;

/** Isolated on-disk configuration for canonical Canvas bootstrap coverage. */
class TemporaryCanvasConfiguration final {
 public:
  TemporaryCanvasConfiguration() {
    root_ = std::filesystem::temp_directory_path() /
            ("puc-application-subsystem-test-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root_);
    database_ = root_ / "sessions.db";
    std::ofstream config{root_ / "canvas.toml"};
    config << "[database]\npath = \"" << database_.string() << "\"\n";
  }

  ~TemporaryCanvasConfiguration() { std::filesystem::remove_all(root_); }

  /** Return property roots that resolve the isolated Canvas configuration. */
  PropertiesSubsystemOptions properties() const {
    return PropertiesSubsystemOptions{
        .primary_root        = {},
        .user_overrides_root = root_,
    };
  }

  /** Return the database path expected to be created during initialization. */
  const std::filesystem::path& database() const noexcept { return database_; }

  /** Point SQLite at a directory so the canonical datastore cannot open it. */
  void make_database_unopenable() const {
    std::ofstream config{root_ / "canvas.toml", std::ios::trunc};
    config << "[database]\npath = \"" << root_.string() << "\"\n";
  }

 private:
  std::filesystem::path root_;     /**< Isolated configuration directory. */
  std::filesystem::path database_; /**< Isolated SQLite database path. */
};

/** Minimal registered command used to verify registry lifetime. */
class AdapterCommand final : public command::CommandApp {
 public:
  command::Status run(command::CommonCommandArgs,
                      std::span<const std::string>) override {
    return command::Status::OK;
  }

  std::string get_description() const override { return "Adapter command"; }

  std::string get_usage() const override { return "--verbose"; }
};

TEST(CoreAdaptersTest, ControlOwnsTerminationSignalsAcrossRunCycles) {
  AppState app;
  auto control = std::make_unique<ApplicationControlSubsystem>();
  ApplicationControlSubsystem* control_view = control.get();
  ASSERT_EQ(app.register_subsystem(std::move(control)), Status::OK);

  ASSERT_EQ(app.initialize(OperatingMode::TUI), Status::OK);
  ASSERT_NE(control_view->control(), nullptr);
  EXPECT_FALSE(control_view->exit_requested());
  ASSERT_EQ(std::raise(SIGTERM), 0);
  EXPECT_TRUE(control_view->exit_requested());

  ASSERT_EQ(app.start(), Status::OK);
  ASSERT_EQ(app.stop(), Status::OK);
  EXPECT_TRUE(control_view->exit_requested());
  ASSERT_EQ(app.start(), Status::OK);
  EXPECT_TRUE(control_view->exit_requested());
  EXPECT_EQ(app.terminate(), Status::OK);
  EXPECT_EQ(control_view->control(), nullptr);
}

TEST(CoreAdaptersTest, CreatesStopsAndRestartsDirectoryAboveWorkers) {
  AppState app;
  auto directory = std::make_unique<DirectorySubsystem>();
  DirectorySubsystem* directory_adapter = directory.get();
  auto workers                          = std::make_unique<WorkerSubsystem>(2U);
  WorkerSubsystem* worker_adapter       = workers.get();
  auto logger                           = std::make_unique<LoggerSubsystem>();
  LoggerSubsystem* logger_adapter       = logger.get();

  // Registering dependents first is valid: topology, not insertion order,
  // controls lifecycle execution.
  ASSERT_EQ(app.register_subsystem(std::move(directory)), Status::OK);
  ASSERT_EQ(app.register_subsystem(std::move(workers)), Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<PropertiesSubsystem>()),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::move(logger)), Status::OK);
  ASSERT_EQ(app.initialize(OperatingMode::TEST), Status::OK);
  ASSERT_NE(logger_adapter->logger(), nullptr);
  EXPECT_EQ(logger::get_logger(), logger_adapter->logger());
  EXPECT_EQ(worker_adapter->workers(), nullptr);
  EXPECT_EQ(directory_adapter->directory(), nullptr);
  EXPECT_EQ(directory_adapter->maximum_message_bytes(),
            kExpectedConfiguredMessageBytes);

  ASSERT_EQ(app.start(), Status::OK);
  ASSERT_NE(worker_adapter->workers(), nullptr);
  ASSERT_NE(directory_adapter->directory(), nullptr);
  EXPECT_TRUE(worker_adapter->workers()->active());
  EXPECT_EQ(worker_adapter->workers()->worker_count(), 2U);
  EXPECT_EQ(directory_adapter->directory()->delivery_worker_count(), 2U);

  auto channel = std::make_shared<ipc::SmemChannel>(
      "//test/adapters", directory_adapter->maximum_message_bytes());
  ipc::ChannelId channel_id = 0U;
  EXPECT_EQ(directory_adapter->directory()->open_channel(channel, channel_id),
            ipc::Status::OK);
  EXPECT_NE(channel_id, 0U);
  EXPECT_EQ(directory_adapter->directory()->size(), 1U);

  ASSERT_EQ(app.stop(), Status::OK);
  EXPECT_EQ(directory_adapter->directory(), nullptr);
  EXPECT_EQ(worker_adapter->workers(), nullptr);
  ASSERT_NE(logger_adapter->logger(), nullptr);
  EXPECT_EQ(logger::get_logger(), logger_adapter->logger());

  ASSERT_EQ(app.start(), Status::OK);
  ASSERT_NE(logger_adapter->logger(), nullptr);
  ASSERT_NE(worker_adapter->workers(), nullptr);
  ASSERT_NE(directory_adapter->directory(), nullptr);
  EXPECT_EQ(directory_adapter->directory()->size(), 0U);
  EXPECT_EQ(directory_adapter->directory()->delivery_worker_count(), 2U);

  EXPECT_EQ(app.terminate(), Status::OK);
  EXPECT_EQ(directory_adapter->directory(), nullptr);
  EXPECT_EQ(worker_adapter->workers(), nullptr);
  EXPECT_EQ(logger_adapter->logger(), nullptr);
  EXPECT_EQ(logger::get_logger(), nullptr);
}

TEST(CoreAdaptersTest, RejectsAZeroWorkerConfigurationDuringInitialization) {
  AppState app;
  auto workers                    = std::make_unique<WorkerSubsystem>(0U);
  WorkerSubsystem* worker_adapter = workers.get();
  ASSERT_EQ(app.register_subsystem(std::move(workers)), Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<LoggerSubsystem>()),
            Status::OK);

  EXPECT_EQ(app.initialize(OperatingMode::TEST), Status::INVALID_ARGUMENT);
  EXPECT_EQ(app.lifecycle_state(), LifecycleState::CRASHED);
  EXPECT_EQ(worker_adapter->workers(), nullptr);
}

TEST(CoreAdaptersTest, LoggerDoesNotClearANewerGlobalReplacement) {
  AppState app;
  auto logger                     = std::make_unique<LoggerSubsystem>();
  LoggerSubsystem* logger_adapter = logger.get();
  ASSERT_EQ(app.register_subsystem(std::move(logger)), Status::OK);
  ASSERT_EQ(app.initialize(OperatingMode::TEST), Status::OK);
  ASSERT_EQ(app.start(), Status::OK);
  ASSERT_NE(logger_adapter->logger(), nullptr);

  logger::init_logger(logger::LoggerConf{});
  const std::shared_ptr<logger::Logger> replacement = logger::get_logger();
  ASSERT_NE(replacement, nullptr);
  ASSERT_NE(replacement, logger_adapter->logger());

  EXPECT_EQ(app.stop(), Status::OK);
  EXPECT_EQ(logger::get_logger(), replacement);
  EXPECT_EQ(app.terminate(), Status::OK);
  EXPECT_EQ(logger::get_logger(), replacement);
  EXPECT_TRUE(logger::clear_logger(replacement));
}

TEST(CoreAdaptersTest, DirectoryRequiresARegisteredWorkerAdapter) {
  AppState app;
  ASSERT_EQ(app.register_subsystem(std::make_unique<DirectorySubsystem>()),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<PropertiesSubsystem>()),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<LoggerSubsystem>()),
            Status::OK);

  EXPECT_EQ(app.initialize(OperatingMode::TEST), Status::MISSING_DEPENDENCY);
  EXPECT_EQ(app.lifecycle_state(), LifecycleState::CRASHED);
}

TEST(ChannelAdaptersTest, OwnCanonicalRoutesForEachRunningGeneration) {
  AppState app;
  auto screen_channels = std::make_unique<ScreenChannelSubsystem>();
  ScreenChannelSubsystem* screen_adapter = screen_channels.get();
  auto terminal_input = std::make_unique<TerminalInputChannelSubsystem>();
  TerminalInputChannelSubsystem* terminal_input_adapter = terminal_input.get();
  auto command_notifications =
      std::make_unique<CommandNotificationChannelSubsystem>();
  CommandNotificationChannelSubsystem* command_adapter =
      command_notifications.get();
  auto directory = std::make_unique<DirectorySubsystem>();
  DirectorySubsystem* directory_adapter = directory.get();

  ASSERT_EQ(app.register_subsystem(std::move(screen_channels)), Status::OK);
  ASSERT_EQ(app.register_subsystem(std::move(terminal_input)), Status::OK);
  ASSERT_EQ(app.register_subsystem(std::move(command_notifications)),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::move(directory)), Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<WorkerSubsystem>(2U)),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<PropertiesSubsystem>()),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<LoggerSubsystem>()),
            Status::OK);
  ASSERT_EQ(app.initialize(OperatingMode::TEST), Status::OK);
  ASSERT_EQ(app.start(), Status::OK);

  ASSERT_NE(directory_adapter->directory(), nullptr);
  EXPECT_EQ(directory_adapter->directory()->size(), 4U);
  EXPECT_NE(screen_adapter->command_channel_id(), 0U);
  EXPECT_NE(screen_adapter->resize_channel_id(), 0U);
  EXPECT_NE(command_adapter->channel_id(), 0U);
  EXPECT_NE(terminal_input_adapter->channel_id(), 0U);
  ASSERT_NE(screen_adapter->command_channel(), nullptr);
  ASSERT_NE(screen_adapter->resize_channel(), nullptr);
  ASSERT_NE(command_adapter->channel(), nullptr);
  ASSERT_NE(terminal_input_adapter->channel(), nullptr);
  EXPECT_EQ(screen_adapter->command_channel()->channel_max_depth(),
            std::optional<std::size_t>{3U});
  EXPECT_EQ(screen_adapter->resize_channel()->channel_max_depth(),
            std::optional<std::size_t>{1U});
  EXPECT_EQ(command_adapter->channel()->channel_max_depth(),
            std::optional<std::size_t>{1U});
  EXPECT_EQ(directory_adapter->directory()
                ->get_channel(msg::kScreenCommandChannel)
                .get(),
            screen_adapter->command_channel());
  EXPECT_EQ(directory_adapter->directory()
                ->get_channel(msg::kScreenResizeEventChannel)
                .get(),
            screen_adapter->resize_channel());
  EXPECT_EQ(directory_adapter->directory()
                ->get_channel(msg::kCmdFrameNotifyChannel)
                .get(),
            command_adapter->channel());
  EXPECT_EQ(directory_adapter->directory()
                ->get_channel(msg::kTerminalInputEventChannel)
                .get(),
            terminal_input_adapter->channel());

  ASSERT_EQ(app.stop(), Status::OK);
  EXPECT_EQ(screen_adapter->command_channel(), nullptr);
  EXPECT_EQ(screen_adapter->resize_channel(), nullptr);
  EXPECT_EQ(command_adapter->channel(), nullptr);
  EXPECT_EQ(terminal_input_adapter->channel(), nullptr);
  EXPECT_EQ(directory_adapter->directory(), nullptr);

  ASSERT_EQ(app.start(), Status::OK);
  ASSERT_NE(directory_adapter->directory(), nullptr);
  EXPECT_EQ(directory_adapter->directory()->size(), 4U);
  EXPECT_EQ(app.terminate(), Status::OK);
}

TEST(TerminalAdapterTest, RetainsMechanismsAndRebindsAcrossRestarts) {
  AppState app;
  auto terminal                       = std::make_unique<TerminalSubsystem>();
  TerminalSubsystem* terminal_adapter = terminal.get();

  ASSERT_EQ(app.register_subsystem(std::move(terminal)), Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<ScreenChannelSubsystem>()),
            Status::OK);
  ASSERT_EQ(
      app.register_subsystem(std::make_unique<TerminalInputChannelSubsystem>()),
      Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<DirectorySubsystem>()),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<WorkerSubsystem>(2U)),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<PropertiesSubsystem>()),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<LoggerSubsystem>()),
            Status::OK);

  ASSERT_EQ(app.initialize(OperatingMode::TEST), Status::OK);
  ASSERT_NE(terminal_adapter->session(), nullptr);
  ASSERT_NE(terminal_adapter->decoder(), nullptr);
  EXPECT_FALSE(terminal_adapter->session()->screen_channels_bound());

  ASSERT_EQ(app.start(), Status::OK);
  EXPECT_TRUE(terminal_adapter->session()->screen_channels_bound());

  ASSERT_EQ(app.stop(), Status::OK);
  ASSERT_NE(terminal_adapter->session(), nullptr);
  ASSERT_NE(terminal_adapter->decoder(), nullptr);
  EXPECT_FALSE(terminal_adapter->session()->screen_channels_bound());

  ASSERT_EQ(app.start(), Status::OK);
  EXPECT_TRUE(terminal_adapter->session()->screen_channels_bound());
  ASSERT_EQ(app.terminate(), Status::OK);
  EXPECT_EQ(terminal_adapter->session(), nullptr);
  EXPECT_EQ(terminal_adapter->decoder(), nullptr);
}

TEST(ScreenAdapterTest, BorrowsLifecycleOwnedTerminalAndDirectory) {
  AppState app;
  auto screen                         = std::make_unique<ScreenSubsystem>();
  ScreenSubsystem* screen_adapter     = screen.get();
  auto terminal                       = std::make_unique<TerminalSubsystem>();
  TerminalSubsystem* terminal_adapter = terminal.get();
  auto directory                      = std::make_unique<DirectorySubsystem>();
  DirectorySubsystem* directory_adapter = directory.get();

  ASSERT_EQ(app.register_subsystem(std::move(screen)), Status::OK);
  ASSERT_EQ(app.register_subsystem(std::move(terminal)), Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<ScreenChannelSubsystem>()),
            Status::OK);
  ASSERT_EQ(
      app.register_subsystem(std::make_unique<TerminalInputChannelSubsystem>()),
      Status::OK);
  ASSERT_EQ(app.register_subsystem(std::move(directory)), Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<WorkerSubsystem>(2U)),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<PropertiesSubsystem>()),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<LoggerSubsystem>()),
            Status::OK);

  ASSERT_EQ(app.initialize(OperatingMode::TEST), Status::OK);
  EXPECT_EQ(screen_adapter->screen(), nullptr);
  ASSERT_EQ(app.start(), Status::OK);
  ASSERT_NE(screen_adapter->screen(), nullptr);
  EXPECT_EQ(screen_adapter->screen_status(), tui::Status::OK);
  EXPECT_EQ(&screen_adapter->screen()->ipc_directory(),
            directory_adapter->directory());
  ASSERT_NE(terminal_adapter->session(), nullptr);
  EXPECT_TRUE(terminal_adapter->session()->screen_channels_bound());
  const std::shared_ptr<ipc::Channel> input_channel =
      directory_adapter->directory()->get_channel(
          msg::kTerminalInputEventChannel);
  ASSERT_NE(input_channel, nullptr);
  EXPECT_EQ(input_channel->subscriber_count(), 1U);

  const msg::TerminalInputEvent message{
      .data = msg::TerminalTextEvent{.utf8 = "lifecycle input"}};
  std::vector<std::uint8_t> payload;
  ASSERT_EQ(msg::TerminalInputEventCodec{}.serialize(message, payload),
            msg::Status::OK);
  const ipc::TransferResult transfer = directory_adapter->directory()->transmit(
      msg::kTerminalInputEventChannel, payload);
  ASSERT_EQ(transfer.status, ipc::Status::OK);
  ASSERT_EQ(transfer.bytes, payload.size());
  std::vector<terminal::Event> events;
  ASSERT_EQ(screen_adapter->screen()->drain_input_events(events),
            tui::Status::OK);
  ASSERT_EQ(events.size(), 1U);
  ASSERT_TRUE(std::holds_alternative<terminal::TextEvent>(events.front()));
  EXPECT_EQ(std::get<terminal::TextEvent>(events.front()).utf8,
            "lifecycle input");

  ASSERT_EQ(app.stop(), Status::OK);
  EXPECT_EQ(screen_adapter->screen(), nullptr);
  EXPECT_FALSE(terminal_adapter->session()->screen_channels_bound());
  EXPECT_EQ(input_channel->subscriber_count(), 0U);
  ASSERT_EQ(app.start(), Status::OK);
  EXPECT_NE(screen_adapter->screen(), nullptr);
  EXPECT_EQ(app.terminate(), Status::OK);
  EXPECT_EQ(screen_adapter->screen(), nullptr);
}

TEST(CommandAdapterTest, RetainsRegistryAndResolvesCurrentBorrowedServices) {
  AppState app;
  auto commands                     = std::make_unique<CommandSubsystem>();
  CommandSubsystem* command_adapter = commands.get();
  auto control = std::make_unique<ApplicationControlSubsystem>();
  ApplicationControlSubsystem* control_adapter = control.get();
  auto directory = std::make_unique<DirectorySubsystem>();
  DirectorySubsystem* directory_adapter = directory.get();
  auto workers                          = std::make_unique<WorkerSubsystem>(2U);
  WorkerSubsystem* worker_adapter       = workers.get();
  auto screen                           = std::make_unique<ScreenSubsystem>();
  ScreenSubsystem* screen_adapter       = screen.get();

  ASSERT_EQ(app.register_subsystem(std::move(commands)), Status::OK);
  ASSERT_EQ(app.register_subsystem(std::move(control)), Status::OK);
  ASSERT_EQ(app.register_subsystem(
                std::make_unique<CommandNotificationChannelSubsystem>()),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::move(screen)), Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<TerminalSubsystem>()),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<ScreenChannelSubsystem>()),
            Status::OK);
  ASSERT_EQ(
      app.register_subsystem(std::make_unique<TerminalInputChannelSubsystem>()),
      Status::OK);
  ASSERT_EQ(app.register_subsystem(std::move(directory)), Status::OK);
  ASSERT_EQ(app.register_subsystem(std::move(workers)), Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<PropertiesSubsystem>()),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<LoggerSubsystem>()),
            Status::OK);

  ASSERT_EQ(app.initialize(OperatingMode::TEST), Status::OK);
  ASSERT_NE(command_adapter->dispatcher(), nullptr);
  ASSERT_EQ(command_adapter->dispatcher()->register_command(
                "adapter", {}, std::make_shared<AdapterCommand>()),
            command::Status::OK);
  ASSERT_EQ(app.start(), Status::OK);

  const command::CommonCommandArgs args = command_adapter->common_args(app);
  EXPECT_EQ(args.workers, worker_adapter->workers());
  EXPECT_EQ(args.directory, directory_adapter->directory());
  EXPECT_EQ(args.screen, screen_adapter->screen());
  EXPECT_EQ(args.control, control_adapter->control());
  EXPECT_NE(args.properties, nullptr);
  EXPECT_EQ(args.state, &app);

  ASSERT_EQ(app.stop(), Status::OK);
  ASSERT_NE(command_adapter->dispatcher(), nullptr);
  EXPECT_TRUE(command_adapter->dispatcher()->contains("adapter"));
  ASSERT_EQ(app.start(), Status::OK);
  EXPECT_TRUE(command_adapter->dispatcher()->contains("adapter"));
  ASSERT_EQ(app.terminate(), Status::OK);
  EXPECT_EQ(command_adapter->dispatcher(), nullptr);
}

TEST(InputAdapterTest, ConsumesTypedNotificationsAcrossRestarts) {
  AppState app;
  auto input                        = std::make_unique<InputSubsystem>();
  InputSubsystem* input_adapter     = input.get();
  auto commands                     = std::make_unique<CommandSubsystem>();
  CommandSubsystem* command_adapter = commands.get();
  auto control = std::make_unique<ApplicationControlSubsystem>();
  auto screen  = std::make_unique<ScreenSubsystem>();
  ScreenSubsystem* screen_adapter = screen.get();

  ASSERT_EQ(app.register_subsystem(std::move(input)), Status::OK);
  ASSERT_EQ(app.register_subsystem(std::move(commands)), Status::OK);
  ASSERT_EQ(app.register_subsystem(std::move(control)), Status::OK);
  ASSERT_EQ(app.register_subsystem(
                std::make_unique<CommandNotificationChannelSubsystem>()),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::move(screen)), Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<TerminalSubsystem>()),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<ScreenChannelSubsystem>()),
            Status::OK);
  ASSERT_EQ(
      app.register_subsystem(std::make_unique<TerminalInputChannelSubsystem>()),
      Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<DirectorySubsystem>()),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<WorkerSubsystem>(2U)),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<PropertiesSubsystem>()),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<LoggerSubsystem>()),
            Status::OK);

  ASSERT_EQ(app.initialize(OperatingMode::TEST), Status::OK);
  std::shared_ptr<tui::InputFrame> frame = input_adapter->input_frame();
  ASSERT_NE(frame, nullptr);
  EXPECT_FALSE(input_adapter->notification_consumer_active());
  ASSERT_EQ(app.start(), Status::OK);
  EXPECT_TRUE(input_adapter->notification_consumer_active());

  command::CommonCommandArgs args = command_adapter->common_args(app);
  ASSERT_NE(screen_adapter->screen(), nullptr);
  EXPECT_EQ(args.screen, screen_adapter->screen());
  ASSERT_EQ(command::send_notification(args, "adapter notification ✓"),
            command::Status::OK);
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (frame->snapshot().notification != "adapter notification ✓" &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  EXPECT_EQ(frame->snapshot().notification, "adapter notification ✓");

  ASSERT_EQ(app.stop(), Status::OK);
  EXPECT_FALSE(input_adapter->notification_consumer_active());
  EXPECT_EQ(input_adapter->input_frame(), frame);
  ASSERT_EQ(app.start(), Status::OK);
  EXPECT_TRUE(input_adapter->notification_consumer_active());
  ASSERT_EQ(command::send_notification(command_adapter->common_args(app),
                                       "second generation"),
            command::Status::OK);
  const auto restart_deadline = std::chrono::steady_clock::now() + 2s;
  while (frame->snapshot().notification != "second generation" &&
         std::chrono::steady_clock::now() < restart_deadline) {
    std::this_thread::sleep_for(1ms);
  }
  EXPECT_EQ(frame->snapshot().notification, "second generation");

  frame.reset();
  ASSERT_EQ(app.terminate(), Status::OK);
  EXPECT_EQ(input_adapter->input_frame(), nullptr);
}

TEST(CommandModeAdapterTest,
     CompletesDispatchesAndAcknowledgesAcrossARestartCycle) {
  AppState app;
  ApplicationSubsystemOptions options;
  options.worker_count                = 2U;
  options.selection.canvas            = false;
  options.selection.metronome         = false;
  options.selection.embedded_terminal = false;
  ASSERT_EQ(register_application_subsystems(app, std::move(options)),
            Status::OK);
  ASSERT_EQ(app.initialize(OperatingMode::TEST), Status::OK);

  CommandSubsystem* commands = app.get_subsystem<CommandSubsystem>();
  CommandModeSubsystem* command_mode =
      app.get_subsystem<CommandModeSubsystem>();
  InputSubsystem* input = app.get_subsystem<InputSubsystem>();
  ASSERT_NE(commands, nullptr);
  ASSERT_NE(command_mode, nullptr);
  ASSERT_NE(input, nullptr);
  const std::shared_ptr<AdapterCommand> adapter_command =
      std::make_shared<AdapterCommand>();
  ASSERT_EQ(
      commands->dispatcher()->register_command("status", {}, adapter_command),
      command::Status::OK);
  ASSERT_EQ(
      commands->dispatcher()->register_command("start", {}, adapter_command),
      command::Status::OK);
  ASSERT_EQ(app.start(), Status::OK);

  ASSERT_EQ(command_mode->handle_event(terminal::Event{terminal::CommandEvent{
                .command = terminal::Command::ENTER_COMMAND_MODE}}),
            tui::Status::OK);
  CommandModeSnapshot snapshot = command_mode->snapshot();
  std::vector<std::string> command_names;
  command_names.reserve(snapshot.completions.size());
  for (const CommandCompletion& completion : snapshot.completions) {
    command_names.push_back(completion.command);
  }
  EXPECT_EQ(command_names,
            (std::vector<std::string>{"config", "exit", "q", "quit", "start",
                                      "status"}));

  ASSERT_EQ(command_mode->handle_event(
                terminal::Event{terminal::TextEvent{.utf8 = "e"}}),
            tui::Status::OK);
  snapshot = command_mode->snapshot();
  ASSERT_EQ(snapshot.completions.size(), 1U);
  EXPECT_EQ(snapshot.completions.front().command, "exit");
  EXPECT_EQ(input->input_frame()->snapshot().command_help,
            (std::vector<std::string>{"> exit        Quit puc."}));

  ASSERT_EQ(command_mode->handle_event(terminal::Event{terminal::KeyEvent{
                .key = terminal::KeyCode{terminal::NamedKey::BACKSPACE}}}),
            tui::Status::OK);
  ASSERT_EQ(command_mode->handle_event(
                terminal::Event{terminal::TextEvent{.utf8 = "qu"}}),
            tui::Status::OK);
  snapshot = command_mode->snapshot();
  ASSERT_EQ(snapshot.completions.size(), 1U);
  EXPECT_EQ(snapshot.completions.front().command, "quit");
  EXPECT_EQ(input->input_frame()->snapshot().command_help,
            (std::vector<std::string>{"> quit        Quit puc."}));

  for (std::size_t index = 0U; index < 2U; ++index) {
    ASSERT_EQ(command_mode->handle_event(terminal::Event{terminal::KeyEvent{
                  .key = terminal::KeyCode{terminal::NamedKey::BACKSPACE}}}),
              tui::Status::OK);
  }
  ASSERT_EQ(command_mode->handle_event(
                terminal::Event{terminal::TextEvent{.utf8 = "st"}}),
            tui::Status::OK);
  snapshot = command_mode->snapshot();
  ASSERT_EQ(snapshot.completions.size(), 2U);
  EXPECT_EQ(input->input_frame()->snapshot().command_help.size(), 2U);

  ASSERT_EQ(command_mode->handle_event(terminal::Event{terminal::KeyEvent{
                .key = terminal::KeyCode{terminal::NamedKey::DOWN}}}),
            tui::Status::OK);
  snapshot = command_mode->snapshot();
  ASSERT_LT(snapshot.selected_completion, snapshot.completions.size());
  const std::string selected =
      snapshot.completions[snapshot.selected_completion].command;
  ASSERT_EQ(command_mode->handle_event(terminal::Event{terminal::KeyEvent{
                .key = terminal::KeyCode{terminal::NamedKey::TAB}}}),
            tui::Status::OK);
  EXPECT_EQ(input->input_frame()->snapshot().command_text, selected + " ");
  EXPECT_EQ(command_mode->snapshot().usage, "--verbose");

  ASSERT_EQ(command_mode->handle_event(terminal::Event{terminal::KeyEvent{
                .key = terminal::KeyCode{terminal::NamedKey::ENTER}}}),
            tui::Status::OK);
  EXPECT_TRUE(command_mode->snapshot().waiting_for_acknowledgement);
  EXPECT_EQ(input->input_frame()->snapshot().mode, tui::InputMode::COMMAND);

  ASSERT_EQ(app.stop(), Status::OK);
  EXPECT_FALSE(command_mode->snapshot().active);
  EXPECT_TRUE(command_mode->snapshot().waiting_for_acknowledgement);
  ASSERT_EQ(app.start(), Status::OK);
  EXPECT_TRUE(command_mode->snapshot().active);
  EXPECT_TRUE(command_mode->snapshot().waiting_for_acknowledgement);
  EXPECT_EQ(input->input_frame()->snapshot().mode, tui::InputMode::COMMAND);
  ASSERT_EQ(command_mode->handle_event(terminal::Event{terminal::KeyEvent{
                .key = terminal::KeyCode{terminal::NamedKey::TAB}}}),
            tui::Status::OK);
  EXPECT_EQ(input->input_frame()->snapshot().mode, tui::InputMode::NORMAL);
  EXPECT_FALSE(command_mode->snapshot().waiting_for_acknowledgement);

  EXPECT_EQ(app.terminate(), Status::OK);
}

TEST(EmbeddedTerminalAdapterTest, ReapsAndRecreatesItsChildAcrossRunCycles) {
  AppState app;
  ApplicationSubsystemOptions options;
  options.worker_count            = 2U;
  options.embedded_terminal.shell = "/bin/sh";
  options.selection               = ApplicationSubsystemSelection{
      .canvas            = false,
      .metronome         = false,
      .presentation      = false,
      .commands          = false,
      .input             = true,
      .command_mode      = false,
      .embedded_terminal = true,
  };
  ASSERT_EQ(register_application_subsystems(app, std::move(options)),
            Status::OK);
  ASSERT_EQ(app.initialize(OperatingMode::TEST), Status::OK);
  ASSERT_EQ(app.start(), Status::OK);

  EmbeddedTerminalSubsystem* embedded =
      app.get_subsystem<EmbeddedTerminalSubsystem>();
  InputSubsystem* input = app.get_subsystem<InputSubsystem>();
  ASSERT_NE(embedded, nullptr);
  ASSERT_NE(input, nullptr);
  ASSERT_EQ(
      input->input_frame()->handle_event(terminal::Event{terminal::CommandEvent{
          .command = terminal::Command::ENTER_TERMINAL_MODE}}),
      tui::Status::OK);
  const std::size_t generation =
      input->input_frame()->snapshot().terminal_generation;
  ASSERT_EQ(embedded->synchronize(80U, 24U), Status::OK);
  EXPECT_TRUE(embedded->child_running());
  EXPECT_EQ(embedded->child_generation(), generation);

  ASSERT_EQ(app.stop(), Status::OK);
  EXPECT_FALSE(embedded->child_running());
  ASSERT_EQ(app.start(), Status::OK);
  ASSERT_EQ(embedded->synchronize(80U, 24U), Status::OK);
  EXPECT_TRUE(embedded->child_running());
  EXPECT_EQ(embedded->child_generation(), generation);

  ASSERT_EQ(app.terminate(), Status::OK);
  EXPECT_FALSE(embedded->child_running());
}

TEST(AdapterBootstrapTest, RegistersAndRunsTheCompleteCanonicalGraph) {
  TemporaryCanvasConfiguration configuration;
  AppState app;
  ApplicationSubsystemOptions options;
  options.worker_count = 2U;
  options.properties   = configuration.properties();
  ASSERT_EQ(register_application_subsystems(app, std::move(options)),
            Status::OK);
  EXPECT_EQ(app.size(), kApplicationSubsystemCount);
  EXPECT_EQ(register_application_subsystems(app), Status::INVALID_ARGUMENT);

  EXPECT_NE(app.get_subsystem<LoggerSubsystem>(), nullptr);
  EXPECT_NE(app.get_subsystem<ApplicationControlSubsystem>(), nullptr);
  EXPECT_NE(app.get_subsystem<PropertiesSubsystem>(), nullptr);
  EXPECT_NE(app.get_subsystem<DatastoreSubsystem>(), nullptr);
  EXPECT_NE(app.get_subsystem<CanvasSubsystem>(), nullptr);
  EXPECT_NE(app.get_subsystem<OrchestrationSubsystem>(), nullptr);
  EXPECT_NE(app.get_subsystem<WorkerSubsystem>(), nullptr);
  EXPECT_NE(app.get_subsystem<TimerSubsystem>(), nullptr);
  EXPECT_NE(app.get_subsystem<ThemeSubsystem>(), nullptr);
  EXPECT_NE(app.get_subsystem<DirectorySubsystem>(), nullptr);
  EXPECT_NE(app.get_subsystem<ScreenChannelSubsystem>(), nullptr);
  EXPECT_NE(app.get_subsystem<TerminalInputChannelSubsystem>(), nullptr);
  EXPECT_NE(app.get_subsystem<CommandNotificationChannelSubsystem>(), nullptr);
  EXPECT_NE(app.get_subsystem<TerminalSubsystem>(), nullptr);
  EXPECT_NE(app.get_subsystem<ScreenSubsystem>(), nullptr);
  EXPECT_NE(app.get_subsystem<CommandSubsystem>(), nullptr);
  EXPECT_NE(app.get_subsystem<BuiltinCommandSubsystem>(), nullptr);
  EXPECT_NE(app.get_subsystem<InputSubsystem>(), nullptr);
  EXPECT_NE(app.get_subsystem<MetronomeSubsystem>(), nullptr);
  EXPECT_NE(app.get_subsystem<PresentationSubsystem>(), nullptr);
  EXPECT_NE(app.get_subsystem<CommandModeSubsystem>(), nullptr);
  EXPECT_NE(app.get_subsystem<EmbeddedTerminalSubsystem>(), nullptr);

  ASSERT_EQ(app.initialize(OperatingMode::TEST), Status::OK);
  EXPECT_TRUE(std::filesystem::is_regular_file(configuration.database()));
  ApplicationControlSubsystem* control =
      app.get_subsystem<ApplicationControlSubsystem>();
  CommandSubsystem* commands = app.get_subsystem<CommandSubsystem>();
  PropertiesSubsystem* properties_adapter =
      app.get_subsystem<PropertiesSubsystem>();
  ASSERT_NE(control, nullptr);
  ASSERT_NE(commands, nullptr);
  ASSERT_NE(properties_adapter, nullptr);
  ASSERT_NE(control->control(), nullptr);
  ASSERT_NE(commands->dispatcher(), nullptr);
  EXPECT_TRUE(commands->dispatcher()->contains("quit"));
  EXPECT_TRUE(commands->dispatcher()->contains("q"));
  EXPECT_TRUE(commands->dispatcher()->contains("exit"));
  EXPECT_TRUE(commands->dispatcher()->contains("config"));
  ASSERT_NE(properties_adapter->properties(), nullptr);
  ASSERT_NE(app.get_subsystem<ThemeSubsystem>()->theme(), nullptr);
  EXPECT_EQ(app.get_subsystem<TimerSubsystem>()->scheduler(), nullptr);
  properties::Properties* properties_generation =
      properties_adapter->properties();
  ASSERT_EQ(app.start(), Status::OK);
  const DirectorySubsystem* directory = app.get_subsystem<DirectorySubsystem>();
  const CanvasSubsystem* canvas       = app.get_subsystem<CanvasSubsystem>();
  ASSERT_NE(directory, nullptr);
  ASSERT_NE(directory->directory(), nullptr);
  ASSERT_NE(canvas, nullptr);
  EXPECT_EQ(directory->directory()->size(), 10U);
  EXPECT_NE(directory->directory()->get_channel(
                CanvasSubsystem::kChannelsAnnounceChannel),
            nullptr);
  EXPECT_NE(directory->directory()->get_channel(
                CanvasSubsystem::kChannelsQueryChannel),
            nullptr);
  EXPECT_NE(directory->directory()->get_channel(
                canvas->turn_submission_channel_name()),
            nullptr);
  EXPECT_NE(directory->directory()->get_channel(
                canvas->committed_turn_channel_name()),
            nullptr);
  EXPECT_NE(directory->directory()->get_channel(
                canvas->committed_presentation_channel_name()),
            nullptr);
  EXPECT_TRUE(
      app.get_subsystem<InputSubsystem>()->notification_consumer_active());
  EXPECT_NE(app.get_subsystem<TimerSubsystem>()->scheduler(), nullptr);
  EXPECT_NE(app.get_subsystem<PresentationSubsystem>()->renderer(), nullptr);
  EXPECT_NE(app.get_subsystem<MetronomeSubsystem>()->metronome(), nullptr);
  EXPECT_TRUE(app.get_subsystem<CommandModeSubsystem>()->snapshot().active);
  EXPECT_FALSE(app.get_subsystem<EmbeddedTerminalSubsystem>()->child_running());
  ASSERT_EQ(
      properties_adapter->properties()->set("theme.colors.primary", "0x123456"),
      properties::Status::OK);

  EXPECT_EQ(
      commands->dispatcher()->dispatch("q", commands->common_args(app), {}),
      command::Status::OK);
  EXPECT_TRUE(control->exit_requested());

  ASSERT_EQ(app.stop(), Status::OK);
  EXPECT_TRUE(control->exit_requested());
  EXPECT_EQ(properties_adapter->properties(), properties_generation);
  EXPECT_EQ(app.get_subsystem<PresentationSubsystem>()->renderer(), nullptr);
  EXPECT_EQ(app.get_subsystem<TimerSubsystem>()->scheduler(), nullptr);
  EXPECT_EQ(app.get_subsystem<MetronomeSubsystem>()->metronome(), nullptr);
  EXPECT_FALSE(app.get_subsystem<CommandModeSubsystem>()->snapshot().active);
  ASSERT_EQ(app.start(), Status::OK);
  EXPECT_TRUE(control->exit_requested());
  EXPECT_EQ(properties_adapter->properties(), properties_generation);
  EXPECT_NE(app.get_subsystem<PresentationSubsystem>()->renderer(), nullptr);
  EXPECT_NE(app.get_subsystem<TimerSubsystem>()->scheduler(), nullptr);
  EXPECT_NE(app.get_subsystem<MetronomeSubsystem>()->metronome(), nullptr);
  EXPECT_EQ(app.get_subsystem<ThemeSubsystem>()->theme()->get_colors().primary,
            0x123456U);
  EXPECT_EQ(app.terminate(), Status::OK);
  EXPECT_EQ(control->control(), nullptr);
  EXPECT_EQ(properties_adapter->properties(), nullptr);
  EXPECT_EQ(app.get_subsystem<DatastoreSubsystem>()->database(), nullptr);
  EXPECT_EQ(app.lifecycle_state(), LifecycleState::TERMINATED);
}

TEST(AdapterBootstrapTest, RegistersExecutableSpecificLeafProfiles) {
  AppState app;
  ApplicationSubsystemOptions options;
  options.worker_count = 2U;
  options.selection    = ApplicationSubsystemSelection{
      .canvas            = false,
      .metronome         = false,
      .presentation      = true,
      .commands          = false,
      .input             = false,
      .command_mode      = false,
      .embedded_terminal = false,
  };
  const std::size_t expected = application_subsystem_count(options.selection);
  ASSERT_EQ(register_application_subsystems(app, std::move(options)),
            Status::OK);
  EXPECT_EQ(app.size(), expected);
  EXPECT_NE(app.get_subsystem<PresentationSubsystem>(), nullptr);
  EXPECT_NE(app.get_subsystem<ApplicationControlSubsystem>(), nullptr);
  EXPECT_EQ(app.get_subsystem<MetronomeSubsystem>(), nullptr);
  EXPECT_EQ(app.get_subsystem<CommandSubsystem>(), nullptr);
  EXPECT_EQ(app.get_subsystem<BuiltinCommandSubsystem>(), nullptr);
  EXPECT_EQ(app.get_subsystem<InputSubsystem>(), nullptr);
  EXPECT_EQ(app.get_subsystem<CommandModeSubsystem>(), nullptr);
  EXPECT_EQ(app.get_subsystem<EmbeddedTerminalSubsystem>(), nullptr);
  EXPECT_EQ(app.get_subsystem<DatastoreSubsystem>(), nullptr);
  ASSERT_EQ(app.initialize(OperatingMode::TEST), Status::OK);
  ASSERT_EQ(app.start(), Status::OK);
  EXPECT_EQ(app.terminate(), Status::OK);
}

TEST(AdapterBootstrapTest, DatastoreFailureAbortsCanonicalInitialization) {
  TemporaryCanvasConfiguration configuration;
  configuration.make_database_unopenable();
  AppState app;
  ApplicationSubsystemOptions options;
  options.properties = configuration.properties();

  ASSERT_EQ(register_application_subsystems(app, std::move(options)),
            Status::OK);
  EXPECT_EQ(app.initialize(OperatingMode::TEST), Status::SUBSYSTEM_FAILURE);
  EXPECT_EQ(app.lifecycle_state(), LifecycleState::CRASHED);
  EXPECT_EQ(app.get_subsystem<DatastoreSubsystem>()->database(), nullptr);
}

TEST(AdapterBootstrapTest, RejectsProfilesWithMissingLeafDependencies) {
  {
    AppState app;
    ApplicationSubsystemOptions options;
    options.selection.commands = false;
    EXPECT_EQ(register_application_subsystems(app, std::move(options)),
              Status::INVALID_ARGUMENT);
    EXPECT_EQ(app.size(), 0U);
  }
  {
    AppState app;
    ApplicationSubsystemOptions options;
    options.selection.input        = false;
    options.selection.command_mode = false;
    EXPECT_EQ(register_application_subsystems(app, std::move(options)),
              Status::INVALID_ARGUMENT);
    EXPECT_EQ(app.size(), 0U);
  }
}

}  // namespace
}  // namespace puc::app
