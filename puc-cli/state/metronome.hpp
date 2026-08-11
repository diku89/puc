#pragma once

/**
 * @file metronome.hpp
 * @brief Lifecycle adapter for the production heartbeat publisher.
 */

#include <memory>

#include "puc-cli/state/state.hpp"
#include "utils/metronome/status.hpp"

namespace puc::metronome {
class Metronome;
}

namespace puc::app {

/**
 * Own the one-hertz heartbeat route and periodic job while the app is running.
 *
 * Every start binds to the current Directory and Worker generations, registers
 * `//metronome/1hz`, and schedules publication. Stop cancels the periodic job
 * and removes the route before either borrowed subsystem can stop.
 */
class MetronomeSubsystem final : public AppSubsystem {
 public:
  /** Declare the channel-directory and worker-pool dependencies. */
  MetronomeSubsystem();

  /** Destroy a stopped heartbeat publisher. */
  ~MetronomeSubsystem() override;

  /** Validate both registered mechanism owners. */
  Status initialize(AppState& app) override;

  /** Construct and start a publisher for the current running generation. */
  Status start(AppState& app) override;

  /** Cancel ticks, close the route, and release the current publisher. */
  Status stop(AppState& app) noexcept override;

  /** Release any publisher retained after partial lifecycle progress. */
  Status terminate(AppState& app) noexcept override;

  /** Return the running heartbeat publisher, or nullptr while stopped. */
  metronome::Metronome* metronome() noexcept { return metronome_.get(); }

  /** Return the latest detailed heartbeat lifecycle status. */
  metronome::Status metronome_status() const noexcept { return status_; }

 private:
  std::unique_ptr<metronome::Metronome>
      metronome_; /**< Publisher for one running generation. */
  metronome::Status status_ = metronome::Status::OK; /**< Last start status. */
};

}  // namespace puc::app
