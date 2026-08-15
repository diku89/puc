/**
 * @file canvas_datastore.cpp
 * @brief Durable Canvas aggregate ownership implementation.
 */

#include "canvas/protos/datastore/canvas_datastore.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace puc::canvas::datastore {
namespace {

constexpr std::size_t kUuidBytes = 16U;

constexpr std::array kMigrations{
    Migration{.version = 1U, .sql = R"sql(
CREATE TABLE turn_tries (
  turn_trie_uuid BLOB PRIMARY KEY CHECK(length(turn_trie_uuid) = 16),
  root_hash BLOB
) STRICT;
CREATE TABLE presentations (
  presentation_uuid BLOB PRIMARY KEY CHECK(length(presentation_uuid) = 16),
  root_hash BLOB
) STRICT;
CREATE TABLE canvases (
  canvas_uuid BLOB PRIMARY KEY CHECK(length(canvas_uuid) = 16),
  title TEXT,
  one_line_description TEXT,
  turn_trie_uuid BLOB NOT NULL UNIQUE,
  presentation_uuid BLOB NOT NULL UNIQUE,
  FOREIGN KEY(turn_trie_uuid) REFERENCES turn_tries(turn_trie_uuid),
  FOREIGN KEY(presentation_uuid) REFERENCES presentations(presentation_uuid)
) STRICT;
)sql"},
};

std::span<const std::uint8_t> bytes(std::string_view value) {
  return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}

bool valid_uuid(std::string_view value) { return value.size() == kUuidBytes; }

Status bind_optional(Statement& statement, std::int32_t index, bool present,
                     std::string_view value) {
  return present ? statement.bind(index, value) : statement.bind_null(index);
}

}  // namespace

MigrationSet CanvasDatastore::migrations() noexcept {
  return MigrationSet{.datastore = "canvas", .migrations = kMigrations};
}

Status CanvasDatastore::create(const proto::Canvas& canvas) {
  if (!canvas.has_canvas_uuid() || !valid_uuid(canvas.canvas_uuid()) ||
      !canvas.has_turn_trie_uuid() || !valid_uuid(canvas.turn_trie_uuid()) ||
      !canvas.has_presentation_uuid() ||
      !valid_uuid(canvas.presentation_uuid())) {
    return Status::INVALID_ARGUMENT;
  }
  const Database::Operation operation = database_.acquire();
  if (!is_ok(database_.begin_immediate())) return Status::SQL_ERROR;

  Statement turn_trie;
  Statement presentation;
  Statement aggregate;
  if (!is_ok(database_.prepare(
          "INSERT INTO turn_tries(turn_trie_uuid) VALUES(?1);", turn_trie)) ||
      !is_ok(turn_trie.bind(1, bytes(canvas.turn_trie_uuid()))) ||
      turn_trie.step() != Status::NOT_FOUND ||
      !is_ok(database_.prepare(
          "INSERT INTO presentations(presentation_uuid) VALUES(?1);",
          presentation)) ||
      !is_ok(presentation.bind(1, bytes(canvas.presentation_uuid()))) ||
      presentation.step() != Status::NOT_FOUND ||
      !is_ok(database_.prepare(
          "INSERT INTO canvases(canvas_uuid, title, one_line_description, "
          "turn_trie_uuid, presentation_uuid) VALUES(?1, ?2, ?3, ?4, ?5);",
          aggregate)) ||
      !is_ok(aggregate.bind(1, bytes(canvas.canvas_uuid()))) ||
      !is_ok(bind_optional(aggregate, 2, canvas.has_title(), canvas.title())) ||
      !is_ok(bind_optional(aggregate, 3, canvas.has_one_line_description(),
                           canvas.one_line_description())) ||
      !is_ok(aggregate.bind(4, bytes(canvas.turn_trie_uuid()))) ||
      !is_ok(aggregate.bind(5, bytes(canvas.presentation_uuid()))) ||
      aggregate.step() != Status::NOT_FOUND || !is_ok(database_.commit())) {
    database_.rollback();
    return Status::SQL_ERROR;
  }
  return Status::OK;
}

Status CanvasDatastore::first(proto::Canvas& canvas) {
  canvas.Clear();
  const Database::Operation operation = database_.acquire();
  Statement select;
  if (!is_ok(database_.prepare(
          "SELECT c.canvas_uuid, c.title, c.one_line_description, "
          "c.turn_trie_uuid, t.root_hash, c.presentation_uuid, p.root_hash "
          "FROM canvases c "
          "JOIN turn_tries t ON t.turn_trie_uuid = c.turn_trie_uuid "
          "JOIN presentations p ON p.presentation_uuid = c.presentation_uuid "
          "ORDER BY c.rowid LIMIT 1;",
          select))) {
    return Status::SQL_ERROR;
  }
  const Status status = select.step();
  if (status != Status::OK) return status;
  const std::string canvas_uuid       = select.blob(0);
  const std::string turn_trie_uuid    = select.blob(3);
  const std::string presentation_uuid = select.blob(5);
  if (!valid_uuid(canvas_uuid) || !valid_uuid(turn_trie_uuid) ||
      !valid_uuid(presentation_uuid)) {
    return Status::CORRUPT_DATA;
  }
  canvas.set_canvas_uuid(canvas_uuid);
  if (!select.is_null(1)) canvas.set_title(select.text(1));
  if (!select.is_null(2)) canvas.set_one_line_description(select.text(2));
  canvas.set_turn_trie_uuid(turn_trie_uuid);
  if (!select.is_null(4)) canvas.set_turn_trie_root_hash(select.blob(4));
  canvas.set_presentation_uuid(presentation_uuid);
  if (!select.is_null(6)) canvas.set_presentation_root_hash(select.blob(6));
  return Status::OK;
}

Status CanvasDatastore::update_turn_trie_root(
    std::span<const std::uint8_t> turn_trie_uuid,
    const hashing::Hash256& root) {
  if (turn_trie_uuid.size() != kUuidBytes || root.empty()) {
    return Status::INVALID_ARGUMENT;
  }
  const Database::Operation operation = database_.acquire();
  Statement update;
  if (!is_ok(database_.prepare(
          "UPDATE turn_tries SET root_hash = ?1 WHERE turn_trie_uuid = ?2;",
          update)) ||
      !is_ok(update.bind(1, root.bytes)) ||
      !is_ok(update.bind(2, turn_trie_uuid)) ||
      update.step() != Status::NOT_FOUND) {
    return Status::SQL_ERROR;
  }
  return Status::OK;
}

}  // namespace puc::canvas::datastore
