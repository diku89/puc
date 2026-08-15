/** @file turn_datastore.cpp @brief Pre-addressed committed-Turn storage. */

#include "canvas/protos/datastore/turn_datastore.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "canvas/turn/address.hpp"

namespace puc::canvas::datastore {
namespace {

constexpr std::size_t kCanvasUuidBytes = 16U;

constexpr std::array kMigrations{
    Migration{.version = 1U, .sql = R"sql(
CREATE TABLE turns (
  canvas_uuid BLOB NOT NULL,
  human_address TEXT NOT NULL,
  parent_address TEXT,
  sibling_ordinal INTEGER NOT NULL CHECK(sibling_ordinal > 0),
  payload BLOB NOT NULL,
  PRIMARY KEY(canvas_uuid, human_address),
  FOREIGN KEY(canvas_uuid) REFERENCES canvases(canvas_uuid)
) STRICT;
CREATE INDEX turns_by_parent
  ON turns(canvas_uuid, parent_address, sibling_ordinal);
)sql"},
};

std::span<const std::uint8_t> bytes(std::string_view value) {
  return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}

Status bind_parent(Statement& statement, std::int32_t index,
                   const proto::Turn& turn) {
  return turn.has_parent()
             ? statement.bind(index, turn.parent().human_address())
             : statement.bind_null(index);
}

bool valid_address(const proto::Turn& turn, const TurnAddress& address) {
  if (!turn.has_parent()) {
    return address.components().size() == 1U &&
           address.components().front().kind == AddressComponent::Kind::NUMERIC;
  }
  const auto parent = TurnAddress::parse(turn.parent().human_address());
  if (!parent.has_value() ||
      address.components().size() != parent->components().size() + 1U) {
    return false;
  }
  const AddressComponent& component = address.components().back();
  return component.kind == AddressComponent::Kind::NUMERIC &&
         parent->numeric_child(component.ordinal) == address;
}

}  // namespace

MigrationSet TurnDatastore::migrations() noexcept {
  return MigrationSet{.datastore = "canvas.turns", .migrations = kMigrations};
}

Status TurnDatastore::persist(std::span<const std::uint8_t> canvas_uuid,
                              const proto::Turn& submitted, proto::Turn& turn) {
  turn.Clear();
  const std::string_view canvas_bytes{
      reinterpret_cast<const char*>(canvas_uuid.data()), canvas_uuid.size()};
  if (canvas_uuid.size() != kCanvasUuidBytes || !submitted.has_id() ||
      !submitted.id().has_canvas_uuid() ||
      submitted.id().canvas_uuid() != canvas_bytes ||
      !submitted.id().has_human_address() || !submitted.has_actor() ||
      !submitted.actor().has_kind() ||
      submitted.actor().kind() == proto::Actor::UNSPECIFIED ||
      !submitted.has_payload() || !submitted.payload().has_kind() ||
      submitted.payload().kind() == proto::Payload::KIND_UNSPECIFIED ||
      (submitted.has_parent() &&
       (!submitted.parent().has_canvas_uuid() ||
        submitted.parent().canvas_uuid() != canvas_bytes ||
        !submitted.parent().has_human_address()))) {
    return Status::INVALID_ARGUMENT;
  }
  const auto address = TurnAddress::parse(submitted.id().human_address());
  if (!address.has_value() || !valid_address(submitted, *address) ||
      address->components().back().ordinal >
          std::numeric_limits<std::uint32_t>::max()) {
    return Status::INVALID_ARGUMENT;
  }

  turn = submitted;
  std::string payload;
  if (!turn.SerializeToString(&payload)) {
    turn.Clear();
    return Status::SERIALIZATION_ERROR;
  }

  const Database::Operation operation = database_.acquire();
  if (!is_ok(database_.begin_immediate())) {
    turn.Clear();
    return Status::SQL_ERROR;
  }

  if (turn.has_parent()) {
    Statement parent;
    if (!is_ok(
            database_.prepare("SELECT 1 FROM turns WHERE canvas_uuid = ?1 AND "
                              "human_address = ?2;",
                              parent)) ||
        !is_ok(parent.bind(1, canvas_uuid)) ||
        !is_ok(parent.bind(2, turn.parent().human_address())) ||
        parent.step() != Status::OK) {
      database_.rollback();
      turn.Clear();
      return Status::NOT_FOUND;
    }
  }

  Statement insert;
  if (!is_ok(database_.prepare(
          "INSERT INTO turns(canvas_uuid, human_address, parent_address, "
          "sibling_ordinal, payload) VALUES(?1, ?2, ?3, ?4, ?5);",
          insert)) ||
      !is_ok(insert.bind(1, canvas_uuid)) ||
      !is_ok(insert.bind(2, turn.id().human_address())) ||
      !is_ok(bind_parent(insert, 3, turn)) ||
      !is_ok(insert.bind(4, static_cast<std::int64_t>(
                                address->components().back().ordinal))) ||
      !is_ok(insert.bind(5, bytes(payload)))) {
    database_.rollback();
    turn.Clear();
    return Status::SQL_ERROR;
  }
  const Status inserted = insert.step();
  if (inserted != Status::NOT_FOUND) {
    database_.rollback();
    turn.Clear();
    return inserted == Status::ALREADY_EXISTS ? Status::ALREADY_EXISTS
                                              : Status::SQL_ERROR;
  }

  if (!is_ok(database_.commit())) {
    database_.rollback();
    turn.Clear();
    return Status::SQL_ERROR;
  }
  return Status::OK;
}

Status TurnDatastore::load_all(std::span<const std::uint8_t> canvas_uuid,
                               std::vector<proto::Turn>& turns) {
  turns.clear();
  if (canvas_uuid.size() != kCanvasUuidBytes) return Status::INVALID_ARGUMENT;
  const Database::Operation operation = database_.acquire();
  Statement select;
  if (!is_ok(database_.prepare(
          "SELECT payload FROM turns WHERE canvas_uuid = ?1;", select)) ||
      !is_ok(select.bind(1, canvas_uuid))) {
    return Status::SQL_ERROR;
  }
  while (true) {
    const Status status = select.step();
    if (status == Status::NOT_FOUND) return Status::OK;
    if (status != Status::OK) return Status::SQL_ERROR;
    proto::Turn turn;
    const std::string payload = select.blob(0);
    if (!turn.ParseFromString(payload)) {
      turns.clear();
      return Status::CORRUPT_DATA;
    }
    turns.push_back(std::move(turn));
  }
}

}  // namespace puc::canvas::datastore
