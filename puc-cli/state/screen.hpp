#pragma once

/**
 * @file screen.hpp
 * @brief Lifecycle adapter for terminal presentation policy.
 */

#include <memory>
#include <optional>

#include "msgs/screen_msgs.hpp"
#include "puc-cli/state/state.hpp"
#include "puc-cli/tui/status.hpp"

namespace puc::tui {
class Screen;
}

namespace puc::app {

/** Terminal ownership policy applied by ScreenSubsystem in TUI mode. */
struct ScreenSubsystemOptions {
  bool take_terminal =
      true; /**< Whether start() requests terminal ownership. */
  std::optional<msg::ScreenSessionOptions>
      session_options; /**< Explicit modes, or Screen's standard modes. */
};

/**
 * Own Screen while borrowing DirectorySubsystem and TerminalSubsystem.
 *
 * In TUI mode start() normally requests terminal ownership after constructing
 * Screen over the lifecycle-owned mechanisms. TEST mode constructs the same
 * presentation and subscription graph without mutating a host terminal.
 */
class ScreenSubsystem final : public AppSubsystem {
 public:
  /** Construct an adapter with terminal-ownership policy. */
  explicit ScreenSubsystem(ScreenSubsystemOptions options = {});

  /** Destroy a released Screen. */
  ~ScreenSubsystem() override;

  /** Validate terminal and directory adapter registration. */
  Status initialize(AppState& app) override;

  /** Construct Screen and optionally request terminal ownership. */
  Status start(AppState& app) override;

  /** Request release and destroy Screen before its borrowed mechanisms stop. */
  Status stop(AppState& app) noexcept override;

  /** Release any Screen retained after partial lifecycle progress. */
  Status terminate(AppState& app) noexcept override;

  /** Return the running presentation object, or nullptr while stopped. */
  tui::Screen* screen() noexcept { return screen_.get(); }

  /** Return the running presentation object, or nullptr while stopped. */
  const tui::Screen* screen() const noexcept { return screen_.get(); }

  /** Return the latest Screen status observed by a lifecycle hook. */
  tui::Status screen_status() const noexcept { return screen_status_; }

 private:
  ScreenSubsystemOptions options_;      /**< Terminal ownership policy. */
  std::unique_ptr<tui::Screen> screen_; /**< Active presentation object. */
  tui::Status screen_status_ =
      tui::Status::OK; /**< Latest mechanism-specific detail. */
};

}  // namespace puc::app
