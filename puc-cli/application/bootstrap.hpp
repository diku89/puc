#pragma once

/**
 * @file bootstrap.hpp
 * @brief Canonical registration of application subsystem graph profiles.
 */

#include <cstddef>
#include <cstdint>

#include "properties/properties_subsystem.hpp"
#include "puc-cli/tui/terminal/embedded_terminal_subsystem.hpp"
#include "puc-cli/tui/terminal/terminal_subsystem.hpp"
#include "state/state.hpp"
#include "utils/logger/logger_subsystem.hpp"

namespace puc::app {

/** Optional leaf subsystems selected for one executable's application graph. */
struct ApplicationSubsystemSelection {
  bool canvas       = true; /**< Register durable Canvas and orchestration. */
  bool metronome    = true; /**< Register the production heartbeat. */
  bool presentation = true; /**< Register parallel frame scheduling. */
  bool commands     = true; /**< Register command dispatch and its route. */
  bool input        = true; /**< Register the composite application input. */
  bool command_mode = true; /**< Bind command input to dispatch. */
  bool embedded_terminal =
      true; /**< Register integrated-terminal PTY ownership. */
};

/** Number of concrete adapters in the complete default production graph. */
inline constexpr std::size_t kApplicationSubsystemCount = 22U;

/** Return the number of adapters selected by one executable profile. */
constexpr std::size_t application_subsystem_count(
    const ApplicationSubsystemSelection& selection) noexcept {
  constexpr std::size_t base_subsystems = 11U;
  const std::size_t canvas_subsystems   = selection.canvas ? 3U : 0U;
  const bool command_notification       = selection.commands || selection.input;
  const std::size_t command_subsystems  = selection.commands ? 2U : 0U;
  return base_subsystems + canvas_subsystems +
         static_cast<std::size_t>(command_notification) +
         static_cast<std::size_t>(selection.metronome) +
         static_cast<std::size_t>(selection.presentation) + command_subsystems +
         static_cast<std::size_t>(selection.input) +
         static_cast<std::size_t>(selection.command_mode) +
         static_cast<std::size_t>(selection.embedded_terminal);
}

/** Configuration retained by the adapters in the canonical application graph.
 */
struct ApplicationSubsystemOptions {
  logger::LoggerConf logger;      /**< Process-wide logging policy. */
  std::uint8_t worker_count = 4U; /**< Shared worker-pool width. */
  PropertiesSubsystemOptions
      properties;                    /**< Application-wide properties roots. */
  TerminalSubsystemOptions terminal; /**< Terminal descriptors and decoding. */
  EmbeddedTerminalSubsystemOptions
      embedded_terminal; /**< Integrated-terminal child launch policy. */
  ApplicationSubsystemSelection
      selection; /**< Executable-specific optional subsystem profile. */
};

/**
 * Register the selected concrete adapter graph into an empty AppState.
 *
 * Registration is root-to-leaf for readability, although AppState derives the
 * actual order from declared dependencies. The function does not initialize
 * or start the graph. A nonempty AppState is rejected to avoid returning a
 * misleading partial graph. Invalid leaf combinations, such as command mode
 * without both command and input subsystems, are rejected before registration.
 *
 * @return Status::OK, Status::INVALID_ARGUMENT for a nonempty AppState, or an
 *         unexpected registration failure.
 */
Status register_application_subsystems(
    AppState& app, ApplicationSubsystemOptions options = {});

}  // namespace puc::app
