#pragma once

/**
 * @file bootstrap.hpp
 * @brief Canonical registration of the complete application subsystem graph.
 */

#include <cstddef>
#include <cstdint>

#include "puc-cli/state/logger.hpp"
#include "puc-cli/state/screen.hpp"
#include "puc-cli/state/state.hpp"
#include "puc-cli/state/terminal.hpp"

namespace puc::app {

/** Number of concrete adapters installed by register_application_subsystems. */
inline constexpr std::size_t kApplicationSubsystemCount = 9U;

/** Configuration retained by the adapters in the canonical application graph.
 */
struct ApplicationSubsystemOptions {
  logger::LoggerConf logger;         /**< Process-wide logging policy. */
  std::uint8_t worker_count = 4U;    /**< Shared worker-pool width. */
  TerminalSubsystemOptions terminal; /**< Terminal descriptors and decoding. */
  ScreenSubsystemOptions screen;     /**< Terminal presentation policy. */
};

/**
 * Register the complete concrete adapter graph into an empty AppState.
 *
 * Registration is root-to-leaf for readability, although AppState derives the
 * actual order from declared dependencies. The function does not initialize
 * or start the graph. A nonempty AppState is rejected to avoid returning a
 * misleading partial "default" graph.
 *
 * @return Status::OK, Status::INVALID_ARGUMENT for a nonempty AppState, or an
 *         unexpected registration failure.
 */
Status register_application_subsystems(
    AppState& app, ApplicationSubsystemOptions options = {});

}  // namespace puc::app
