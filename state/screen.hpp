#pragma once

/**
 * @file screen.hpp
 * @brief Lifecycle adapter for terminal presentation policy.
 */

#include <memory>

#include "puc-cli/tui/rendering/status.hpp"
#include "state/state.hpp"

namespace puc::tui {
class Screen;
}

namespace puc::app {

/**
 * Own Screen while borrowing DirectorySubsystem, TerminalSubsystem, and the
 * lifecycle-owned terminal-input route.
 *
 * In TUI mode start() requests terminal ownership after constructing Screen
 * over the lifecycle-owned mechanisms. TEST mode constructs the same
 * presentation and subscription graph without mutating a host terminal.
 */
class ScreenSubsystem final : public AppSubsystem {
 public:
  /** Construct the canonical Screen lifecycle adapter. */
  ScreenSubsystem();

  /** Destroy a released Screen. */
  ~ScreenSubsystem() override;

  /** Validate terminal and directory adapter registration. */
  Status initialize(AppState& app) override;

  /** Construct Screen and request terminal ownership in TUI mode. */
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
  std::unique_ptr<tui::Screen> screen_; /**< Active presentation object. */
  tui::Status screen_status_ =
      tui::Status::OK; /**< Latest mechanism-specific detail. */
};

}  // namespace puc::app
