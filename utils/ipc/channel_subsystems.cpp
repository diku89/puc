/**
 * @file channel_subsystems.cpp
 * @brief Application protocol-channel subsystem implementations.
 */

#include "utils/ipc/channel_subsystems.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include "msgs/cmdframe_msgs.hpp"
#include "msgs/screen_msgs.hpp"
#include "msgs/terminal_msgs.hpp"
#include "utils/ipc/directory.hpp"
#include "utils/ipc/directory_subsystem.hpp"
#include "utils/ipc/smem_channel.hpp"
#include "utils/ipc/status.hpp"

namespace puc::app {
namespace {

/** Maximum Screen commands retained while terminal delivery is behind. */
constexpr std::size_t kScreenCommandDepth = 3U;

/** Geometry is state, so only the newest pending observation is useful. */
constexpr std::size_t kResizeEventDepth = 1U;

/** Only the newest command notification remains useful to the view. */
constexpr std::size_t kCommandNotificationDepth = 1U;

/** Treat an already absent route as successful idempotent teardown. */
Status close_route(ipc::Directory* directory, std::string_view name) noexcept {
  if (directory == nullptr) {
    return Status::OK;
  }
  try {
    const ipc::Status status = directory->close_channel(name);
    return ipc::is_ok(status) || status == ipc::Status::CHANNEL_NOT_FOUND
               ? Status::OK
               : Status::SUBSYSTEM_FAILURE;
  } catch (...) {
    return Status::SUBSYSTEM_FAILURE;
  }
}

}  // namespace

ScreenChannelSubsystem::ScreenChannelSubsystem()
    : AppSubsystem("screen-channels",
                   subsystem_dependencies<DirectorySubsystem>()) {}

Status ScreenChannelSubsystem::initialize(AppState& app) {
  return app.get_subsystem<DirectorySubsystem>() == nullptr
             ? Status::SUBSYSTEM_NOT_FOUND
             : Status::OK;
}

Status ScreenChannelSubsystem::start(AppState& app) {
  if (command_channel_ != nullptr && resize_channel_ != nullptr) {
    return Status::OK;
  }
  DirectorySubsystem* directory_subsystem =
      app.get_subsystem<DirectorySubsystem>();
  if (directory_subsystem == nullptr ||
      directory_subsystem->directory() == nullptr) {
    return Status::SUBSYSTEM_FAILURE;
  }
  directory_ = directory_subsystem->directory();
  const std::size_t maximum_message_bytes =
      directory_subsystem->maximum_message_bytes();
  if (maximum_message_bytes == 0U) {
    directory_ = nullptr;
    return Status::SUBSYSTEM_FAILURE;
  }

  auto command = std::make_shared<ipc::SmemChannel>(
      std::string{msg::kScreenCommandChannel}, maximum_message_bytes,
      ipc::ChannelOptions{.channel_max_depth = kScreenCommandDepth});
  ipc::Status ipc_status = directory_->open_channel(command, command_id_);
  if (!ipc::is_ok(ipc_status)) {
    directory_  = nullptr;
    command_id_ = 0U;
    return Status::SUBSYSTEM_FAILURE;
  }

  auto resize = std::make_shared<ipc::SmemChannel>(
      std::string{msg::kScreenResizeEventChannel}, 16U,
      ipc::ChannelOptions{.channel_max_depth = kResizeEventDepth});
  ipc_status = directory_->open_channel(resize, resize_id_);
  if (!ipc::is_ok(ipc_status)) {
    static_cast<void>(close_route(directory_, msg::kScreenCommandChannel));
    directory_  = nullptr;
    command_id_ = 0U;
    resize_id_  = 0U;
    return Status::SUBSYSTEM_FAILURE;
  }

  command_channel_ = std::move(command);
  resize_channel_  = std::move(resize);
  return Status::OK;
}

Status ScreenChannelSubsystem::stop(AppState& app) noexcept {
  static_cast<void>(app);
  return close_channels();
}

Status ScreenChannelSubsystem::terminate(AppState& app) noexcept {
  static_cast<void>(app);
  return close_channels();
}

Status ScreenChannelSubsystem::close_channels() noexcept {
  Status result = close_route(directory_, msg::kScreenCommandChannel);
  const Status resize_status =
      close_route(directory_, msg::kScreenResizeEventChannel);
  if (is_ok(result)) {
    result = resize_status;
  }
  command_channel_.reset();
  resize_channel_.reset();
  command_id_ = 0U;
  resize_id_  = 0U;
  directory_  = nullptr;
  return result;
}

TerminalInputChannelSubsystem::TerminalInputChannelSubsystem()
    : AppSubsystem("terminal-input-channel",
                   subsystem_dependencies<DirectorySubsystem>()) {}

Status TerminalInputChannelSubsystem::initialize(AppState& app) {
  return app.get_subsystem<DirectorySubsystem>() == nullptr
             ? Status::SUBSYSTEM_NOT_FOUND
             : Status::OK;
}

Status TerminalInputChannelSubsystem::start(AppState& app) {
  if (channel_ != nullptr) {
    return Status::OK;
  }
  DirectorySubsystem* directory_subsystem =
      app.get_subsystem<DirectorySubsystem>();
  if (directory_subsystem == nullptr ||
      directory_subsystem->directory() == nullptr) {
    return Status::SUBSYSTEM_FAILURE;
  }
  directory_ = directory_subsystem->directory();
  const std::size_t maximum_message_bytes =
      directory_subsystem->maximum_message_bytes();
  if (maximum_message_bytes == 0U) {
    directory_ = nullptr;
    return Status::SUBSYSTEM_FAILURE;
  }
  auto channel = std::make_shared<ipc::SmemChannel>(
      std::string{msg::kTerminalInputEventChannel}, maximum_message_bytes);
  const ipc::Status status = directory_->open_channel(channel, channel_id_);
  if (!ipc::is_ok(status)) {
    directory_  = nullptr;
    channel_id_ = 0U;
    return Status::SUBSYSTEM_FAILURE;
  }
  channel_ = std::move(channel);
  return Status::OK;
}

Status TerminalInputChannelSubsystem::stop(AppState& app) noexcept {
  static_cast<void>(app);
  return close_channel();
}

Status TerminalInputChannelSubsystem::terminate(AppState& app) noexcept {
  static_cast<void>(app);
  return close_channel();
}

Status TerminalInputChannelSubsystem::close_channel() noexcept {
  const Status result =
      close_route(directory_, msg::kTerminalInputEventChannel);
  channel_.reset();
  channel_id_ = 0U;
  directory_  = nullptr;
  return result;
}

CommandNotificationChannelSubsystem::CommandNotificationChannelSubsystem()
    : AppSubsystem("command-notification-channel",
                   subsystem_dependencies<DirectorySubsystem>()) {}

Status CommandNotificationChannelSubsystem::initialize(AppState& app) {
  return app.get_subsystem<DirectorySubsystem>() == nullptr
             ? Status::SUBSYSTEM_NOT_FOUND
             : Status::OK;
}

Status CommandNotificationChannelSubsystem::start(AppState& app) {
  if (channel_ != nullptr) {
    return Status::OK;
  }
  DirectorySubsystem* directory_subsystem =
      app.get_subsystem<DirectorySubsystem>();
  if (directory_subsystem == nullptr ||
      directory_subsystem->directory() == nullptr) {
    return Status::SUBSYSTEM_FAILURE;
  }
  directory_ = directory_subsystem->directory();
  const std::size_t maximum_message_bytes =
      directory_subsystem->maximum_message_bytes();
  if (maximum_message_bytes == 0U) {
    directory_ = nullptr;
    return Status::SUBSYSTEM_FAILURE;
  }
  auto channel = std::make_shared<ipc::SmemChannel>(
      std::string{msg::kCmdFrameNotifyChannel}, maximum_message_bytes,
      ipc::ChannelOptions{.channel_max_depth = kCommandNotificationDepth});
  const ipc::Status status = directory_->open_channel(channel, channel_id_);
  if (!ipc::is_ok(status)) {
    directory_  = nullptr;
    channel_id_ = 0U;
    return Status::SUBSYSTEM_FAILURE;
  }
  channel_ = std::move(channel);
  return Status::OK;
}

Status CommandNotificationChannelSubsystem::stop(AppState& app) noexcept {
  static_cast<void>(app);
  return close_channel();
}

Status CommandNotificationChannelSubsystem::terminate(AppState& app) noexcept {
  static_cast<void>(app);
  return close_channel();
}

Status CommandNotificationChannelSubsystem::close_channel() noexcept {
  const Status result = close_route(directory_, msg::kCmdFrameNotifyChannel);
  channel_.reset();
  channel_id_ = 0U;
  directory_  = nullptr;
  return result;
}

}  // namespace puc::app
