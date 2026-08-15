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
  return (component.kind == AddressComponent::Kind::NUMERIC
              ? parent.numeric_child(component.ordinal)
              : parent.alphabetic_child(component.ordinal)) == child;
}

void advance_past(std::atomic<std::uint32_t>& next, std::uint32_t ordinal) {
  const std::uint32_t desired =
      ordinal == std::numeric_limits<std::uint32_t>::max() ? 0U : ordinal + 1U;
  std::uint32_t current = next.load(std::memory_order_relaxed);
  while (current != 0U && (desired == 0U || current < desired) &&
         !next.compare_exchange_weak(current, desired,
                                     std::memory_order_relaxed)) {
  }
}

void preserve_watermark(const std::atomic<std::uint32_t>& current,
                        std::atomic<std::uint32_t>& rebuilt) {
  const std::uint32_t current_value = current.load(std::memory_order_relaxed);
  const std::uint32_t rebuilt_value = rebuilt.load(std::memory_order_relaxed);
  if (current_value == 0U || rebuilt_value == 0U) {
    rebuilt.store(0U, std::memory_order_relaxed);
  } else if (current_value > rebuilt_value) {
    rebuilt.store(current_value, std::memory_order_relaxed);
  }
}

}  // namespace

TurnTree::Status TurnTree::reply_to(std::span<const std::uint8_t> canvas_uuid,
                                    const proto::TurnId* parent,
                                    proto::Turn& started) {
  return allocate_child(canvas_uuid, parent, AddressComponent::Kind::NUMERIC,
                        started);
}

TurnTree::Status TurnTree::append_part(
    std::span<const std::uint8_t> canvas_uuid, const proto::TurnId& parent,
    proto::Turn& started) {
  return allocate_child(canvas_uuid, &parent,
                        AddressComponent::Kind::ALPHABETIC, started);
}

TurnTree::Status TurnTree::allocate_child(
    std::span<const std::uint8_t> canvas_uuid, const proto::TurnId* parent,
    AddressComponent::Kind kind, proto::Turn& started) {
  started.Clear();
  if (canvas_uuid.size() != 16U ||
      (kind == AddressComponent::Kind::ALPHABETIC && parent == nullptr)) {
    return Status::INVALID_TURN;
  }

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
    if (!parent_address.has_value()) {
      return Status::INVALID_TURN;
    }
    parent_index = trie_.find_node(parent_address->components());
    if (parent_index == Trie::kInvalidNode ||
        !trie_.node(parent_index).sequence_end) {
      return Status::PARENT_NOT_FOUND;
    }
  }

  std::atomic<std::uint32_t>& next =
      trie_.node(parent_index).value.allocator(kind);
  std::uint32_t ordinal = next.load(std::memory_order_relaxed);
  while (true) {
    if (ordinal == 0U) {
      return Status::ADDRESS_EXHAUSTED;
    }
    const std::uint32_t replacement =
        ordinal == std::numeric_limits<std::uint32_t>::max() ? 0U
                                                             : ordinal + 1U;
    if (next.compare_exchange_weak(ordinal, replacement,
                                   std::memory_order_relaxed)) {
      break;
    }
  }

  const TurnAddress address = !parent_address.has_value()
                                  ? TurnAddress::root(ordinal)
                              : kind == AddressComponent::Kind::NUMERIC
                                  ? parent_address->numeric_child(ordinal)
                                  : parent_address->alphabetic_child(ordinal);
  started.mutable_id()->set_canvas_uuid(canvas_uuid.data(), canvas_uuid.size());
  started.mutable_id()->set_human_address(address.string());
  if (parent != nullptr) {
    *started.mutable_parent() = *parent;
  }
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

  trie_.insert(address->components(), TurnNode{turn});
  hashes_.resize(trie_.size());

  const AddressComponent& child = address->components().back();
  advance_past(trie_.node(parent_index).value.allocator(child.kind),
               static_cast<std::uint32_t>(child.ordinal));

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
    if (status != Status::OK) {
      return status;
    }
  }
  preserve_reservations(candidate);
  *this = std::move(candidate);
  return Status::OK;
}

void TurnTree::preserve_reservations(TurnTree& rebuilt) const {
  const auto preserve_node = [](const TurnNode& current,
                                const TurnNode& candidate) {
    preserve_watermark(current.allocator(AddressComponent::Kind::NUMERIC),
                       candidate.allocator(AddressComponent::Kind::NUMERIC));
    preserve_watermark(current.allocator(AddressComponent::Kind::ALPHABETIC),
                       candidate.allocator(AddressComponent::Kind::ALPHABETIC));
  };

  preserve_node(trie_.node(Trie::root()).value,
                rebuilt.trie_.node(Trie::root()).value);
  for (const std::vector<AddressComponent>& address : trie_.completions()) {
    const Trie::NodeIndex current   = trie_.find_node(address);
    const Trie::NodeIndex candidate = rebuilt.trie_.find_node(address);
    if (current == Trie::kInvalidNode || candidate == Trie::kInvalidNode ||
        !trie_.node(current).sequence_end ||
        !rebuilt.trie_.node(candidate).sequence_end) {
      continue;
    }
    preserve_node(trie_.node(current).value,
                  rebuilt.trie_.node(candidate).value);
  }
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
    if (!node.value.turn_.SerializeToString(&payload)) {
      return {};
    }
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
