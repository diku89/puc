#pragma once

/**
 * @file input.hpp
 * @brief Lifecycle adapter for the composite application input frame.
 */

#include <memory>

#include "puc-cli/state/state.hpp"
#include "utils/ipc/channel.hpp"

namespace puc::tui {
class InputFrame;
}

namespace puc::app {

/**
 * Own InputFrame and its command-notification consumer subscription.
 *
 * The frame is retained across stop/start so draft input survives a temporary
 * application pause. Each running generation receives a fresh subscription to
 * CommandNotificationChannelSubsystem. Presentation code may retain shared
 * frame ownership for render jobs, but must quiesce those jobs before this
 * subsystem terminates.
 */
class InputSubsystem final : public AppSubsystem {
 public:
  /** Declare command and Screen presentation dependencies. */
  InputSubsystem();

  /** Destroy the released frame and inactive subscription. */
  ~InputSubsystem() override;

  /** Construct the composite InputFrame. */
  Status initialize(AppState& app) override;

  /** Subscribe the frame to typed command notifications. */
  Status start(AppState& app) override;

  /** Disable command notifications while retaining editor state. */
  Status stop(AppState& app) noexcept override;

  /** Disable delivery and release the complete input-frame mechanism. */
  Status terminate(AppState& app) noexcept override;

  /** Return shared ownership for layouts and asynchronous render jobs. */
  std::shared_ptr<tui::InputFrame> input_frame() const noexcept {
    return input_frame_;
  }

  /** Return whether the notification consumer is currently active. */
  bool notification_consumer_active() const noexcept {
    return notification_subscription_.active();
  }

 private:
  std::shared_ptr<tui::InputFrame>
      input_frame_; /**< Editor retained while the app is initialized. */
  ipc::Subscription
      notification_subscription_; /**< Running-generation consumer handle. */
};

}  // namespace puc::app
