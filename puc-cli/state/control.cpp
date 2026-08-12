/**
 * @file control.cpp
 * @brief Deferred application-exit lifecycle implementation.
 */

#include "puc-cli/state/control.hpp"

#include <signal.h>

#include <csignal>
#include <memory>
#include <utility>

namespace puc::app {

struct ApplicationControlSubsystem::SignalHandlers {
  using Action = struct sigaction;

  Action interrupt{};   /**< Disposition preceding SIGINT ownership. */
  Action termination{}; /**< Disposition preceding SIGTERM ownership. */
};

volatile std::sig_atomic_t
    ApplicationControlSubsystem::termination_signal_requested_ = 0;
ApplicationControlSubsystem* ApplicationControlSubsystem::signal_owner_ =
    nullptr;

ApplicationControlSubsystem::ApplicationControlSubsystem()
    : AppSubsystem("application-control") {}

ApplicationControlSubsystem::~ApplicationControlSubsystem() {
  static_cast<void>(restore_signal_handlers());
}

Status ApplicationControlSubsystem::initialize(AppState& app) {
  control_ = std::make_unique<ApplicationControl>();
  if (app.operating_mode() == OperatingMode::TUI) {
    const Status status = install_signal_handlers();
    if (!is_ok(status)) {
      control_.reset();
      return status;
    }
  }
  return Status::OK;
}

Status ApplicationControlSubsystem::start(AppState& app) {
  static_cast<void>(app);
  return control_ == nullptr ? Status::SUBSYSTEM_FAILURE : Status::OK;
}

Status ApplicationControlSubsystem::stop(AppState& app) noexcept {
  static_cast<void>(app);
  return Status::OK;
}

Status ApplicationControlSubsystem::terminate(AppState& app) noexcept {
  static_cast<void>(app);
  const Status status = restore_signal_handlers();
  control_.reset();
  return status;
}

bool ApplicationControlSubsystem::exit_requested() const noexcept {
  return (signal_handlers_ != nullptr && termination_signal_requested_ != 0) ||
         (control_ != nullptr && control_->exit_requested());
}

void ApplicationControlSubsystem::handle_termination_signal(
    int signal_number) noexcept {
  static_cast<void>(signal_number);
  termination_signal_requested_ = 1;
}

Status ApplicationControlSubsystem::install_signal_handlers() {
  if (signal_handlers_ != nullptr || signal_owner_ != nullptr) {
    return Status::SUBSYSTEM_FAILURE;
  }

  auto handlers = std::make_unique<SignalHandlers>();
  struct sigaction action {};
  action.sa_handler = &ApplicationControlSubsystem::handle_termination_signal;
  if (sigemptyset(&action.sa_mask) != 0) {
    return Status::SUBSYSTEM_FAILURE;
  }
  action.sa_flags = SA_RESTART;

  termination_signal_requested_ = 0;
  if (::sigaction(SIGINT, &action, &handlers->interrupt) != 0) {
    return Status::SUBSYSTEM_FAILURE;
  }
  if (::sigaction(SIGTERM, &action, &handlers->termination) != 0) {
    static_cast<void>(::sigaction(SIGINT, &handlers->interrupt, nullptr));
    return Status::SUBSYSTEM_FAILURE;
  }

  signal_handlers_ = std::move(handlers);
  signal_owner_    = this;
  return Status::OK;
}

Status ApplicationControlSubsystem::restore_signal_handlers() noexcept {
  if (signal_handlers_ == nullptr) {
    return Status::OK;
  }

  Status status = Status::OK;
  if (::sigaction(SIGTERM, &signal_handlers_->termination, nullptr) != 0) {
    status = Status::SUBSYSTEM_FAILURE;
  }
  if (::sigaction(SIGINT, &signal_handlers_->interrupt, nullptr) != 0) {
    status = Status::SUBSYSTEM_FAILURE;
  }
  signal_handlers_.reset();
  if (signal_owner_ == this) {
    signal_owner_ = nullptr;
  }
  termination_signal_requested_ = 0;
  return status;
}

}  // namespace puc::app
