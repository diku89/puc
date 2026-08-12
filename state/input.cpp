/**
 * @file input.cpp
 * @brief Composite input-frame subsystem implementation.
 */

#include "state/input.hpp"

#include <memory>
#include <string>
#include <utility>

#include "msgs/cmdframe_msgs.hpp"
#include "msgs/status.hpp"
#include "puc-cli/tui/frames/input_frame.hpp"
#include "state/channels.hpp"
#include "state/directory.hpp"
#include "utils/ipc/directory.hpp"
#include "utils/ipc/status.hpp"

namespace puc::app {

InputSubsystem::InputSubsystem()
    : AppSubsystem("input",
                   subsystem_dependencies<CommandNotificationChannelSubsystem,
                                          DirectorySubsystem>()) {}

InputSubsystem::~InputSubsystem() = default;

Status InputSubsystem::initialize(AppState& app) {
  static_cast<void>(app);
  input_frame_ = std::make_shared<tui::InputFrame>();
  return Status::OK;
}

Status InputSubsystem::start(AppState& app) {
  if (input_frame_ == nullptr) {
    return Status::SUBSYSTEM_FAILURE;
  }
  if (notification_subscription_.active()) {
    return Status::OK;
  }
  DirectorySubsystem* directory_subsystem =
      app.get_subsystem<DirectorySubsystem>();
  if (directory_subsystem == nullptr ||
      directory_subsystem->directory() == nullptr) {
    return Status::SUBSYSTEM_FAILURE;
  }

  const std::weak_ptr<tui::InputFrame> weak_frame = input_frame_;
  ipc::Subscription subscription;
  const ipc::Status status = directory_subsystem->directory()->subscribe(
      msg::kCmdFrameNotifyChannel,
      [weak_frame](ipc::Channel::Bytes payload) noexcept {
        try {
          msg::CmdFrameNotification notification;
          if (!msg::is_ok(msg::CmdFrameNotificationCodec{}.deserialize(
                  payload, notification))) {
            return;
          }
          if (const std::shared_ptr<tui::InputFrame> frame =
                  weak_frame.lock()) {
            frame->set_notification(std::move(notification.text));
          }
        } catch (...) {
          // Channel callbacks are a no-throw boundary. A malformed or
          // allocation-failing notification leaves the previous text intact.
        }
      },
      subscription);
  if (!ipc::is_ok(status)) {
    return Status::SUBSYSTEM_FAILURE;
  }
  notification_subscription_ = std::move(subscription);
  return Status::OK;
}

Status InputSubsystem::stop(AppState& app) noexcept {
  static_cast<void>(app);
  notification_subscription_.reset();
  return Status::OK;
}

Status InputSubsystem::terminate(AppState& app) noexcept {
  static_cast<void>(stop(app));
  input_frame_.reset();
  return Status::OK;
}

}  // namespace puc::app
