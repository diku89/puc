#pragma once

/**
 * @file configuration.hpp
 * @brief Lifecycle owner for application-wide configuration roots.
 */

#include <filesystem>
#include <memory>

#include "puc-cli/state/state.hpp"

namespace puc::config {
class Config;
}

namespace puc::app {

/** Immutable roots used to construct one configuration service generation. */
struct ConfigurationSubsystemOptions {
  std::filesystem::path
      primary_root; /**< Packaged/default configuration root. */
  std::filesystem::path
      user_overrides_root; /**< Optional user-overlay configuration root. */
};

/**
 * Own the process-wide Config service from initialize through terminate.
 *
 * Config performs fresh file reads for every load, so reload operations do not
 * require replacing this object. The stable service and its root policy survive
 * every stop/start cycle; only final termination releases them.
 */
class ConfigurationSubsystem final : public AppSubsystem {
 public:
  /** Retain the roots used by the one initialized configuration service. */
  explicit ConfigurationSubsystem(ConfigurationSubsystemOptions options = {});

  /** Destroy a configuration service already released by terminate(). */
  ~ConfigurationSubsystem() override;

  /** Construct the durable root-scoped configuration service exactly once. */
  Status initialize(AppState& app) override;

  /** Validate that the initialized configuration service remains available. */
  Status start(AppState& app) override;

  /** Preserve configuration state across a suspend-style stop. */
  Status stop(AppState& app) noexcept override;

  /** Release the durable configuration service exactly once. */
  Status terminate(AppState& app) noexcept override;

  /** Return the initialized service, or nullptr outside its lifetime. */
  config::Config* configuration() noexcept { return configuration_.get(); }

  /** Return the initialized service, or nullptr outside its lifetime. */
  const config::Config* configuration() const noexcept {
    return configuration_.get();
  }

 private:
  ConfigurationSubsystemOptions
      options_; /**< Roots retained until terminate. */
  std::unique_ptr<config::Config>
      configuration_; /**< Durable root-scoped configuration service. */
};

}  // namespace puc::app
