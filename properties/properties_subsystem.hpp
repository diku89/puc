#pragma once

/**
 * @file properties_subsystem.hpp
 * @brief Lifecycle owner for application-wide properties.
 */

#include <filesystem>
#include <memory>

#include "state/state.hpp"

namespace puc::properties {
class Properties;
}

namespace puc::app {

/** Immutable roots used by the sole application properties service. */
struct PropertiesSubsystemOptions {
  std::filesystem::path primary_root; /**< Packaged/default source root. */
  std::filesystem::path
      user_overrides_root; /**< Optional user-overlay source root. */
};

/**
 * Own all application configuration and mutable property state.
 *
 * The service is constructed once during initialize, survives every
 * suspend-style stop/start cycle, and is released only by terminate. No other
 * subsystem may construct or access the low-level TOML Config loader.
 */
class PropertiesSubsystem final : public AppSubsystem {
 public:
  /** Retain roots for the one initialized properties service. */
  explicit PropertiesSubsystem(PropertiesSubsystemOptions options = {});

  /** Destroy a service already released by terminate(). */
  ~PropertiesSubsystem() override;

  /** Construct the durable properties service exactly once. */
  Status initialize(AppState& app) override;

  /** Validate that initialized property state remains available. */
  Status start(AppState& app) override;

  /** Preserve documents and mutations across a suspend-style stop. */
  Status stop(AppState& app) noexcept override;

  /** Release all property state after the final running generation. */
  Status terminate(AppState& app) noexcept override;

  /** Return the initialized service, or nullptr outside its lifetime. */
  properties::Properties* properties() noexcept { return properties_.get(); }

  /** Return the initialized service, or nullptr outside its lifetime. */
  const properties::Properties* properties() const noexcept {
    return properties_.get();
  }

 private:
  PropertiesSubsystemOptions options_; /**< Roots retained until terminate. */
  std::unique_ptr<properties::Properties>
      properties_; /**< Durable documents and mutable values. */
};

}  // namespace puc::app
