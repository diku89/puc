#pragma once

/**
 * @file datastore_subsystem.hpp
 * @brief Lifecycle ownership for Canvas persistence and migrations.
 */

#include <memory>

#include "state/state.hpp"

namespace puc::canvas::datastore {
class Database;
}

namespace puc::app {

/** Own the application SQLite connection and apply all datastore migrations. */
class DatastoreSubsystem final : public AppSubsystem {
 public:
  /** Construct a closed datastore lifecycle adapter. */
  DatastoreSubsystem();

  /** Destroy the database already released by terminate(). */
  ~DatastoreSubsystem() override;

  /** Resolve configuration, open SQLite, and apply every migration exactly
   * once. */
  Status initialize(AppState& app) override;

  /** Validate that durable storage remains available to dependent subsystems.
   */
  Status start(AppState& app) override;

  /** Preserve the database across a suspend-style stop. */
  Status stop(AppState& app) noexcept override;

  /** Close SQLite after every dependent datastore wrapper has been released. */
  Status terminate(AppState& app) noexcept override;

  /** Return the initialized database, or nullptr outside its durable lifetime.
   */
  canvas::datastore::Database* database() noexcept;

  /** Return the initialized database, or nullptr outside its durable lifetime.
   */
  const canvas::datastore::Database* database() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_; /**< Hidden database and configuration state. */
};

}  // namespace puc::app
