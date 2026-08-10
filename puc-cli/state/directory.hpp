#pragma once

/**
 * @file directory.hpp
 * @brief Application lifecycle adapter for the IPC channel directory.
 */

#include <memory>

#include "puc-cli/state/state.hpp"

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
  /** Declare the worker-pool dependency used for channel delivery. */
  DirectorySubsystem();

  /** Destroy a released channel directory. */
  ~DirectorySubsystem() override;

  /** Validate that the registered WorkerSubsystem can be resolved. */
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

 private:
  std::unique_ptr<ipc::Directory>
      directory_; /**< Registry present only while this adapter is started. */
};

}  // namespace puc::app
