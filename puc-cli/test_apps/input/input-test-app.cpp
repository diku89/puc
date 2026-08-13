/**
 * @file input-test-app.cpp
 * @brief Lifecycle entry point for the InputFrame manual test application.
 *
 * Primary application logic lives in InputTestRuntimeSubsystem. `main()` owns
 * the process boundary: subsystem registration and the one
 * initialize/start/stop/terminate lifetime.
 */

#include <unistd.h>

#include <clocale>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "puc-cli/application/application_control_subsystem.hpp"
#include "puc-cli/application/bootstrap.hpp"
#include "puc-cli/test_apps/input/input_test_runtime.hpp"
#include "puc-cli/tui/terminal/configuration_paths.hpp"
#include "utils/logger/logger.hpp"

/** @cond INPUT_TEST_APP_LOGGER_MODULE */
LOGGER_MODULE("Input Test App");
/** @endcond */

namespace {

/** Shared worker budget for IPC delivery, rendering, timers, and commands. */
constexpr std::uint8_t kWorkerCount = 4U;

/** Process-wide logging policy used before and during the app lifecycle. */
const puc::logger::LoggerConf kLoggerConfiguration{
    .global_level = puc::logger::LogLevel::WARN};

/** Terminal profile name captured once from the process environment. */
const std::string kTerminalName = puc::terminal::environment_value("TERM");

/** Interactive shell selected once for embedded-terminal test generations. */
const std::string kEmbeddedShell = [] {
  std::string shell = puc::terminal::environment_value("PUC_TEST_SHELL");
  return shell.empty() ? std::string{"/bin/sh"} : shell;
}();

/** Terminal decoding and descriptor policy retained across start/stop cycles.
 */
const puc::app::TerminalSubsystemOptions kTerminalOptions{
    .input_fd          = STDIN_FILENO,
    .output_fd         = STDOUT_FILENO,
    .decoder_limits    = {},
    .configure_decoder = true,
    .terminal_name     = kTerminalName,
};

/** Embedded-terminal child launch policy retained by its subsystem. */
const puc::app::EmbeddedTerminalSubsystemOptions kEmbeddedTerminalOptions{
    .shell = kEmbeddedShell};

/**
 * Static application profile copied by main before discovered paths are added.
 *
 * Configuration roots depend on argv[0] and therefore remain the sole profile
 * values supplied at the process boundary.
 */
const puc::app::ApplicationSubsystemOptions kApplicationProfile{
    .logger            = kLoggerConfiguration,
    .worker_count      = kWorkerCount,
    .properties        = {},
    .terminal          = kTerminalOptions,
    .embedded_terminal = kEmbeddedTerminalOptions,
};

}  // namespace

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
  auto runtime = std::make_unique<puc::app::InputTestRuntimeSubsystem>();
  puc::app::InputTestRuntimeSubsystem* runtime_view = runtime.get();
  if (puc::app::is_ok(graph_registration_status)) {
    const puc::app::Status runtime_registration_status =
        app.register_subsystem(std::move(runtime));
    if (!puc::app::is_ok(runtime_registration_status)) {
      Logger<ERROR> << "Could not register input-test runtime: "
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
      Logger<ERROR> << "Input-test runtime failed";
      exit_code = 1;
    }
  }

  const puc::app::Status stop_status = app.stop();
  if (!puc::app::is_ok(stop_status)) {
    Logger<ERROR> << "Could not stop application subsystems: "
                  << puc::app::status_message(stop_status);
    exit_code = 1;
  }
  const puc::app::Status terminate_status = app.terminate();
  if (!puc::app::is_ok(terminate_status)) {
    exit_code = 1;
  }
  return exit_code;
}
