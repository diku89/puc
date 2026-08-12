#pragma once

/**
 * @file theme.hpp
 * @brief Lifecycle owner for the property-backed application theme.
 */

#include <memory>

#include "puc-cli/state/state.hpp"
#include "themes/themes.hpp"

namespace puc::tui {
class Theme;
}

namespace puc::app {

/**
 * Own the application Theme and load its defaults through PropertiesSubsystem.
 *
 * The Theme survives suspend-style stop/start cycles. Each start reapplies the
 * current user-mutable values, so changes made through `config set` become
 * visible on the next running generation.
 */
class ThemeSubsystem final : public AppSubsystem {
 public:
  /** Declare the properties dependency. */
  ThemeSubsystem();

  /** Destroy a Theme already released by terminate(). */
  ~ThemeSubsystem() override;

  /** Register the packaged default palette and construct Theme. */
  Status initialize(AppState& app) override;

  /** Apply the current mutable palette to the running generation. */
  Status start(AppState& app) override;

  /** Preserve the Theme and its properties across a temporary stop. */
  Status stop(AppState& app) noexcept override;

  /** Release the Theme after the final running generation. */
  Status terminate(AppState& app) noexcept override;

  /** Return the initialized theme, or nullptr outside its lifetime. */
  tui::Theme* theme() noexcept { return theme_.get(); }

  /** Return the initialized theme, or nullptr outside its lifetime. */
  const tui::Theme* theme() const noexcept { return theme_.get(); }

  /** Return the latest detailed theme loading result. */
  themes::Status theme_status() const noexcept { return theme_status_; }

 private:
  std::unique_ptr<tui::Theme> theme_; /**< Durable semantic palette. */
  themes::Status theme_status_ = themes::Status::OK; /**< Last load result. */
};

}  // namespace puc::app
