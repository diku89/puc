#pragma once

/**
 * @file status.hpp
 * @brief Canvas persistence result values.
 */

#include <string_view>

namespace puc::canvas::datastore {

/** Expected outcomes from Canvas persistence operations. */
enum class Status {
  OK,                /**< The operation completed successfully. */
  INVALID_ARGUMENT,  /**< Input violated the operation's contract. */
  INVALID_STATE,     /**< Lifecycle state does not permit the operation. */
  OPEN_FAILED,       /**< SQLite could not open the configured database. */
  SQL_ERROR,         /**< SQLite rejected a non-migration operation. */
  MIGRATION_ERROR,   /**< A migration transaction could not complete. */
  MIGRATION_CHANGED, /**< An applied migration's checksum no longer matches. */
  NOT_FOUND,         /**< The requested durable object does not exist. */
  ALREADY_EXISTS,    /**< A durable identity or address already exists. */
  SERIALIZATION_ERROR, /**< A protobuf payload could not be encoded. */
  CORRUPT_DATA,        /**< Durable bytes violate an expected invariant. */
};

/** Return whether a datastore operation succeeded. */
constexpr bool is_ok(Status status) noexcept { return status == Status::OK; }

/** Return stable diagnostic text for every datastore status. */
constexpr std::string_view status_message(Status status) noexcept {
  switch (status) {
    case Status::OK:
      return "ok";
    case Status::INVALID_ARGUMENT:
      return "invalid argument";
    case Status::INVALID_STATE:
      return "invalid state";
    case Status::OPEN_FAILED:
      return "database open failed";
    case Status::SQL_ERROR:
      return "SQL error";
    case Status::MIGRATION_ERROR:
      return "migration error";
    case Status::MIGRATION_CHANGED:
      return "applied migration changed";
    case Status::NOT_FOUND:
      return "not found";
    case Status::ALREADY_EXISTS:
      return "already exists";
    case Status::SERIALIZATION_ERROR:
      return "serialization error";
    case Status::CORRUPT_DATA:
      return "corrupt data";
  }
  return "unknown canvas datastore error";
}

}  // namespace puc::canvas::datastore
