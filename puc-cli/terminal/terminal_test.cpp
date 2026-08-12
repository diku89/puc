/**
 * @file terminal_test.cpp
 * @brief Lifecycle entry point for terminal input conformance testing.
 *
 * TerminalTestRuntimeSubsystem owns the interactive test plan, presentation,
 * and durable report. `main()` owns command-line handling, static process
 * configuration, subsystem registration, and the single initialized lifetime.
 */

#include <unistd.h>

#include <clocale>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "puc-cli/state/bootstrap.hpp"
#include "puc-cli/state/control.hpp"
#include "puc-cli/terminal/configuration_paths.hpp"
#include "puc-cli/terminal/terminal_test_options.hpp"
#include "puc-cli/terminal/terminal_test_runtime.hpp"
#include "utils/logger/logger.hpp"

/** @cond TERMINAL_TEST_APP_LOGGER_MODULE */
LOGGER_MODULE("Terminal Test App");
/** @endcond */

namespace {

/** Shared worker budget for presentation, input delivery, and heartbeat work.
 */
constexpr std::uint8_t kWorkerCount = 4U;

/** Process-wide logging policy used before and during the app lifecycle. */
const puc::logger::LoggerConf kLoggerConfiguration{
    .global_level = puc::logger::LogLevel::WARN};

/** Terminal profile name captured once from the process environment. */
const std::string kTerminalName = puc::terminal::environment_value("TERM");

/** Real terminal descriptors and property-backed decoder configuration. */
const puc::app::TerminalSubsystemOptions kTerminalOptions{
    .input_fd          = STDIN_FILENO,
    .output_fd         = STDOUT_FILENO,
    .decoder_limits    = {},
    .configure_decoder = true,
    .terminal_name     = kTerminalName,
};

/**
 * Static conformance profile copied before discovered property roots are added.
 */
const puc::app::ApplicationSubsystemOptions kApplicationProfile{
    .logger       = kLoggerConfiguration,
    .worker_count = kWorkerCount,
    .properties   = {},
    .terminal     = kTerminalOptions,
    .selection =
        puc::app::ApplicationSubsystemSelection{
            .metronome         = true,
            .presentation      = true,
            .commands          = false,
            .input             = false,
            .command_mode      = false,
            .embedded_terminal = false,
        },
};

}  // namespace

/**
 * Run a selected terminal conformance plan and print its restored-screen
 * report.
 *
 * @param[in] argc Conventional process argument count.
 * @param[in] argv Conventional process argument vector; argv[0] locates the
 *                 packaged terminal and theme configuration.
 * @return Zero for help/listing or a completely passing run; two for invalid
 *         options; otherwise one.
 */
int main(int argc, char** argv) {
  LOGGER_INIT(kLoggerConfiguration);
  if (std::setlocale(LC_CTYPE, "") == nullptr) {
    Logger<WARN> << "Could not activate the environment character encoding";
  }
  const std::string_view executable = argc > 0 && argv[0] != nullptr
                                          ? std::string_view{argv[0]}
                                          : std::string_view{};
  std::vector<std::string_view> arguments;
  arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0U);
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index] == nullptr ? "" : argv[index]);
  }

  puc::terminal::TerminalTestOptions parsed_options;
  const puc::terminal::TerminalTestOptionsStatus option_status =
      puc::terminal::parse_terminal_test_options(arguments, parsed_options);
  if (option_status != puc::terminal::TerminalTestOptionsStatus::OK) {
    std::cerr << "terminal-test: "
              << puc::terminal::terminal_test_options_status_message(
                     option_status);
    if (!parsed_options.argument.empty()) {
      std::cerr << ": " << parsed_options.argument;
    }
    std::cerr << "\n\n";
    puc::terminal::print_terminal_test_usage(std::cerr, executable);
    std::cerr << "\nRun --list to see valid test names.\n";
    return 2;
  }
  if (parsed_options.command == puc::terminal::TerminalTestCommand::LIST) {
    puc::terminal::print_terminal_test_list(std::cout);
    return 0;
  }
  if (parsed_options.command == puc::terminal::TerminalTestCommand::HELP) {
    puc::terminal::print_terminal_test_usage(std::cout, executable);
    return 0;
  }

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
  auto runtime = std::make_unique<puc::app::TerminalTestRuntimeSubsystem>(
      parsed_options.selected_test);
  puc::app::TerminalTestRuntimeSubsystem* runtime_view = runtime.get();
  if (puc::app::is_ok(graph_registration_status)) {
    const puc::app::Status runtime_registration_status =
        app.register_subsystem(std::move(runtime));
    if (!puc::app::is_ok(runtime_registration_status)) {
      Logger<ERROR> << "Could not register terminal-test runtime: "
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
  while (exit_code == 0 && !control->exit_requested() &&
         !runtime_view->finished()) {
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
  if (!runtime_view->print_report()) {
    exit_code = 1;
  }
  if (!puc::app::is_ok(app.terminate())) {
    exit_code = 1;
  }
  return exit_code;
}
