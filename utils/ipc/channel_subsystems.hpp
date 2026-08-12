#pragma once

/**
 * @file channel_subsystems.hpp
 * @brief Lifecycle-owned producers for application protocol channels.
 */

#include <cstdint>
#include <memory>

#include "state/state.hpp"
#include "utils/ipc/channel.hpp"

namespace puc::ipc {
class Directory;
}

namespace puc::app {

/**
 * Register the command and resize channels shared by Screen and the terminal.
 *
 * This adapter is the server/producer owner for both canonical channel names.
 * Terminal and presentation consumers depend on it and own only their
 * subscriptions. Stopping the adapter closes both routes before Directory is
 * destroyed.
 */
class ScreenChannelSubsystem final : public AppSubsystem {
 public:
  /** Declare the channel-directory dependency. */
  ScreenChannelSubsystem();

  /** Validate access to the registered DirectorySubsystem. */
  Status initialize(AppState& app) override;

  /** Create and register both canonical Screen channels. */
  Status start(AppState& app) override;

  /** Close both canonical routes and release their endpoints. */
  Status stop(AppState& app) noexcept override;

  /** Release any endpoints retained after partial lifecycle progress. */
  Status terminate(AppState& app) noexcept override;

  /** Return the Screen command endpoint, or nullptr while stopped. */
  ipc::Channel* command_channel() noexcept { return command_channel_.get(); }

  /** Return the terminal resize endpoint, or nullptr while stopped. */
  ipc::Channel* resize_channel() noexcept { return resize_channel_.get(); }

  /** Return the assigned Screen command identifier, or zero while stopped. */
  ipc::ChannelId command_channel_id() const noexcept { return command_id_; }

  /** Return the assigned resize-event identifier, or zero while stopped. */
  ipc::ChannelId resize_channel_id() const noexcept { return resize_id_; }

 private:
  /** Close registered routes and clear retained state. */
  Status close_channels() noexcept;

  ipc::Directory* directory_ = nullptr; /**< Borrowed while this is started. */
  std::shared_ptr<ipc::Channel> command_channel_; /**< Command producer. */
  std::shared_ptr<ipc::Channel> resize_channel_;  /**< Resize producer. */
  ipc::ChannelId command_id_ = 0U; /**< Directory-assigned command id. */
  ipc::ChannelId resize_id_  = 0U; /**< Directory-assigned resize id. */
};

/**
 * Register the ordered `//terminal/input_events` producer channel.
 *
 * TerminalSubsystem publishes decoded events through this route. Screen owns
 * the transport subscription and retains normalized events until an application
 * drains them to apply only its mode-specific policy. No additional
 * input-router mechanism is interposed.
 */
class TerminalInputChannelSubsystem final : public AppSubsystem {
 public:
  /** Declare the channel-directory dependency. */
  TerminalInputChannelSubsystem();

  /** Validate access to the registered DirectorySubsystem. */
  Status initialize(AppState& app) override;

  /** Create and register the canonical terminal-input channel. */
  Status start(AppState& app) override;

  /** Close the route and release its endpoint. */
  Status stop(AppState& app) noexcept override;

  /** Release any endpoint retained after partial lifecycle progress. */
  Status terminate(AppState& app) noexcept override;

  /** Return the terminal-input endpoint, or nullptr while stopped. */
  ipc::Channel* channel() noexcept { return channel_.get(); }

  /** Return the assigned terminal-input identifier, or zero while stopped. */
  ipc::ChannelId channel_id() const noexcept { return channel_id_; }

 private:
  Status close_channel() noexcept;

  ipc::Directory* directory_ = nullptr;   /**< Borrowed while started. */
  std::shared_ptr<ipc::Channel> channel_; /**< Ordered event endpoint. */
  ipc::ChannelId channel_id_ = 0U; /**< Directory-assigned endpoint id. */
};

/**
 * Register the latest-only `//cmdframe/notify` producer channel.
 *
 * Command implementations publish through Directory; command-frame consumers
 * depend on this adapter and retain only a subscription. Keeping route
 * ownership separate from CommandDispatcher permits either side to restart
 * without making the registry responsible for transport lifetime.
 */
class CommandNotificationChannelSubsystem final : public AppSubsystem {
 public:
  /** Declare the channel-directory dependency. */
  CommandNotificationChannelSubsystem();

  /** Validate access to the registered DirectorySubsystem. */
  Status initialize(AppState& app) override;

  /** Create and register the canonical notification channel. */
  Status start(AppState& app) override;

  /** Close the canonical route and release its endpoint. */
  Status stop(AppState& app) noexcept override;

  /** Release any endpoint retained after partial lifecycle progress. */
  Status terminate(AppState& app) noexcept override;

  /** Return the notification endpoint, or nullptr while stopped. */
  ipc::Channel* channel() noexcept { return channel_.get(); }

  /** Return the assigned notification identifier, or zero while stopped. */
  ipc::ChannelId channel_id() const noexcept { return channel_id_; }

 private:
  /** Close the registered route and clear retained state. */
  Status close_channel() noexcept;

  ipc::Directory* directory_ = nullptr; /**< Borrowed while this is started. */
  std::shared_ptr<ipc::Channel> channel_; /**< Latest-only producer endpoint. */
  ipc::ChannelId channel_id_ = 0U; /**< Directory-assigned endpoint id. */
};

}  // namespace puc::app
