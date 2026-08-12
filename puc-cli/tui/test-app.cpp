/**
 * @file test-app.cpp
 * @brief Lifecycle entry point for the interactive TUI rendering smoke test.
 *
 * Primary application logic lives in TuiTestRuntimeSubsystem. `main()` owns
 * only command-independent process setup, graph registration, and the one
 * initialize/start/stop/terminate lifetime.
 */

#include <unistd.h>

#include <clocale>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "puc-cli/tui/terminal/configuration_paths.hpp"
#include "puc-cli/tui/tui_test_runtime.hpp"
#include "state/bootstrap.hpp"
#include "state/control.hpp"
#include "utils/logger/logger.hpp"

/** @cond TUI_TEST_APP_LOGGER_MODULE */
LOGGER_MODULE("TUI Test App");
/** @endcond */

namespace {

/** Shared worker budget for rendering and lifecycle-owned IPC delivery. */
constexpr std::uint8_t kWorkerCount = 4U;

/** Process-wide logging policy used before and during the app lifecycle. */
const puc::logger::LoggerConf kLoggerConfiguration{
    .global_level = puc::logger::LogLevel::WARN};

/** Terminal profile name captured once from the process environment. */
const std::string kTerminalName = puc::terminal::environment_value("TERM");

/** Terminal descriptors retained by the canonical subsystem graph. */
const puc::app::TerminalSubsystemOptions kTerminalOptions{
    .input_fd          = STDIN_FILENO,
    .output_fd         = STDOUT_FILENO,
    .decoder_limits    = {},
    .configure_decoder = false,
    .terminal_name     = kTerminalName,
};

/**
 * Static smoke-test profile copied before discovered property roots are added.
 */
const puc::app::ApplicationSubsystemOptions kApplicationProfile{
    .logger       = kLoggerConfiguration,
    .worker_count = kWorkerCount,
    .properties   = {},
    .terminal     = kTerminalOptions,
    .selection =
        puc::app::ApplicationSubsystemSelection{
            .metronome         = false,
            .presentation      = true,
            .commands          = false,
            .input             = false,
            .command_mode      = false,
            .embedded_terminal = false,
        },
};

}  // namespace

/**
 * Run the smoke test until application control requests termination.
 *
 * @param[in] argc Conventional process argument count.
 * @param[in] argv Conventional process argument vector; argv[0] locates the
 *                 packaged theme configuration.
 * @return Zero when setup, drawing, and lifecycle teardown succeed; otherwise
 *         one.
 */
int main(int argc, char** argv) {
  LOGGER_INIT(kLoggerConfiguration);
  if (std::setlocale(LC_CTYPE, "") == nullptr) {
    Logger<WARN> << "Could not activate the environment character encoding";
  }
  const std::string_view executable = argc > 0 && argv[0] != nullptr
                                          ? std::string_view{argv[0]}
                                          : std::string_view{};
  const puc::terminal::ConfigurationRoots roots =
      puc::terminal::discover_configuration_roots(executable);
  puc::app::ApplicationSubsystemOptions options = kApplicationProfile;
  options.properties = puc::app::PropertiesSubsystemOptions{
      .primary_root        = roots.primary,
      .user_overrides_root = roots.user_overrides,
  };

  puc::app::AppState app;
  const puc::app::Status graph_registration_status =
      puc::app::register_application_subsystems(app, std::move(options));
  auto runtime = std::make_unique<puc::app::TuiTestRuntimeSubsystem>();
  puc::app::TuiTestRuntimeSubsystem* runtime_view = runtime.get();
  if (puc::app::is_ok(graph_registration_status)) {
    const puc::app::Status runtime_registration_status =
        app.register_subsystem(std::move(runtime));
    if (!puc::app::is_ok(runtime_registration_status)) {
      Logger<ERROR> << "Could not register TUI test runtime: "
                    << puc::app::status_message(runtime_registration_status);
      return 1;
    }
  } else {
    Logger<ERROR> << "Could not register application subsystems: "
                  << puc::app::status_message(graph_registration_status);
    return 1;
  }

  const puc::app::Status initialize_status =
      app.initialize(puc::app::OperatingMode::TUI);
  if (!puc::app::is_ok(initialize_status)) {
    Logger<ERROR> << "Could not initialize application subsystems: "
                  << puc::app::status_message(initialize_status);
    static_cast<void>(app.terminate());
    return 1;
  }
  const puc::app::Status start_status = app.start();
  if (!puc::app::is_ok(start_status)) {
    Logger<ERROR> << "Could not start application subsystems: "
                  << puc::app::status_message(start_status);
    static_cast<void>(app.terminate());
    return 1;
  }

  const auto* control =
      app.get_subsystem<puc::app::ApplicationControlSubsystem>();
  int exit_code = 0;
  if (control == nullptr || control->control() == nullptr) {
    Logger<ERROR> << "Application control was not initialized";
    exit_code = 1;
  }
  while (exit_code == 0 && !control->exit_requested()) {
    if (!runtime_view->draw()) {
      exit_code = 1;
    }
  }

  const puc::app::Status stop_status = app.stop();
  if (!puc::app::is_ok(stop_status)) {
    Logger<ERROR> << "Could not stop application subsystems: "
                  << puc::app::status_message(stop_status);
    exit_code = 1;
  }
  if (!puc::app::is_ok(app.terminate())) {
    exit_code = 1;
  }
  return exit_code;
}
