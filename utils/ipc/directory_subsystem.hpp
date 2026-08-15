#pragma once

/**
 * @file directory_subsystem.hpp
 * @brief Application lifecycle adapter for the IPC channel directory.
 */

#include <cstddef>
#include <memory>

#include "state/state.hpp"

namespace puc::ipc {
class Directory;
}

namespace puc::app {

/**
 * Own the process-local channel Directory above WorkerSubsystem.
 *
 * Each start creates a Directory borrowing that lifecycle generation's live
 * worker pool. Each stop destroys the Directory first, detaching and draining
 * every registered channel while its delivery workers still accept work.
 */
class DirectorySubsystem final : public AppSubsystem {
 public:
  /** Declare configuration and worker-pool lifecycle dependencies. */
  DirectorySubsystem();

  /** Destroy a released channel directory. */
  ~DirectorySubsystem() override;

  /** Load the configured message limit and validate dependencies. */
  Status initialize(AppState& app) override;

  /** Construct a Directory over the currently running worker pool. */
  Status start(AppState& app) override;

  /** Detach all channels and release the Directory. */
  Status stop(AppState& app) noexcept override;

  /** Release any Directory retained after partial lifecycle progress. */
  Status terminate(AppState& app) noexcept override;

  /** Return the live Directory, or nullptr outside the running phase. */
  ipc::Directory* directory() noexcept { return directory_.get(); }

  /** Return the live Directory, or nullptr outside the running phase. */
  const ipc::Directory* directory() const noexcept { return directory_.get(); }

  /** Return the initialized general IPC payload limit. */
  std::size_t maximum_message_bytes() const noexcept {
    return maximum_message_bytes_;
  }

 private:
  std::unique_ptr<ipc::Directory>
      directory_; /**< Registry present only while this adapter is started. */
  std::size_t maximum_message_bytes_ =
      0U; /**< Property-backed limit retained across stop/start cycles. */
};

}  // namespace puc::app
