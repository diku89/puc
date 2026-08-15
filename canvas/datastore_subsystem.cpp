/**
 * @file datastore_subsystem.cpp
 * @brief Canvas datastore lifecycle implementation.
 */

#include "canvas/datastore_subsystem.hpp"

#include <array>
#include <filesystem>
#include <memory>
#include <system_error>

#include "canvas/protos/datastore/canvas_datastore.hpp"
#include "canvas/protos/datastore/database.hpp"
#include "canvas/protos/datastore/presentation_datastore.hpp"
#include "canvas/protos/datastore/turn_datastore.hpp"
#include "canvas/settings.hpp"
#include "properties/properties_subsystem.hpp"
#include "utils/logger/logger.hpp"
#include "utils/logger/logger_subsystem.hpp"

/** @cond DATASTORE_LOGGER_MODULE */
LOGGER_MODULE("Datastore");
/** @endcond */

namespace puc::app {

/** Hidden durable resources behind the datastore lifecycle adapter. */
class DatastoreSubsystem::Impl {
 public:
  canvas::datastore::Database database; /**< Sole application SQLite owner. */
};

DatastoreSubsystem::DatastoreSubsystem()
    : AppSubsystem(
          "datastore",
          subsystem_dependencies<PropertiesSubsystem, LoggerSubsystem>()),
      impl_(std::make_unique<Impl>()) {}

DatastoreSubsystem::~DatastoreSubsystem() = default;

Status DatastoreSubsystem::initialize(AppState& app) {
  PropertiesSubsystem* properties = app.get_subsystem<PropertiesSubsystem>();
  if (properties == nullptr || properties->properties() == nullptr) {
    Logger<ERROR> << "Application properties are unavailable";
    return Status::SUBSYSTEM_FAILURE;
  }

  canvas::Settings settings;
  if (!canvas::load_settings(*properties->properties(), settings)) {
    Logger<ERROR> << "Could not load Canvas datastore configuration";
    return Status::SUBSYSTEM_FAILURE;
  }

  std::error_code error;
  std::filesystem::create_directories(settings.database_path.parent_path(),
                                      error);
  if (error) {
    Logger<ERROR> << "Could not create datastore directory '"
                  << settings.database_path.parent_path().string()
                  << "': " << error.message();
    return Status::SUBSYSTEM_FAILURE;
  }

  const std::array migration_sets{
      canvas::datastore::CanvasDatastore::migrations(),
      canvas::datastore::TurnDatastore::migrations(),
      canvas::datastore::PresentationDatastore::migrations(),
  };
  const canvas::datastore::Status initialized =
      impl_->database.initialize(settings.database_path, migration_sets);
  if (!canvas::datastore::is_ok(initialized)) {
    if (impl_->database.last_error().empty()) {
      Logger<ERROR> << "Could not initialize datastore at '"
                    << settings.database_path.string()
                    << "': " << canvas::datastore::status_message(initialized);
    } else {
      Logger<ERROR> << "Could not initialize datastore at '"
                    << settings.database_path.string()
                    << "': " << canvas::datastore::status_message(initialized)
                    << "; " << impl_->database.last_error();
    }
    return Status::SUBSYSTEM_FAILURE;
  }
  return Status::OK;
}

Status DatastoreSubsystem::start(AppState& app) {
  static_cast<void>(app);
  return impl_->database.ready() ? Status::OK : Status::SUBSYSTEM_FAILURE;
}

Status DatastoreSubsystem::stop(AppState& app) noexcept {
  static_cast<void>(app);
  return Status::OK;
}

Status DatastoreSubsystem::terminate(AppState& app) noexcept {
  static_cast<void>(app);
  impl_->database.close();
  return Status::OK;
}

canvas::datastore::Database* DatastoreSubsystem::database() noexcept {
  return impl_->database.ready() ? &impl_->database : nullptr;
}

const canvas::datastore::Database* DatastoreSubsystem::database()
    const noexcept {
  return impl_->database.ready() ? &impl_->database : nullptr;
}

}  // namespace puc::app
