/** @file turn_tree.cpp @brief Turn reply-Trie materialization. */

#include "canvas/turn/turn_tree.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace puc::canvas {
namespace {

bool direct_child(const TurnAddress& parent, const TurnAddress& child) {
  if (child.components().size() != parent.components().size() + 1U) {
    return false;
  }
  const AddressComponent& component = child.components().back();
  return component.kind == AddressComponent::Kind::NUMERIC &&
         parent.numeric_child(component.ordinal) == child;
}

}  // namespace

TurnTree::Status TurnTree::reply_to(std::span<const std::uint8_t> canvas_uuid,
                                    const proto::TurnId* parent,
                                    proto::Turn& started) {
  started.Clear();
  if (canvas_uuid.size() != 16U) return Status::INVALID_TURN;

  Trie::NodeIndex parent_index = Trie::root();
  std::optional<TurnAddress> parent_address;
  if (parent != nullptr) {
    const std::string_view canvas_bytes{
        reinterpret_cast<const char*>(canvas_uuid.data()), canvas_uuid.size()};
    if (!parent->has_canvas_uuid() || parent->canvas_uuid() != canvas_bytes ||
        !parent->has_human_address()) {
      return Status::INVALID_TURN;
    }
    parent_address = TurnAddress::parse(parent->human_address());
    if (!parent_address.has_value()) return Status::INVALID_TURN;
    parent_index = trie_.find_node(parent_address->components());
    if (parent_index == Trie::kInvalidNode ||
        !trie_.node(parent_index).sequence_end) {
      return Status::PARENT_NOT_FOUND;
    }
  }

  std::atomic<std::uint32_t>& next =
      trie_.node(parent_index).value.next_reply_ordinal_;
  std::uint32_t ordinal = next.load(std::memory_order_relaxed);
  while (true) {
    if (ordinal == 0U) return Status::ADDRESS_EXHAUSTED;
    const std::uint32_t replacement =
        ordinal == std::numeric_limits<std::uint32_t>::max() ? 0U
                                                             : ordinal + 1U;
    if (next.compare_exchange_weak(ordinal, replacement,
                                   std::memory_order_relaxed)) {
      break;
    }
  }

  const TurnAddress address = parent_address.has_value()
                                  ? parent_address->numeric_child(ordinal)
                                  : TurnAddress::root(ordinal);
  started.mutable_id()->set_canvas_uuid(canvas_uuid.data(), canvas_uuid.size());
  started.mutable_id()->set_human_address(address.string());
  if (parent != nullptr) *started.mutable_parent() = *parent;
  return Status::OK;
}

