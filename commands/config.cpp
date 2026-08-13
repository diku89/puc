/**
 * @file config.cpp
 * @brief Properties inspection and mutation command implementation.
 */

#include "commands/config.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "properties/properties.hpp"

namespace puc::command {
namespace {

/** Translate service failures into the command result vocabulary. */
Status command_status(properties::Status status) noexcept {
  switch (status) {
    case properties::Status::OK:
      return Status::OK;
    case properties::Status::NOT_FOUND:
    case properties::Status::INVALID_PATH:
    case properties::Status::IMMUTABLE_PROPERTY:
    case properties::Status::UNSUPPORTED_VALUE:
    case properties::Status::TYPE_MISMATCH:
    case properties::Status::INVALID_VALUE:
      return Status::INVALID_ARGUMENT;
    case properties::Status::INVALID_ROOT:
    case properties::Status::NOT_REGULAR_FILE:
    case properties::Status::IO_ERROR:
    case properties::Status::PARSE_ERROR:
    case properties::Status::DUPLICATE_SOURCE:
    case properties::Status::DUPLICATE_PROPERTY:
      return Status::INTERNAL_ERROR;
  }
  return Status::INTERNAL_ERROR;
}

/** Publish one property value in the canonical list representation. */
Status publish_property(const CommonCommandArgs& common_args,
                        const properties::Property& property) {
  std::string text = property.name;
  text.push_back('\t');
  text.append(properties::scalar_text(property.value));
  return send_notification(common_args, std::move(text));
}

}  // namespace

Status ConfigCommand::run(CommonCommandArgs common_args,
                          std::span<const std::string> args) {
  if (common_args.properties == nullptr || args.empty()) {
    return Status::INVALID_ARGUMENT;
  }

  if (args[0] == "get") {
    if (args.size() != 2U) {
      return Status::INVALID_ARGUMENT;
    }
    properties::Property property;
    const properties::Status status =
        common_args.properties->get(args[1], property);
    return properties::is_ok(status) ? publish_property(common_args, property)
                                     : command_status(status);
  }

  if (args[0] == "set") {
    if (args.size() != 3U) {
      return Status::INVALID_ARGUMENT;
    }
    const properties::Status status =
        common_args.properties->set(args[1], args[2]);
    if (!properties::is_ok(status)) {
      return command_status(status);
    }
    properties::Property property;
    const properties::Status get_status =
        common_args.properties->get(args[1], property);
    return properties::is_ok(get_status)
               ? publish_property(common_args, property)
               : command_status(get_status);
  }

  if (args[0] == "list") {
    if (args.size() > 2U) {
      return Status::INVALID_ARGUMENT;
    }
    std::string_view prefix;
    if (args.size() == 2U) {
      prefix = args[1];
    }
    const std::vector<properties::Property> listed_properties =
        common_args.properties->list(prefix);
    std::string text;
    for (const properties::Property& property : listed_properties) {
      if (!text.empty()) {
        text.push_back('\n');
      }
      text.append(property.name);
      text.push_back('\t');
      text.append(properties::scalar_text(property.value));
    }
    return send_notification(common_args, std::move(text));
  }

  if (args[0] == "reload") {
    if (args.size() != 1U) {
      return Status::INVALID_ARGUMENT;
    }
    const properties::Status status = common_args.properties->reload();
    return properties::is_ok(status)
               ? send_notification(common_args, "Properties reloaded.")
               : command_status(status);
  }

  return Status::INVALID_ARGUMENT;
}

}  // namespace puc::command