TurnTree::Status TurnTree::apply(const proto::Turn& turn) {
  if (!turn.has_id() || !turn.id().has_human_address() ||
      !turn.id().has_canvas_uuid()) {
    return Status::INVALID_TURN;
  }
  const auto address = TurnAddress::parse(turn.id().human_address());
  if (!address.has_value() || turn.id().canvas_uuid().size() != 16U) {
    return Status::INVALID_TURN;
  }
  if (address->components().back().ordinal >
      std::numeric_limits<std::uint32_t>::max()) {
    return Status::ADDRESS_EXHAUSTED;
  }

  Trie::NodeIndex parent_index = Trie::root();
  if (turn.has_parent()) {
    if (!turn.parent().has_canvas_uuid() ||
        !turn.parent().has_human_address() ||
        turn.parent().canvas_uuid() != turn.id().canvas_uuid()) {
      return Status::INVALID_TURN;
    }
    const auto parent = TurnAddress::parse(turn.parent().human_address());
    if (!parent.has_value() || !direct_child(*parent, *address)) {
      return Status::INVALID_TURN;
    }
    parent_index = trie_.find_node(parent->components());
    if (parent_index == Trie::kInvalidNode ||
        !trie_.node(parent_index).sequence_end) {
      return Status::PARENT_NOT_FOUND;
    }
  } else if (address->components().size() != 1U ||
             address->components().front().kind !=
                 AddressComponent::Kind::NUMERIC) {
    return Status::INVALID_TURN;
  }

  const Trie::NodeIndex existing = trie_.find_node(address->components());
  if (existing != Trie::kInvalidNode && trie_.node(existing).sequence_end) {
    return Status::ALREADY_EXISTS;
  }

  const std::size_t old_size = trie_.size();
  trie_.insert(address->components(), TurnNode{turn});
  hashes_.resize(trie_.size());

  if (trie_.size() != old_size) {
    const std::uint64_t ordinal = address->components().back().ordinal;
    const auto reply_ordinal    = static_cast<std::uint32_t>(ordinal);
    const std::uint32_t desired =
        reply_ordinal == std::numeric_limits<std::uint32_t>::max()
            ? 0U
            : reply_ordinal + 1U;
    std::atomic<std::uint32_t>& next =
        trie_.node(parent_index).value.next_reply_ordinal_;
    std::uint32_t current = next.load(std::memory_order_relaxed);
    while (current != 0U && (desired == 0U || current < desired) &&
           !next.compare_exchange_weak(current, desired,
                                       std::memory_order_relaxed)) {
    }
  }

  std::vector<Trie::NodeIndex> path{Trie::root()};
  Trie::NodeIndex current = Trie::root();
  for (const AddressComponent& component : address->components()) {
    current = trie_.find_child(current, component);
    path.push_back(current);
  }
  for (auto node = path.rbegin(); node != path.rend(); ++node) {
    hashes_[*node] = hash_node(*node);
  }
  root_hash_ = hashes_[Trie::root()];
  return Status::OK;
}

TurnTree::Status TurnTree::rebuild(std::span<const proto::Turn> turns) {
  std::vector<const proto::Turn*> ordered;
  ordered.reserve(turns.size());
  for (const proto::Turn& turn : turns) {
    if (!turn.has_id() ||
        !TurnAddress::parse(turn.id().human_address()).has_value()) {
      return Status::INVALID_TURN;
    }
    ordered.push_back(&turn);
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const proto::Turn* left, const proto::Turn* right) {
              const auto left_address =
                  TurnAddress::parse(left->id().human_address());
              const auto right_address =
                  TurnAddress::parse(right->id().human_address());
              return *left_address < *right_address;
            });
  TurnTree candidate;
  for (const proto::Turn* turn : ordered) {
    const Status status = candidate.apply(*turn);
    if (status != Status::OK) return status;
  }
  *this = std::move(candidate);
  return Status::OK;
}

const proto::Turn* TurnTree::find(const TurnAddress& address) const {
  const TurnNode* found = trie_.find(address.components());
  return found == nullptr ? nullptr : &found->turn_;
}

hashing::Hash256 TurnTree::hash_node(Trie::NodeIndex index) const {
  const auto& node = trie_.node(index);
  std::string encoded{"PUC-TURN-TRIE-V1", 16U};
  encoded.push_back(node.sequence_end ? '\1' : '\0');
  if (node.sequence_end) {
    std::string payload;
    if (!node.value.turn_.SerializeToString(&payload)) return {};
    encoded.append(payload);
  }
  for (const auto child_index : node.children) {
    const auto& child = trie_.node(child_index);
    encoded.push_back(child.key.kind == AddressComponent::Kind::NUMERIC ? '\0'
                                                                        : '\1');
    for (std::size_t byte = 0U; byte < sizeof(child.key.ordinal); ++byte) {
      const std::size_t shift = (sizeof(child.key.ordinal) - byte - 1U) * 8U;
      encoded.push_back(static_cast<char>(child.key.ordinal >> shift));
    }
    encoded.append(
        reinterpret_cast<const char*>(hashes_[child_index].bytes.data()),
        hashes_[child_index].bytes.size());
  }
  return hashing::sha256(encoded);
}

}  // namespace puc::canvas
