#pragma once

/**
 * @file trie.hpp
 * @brief Contiguous, allocation-free-lookup trie for key-sequence matching.
 */

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

namespace puc {
namespace containers {

/**
 * Key type accepted by Trie.
 *
 * Trie keys must support default construction for the sentinel root, copying
 * when a new node is appended, and total ordering so every child list remains
 * sorted for lookup and deterministic traversal.
 *
 * @tparam Type Candidate key type.
 */
template <typename Type>
concept TrieKey = std::regular<Type> && std::totally_ordered<Type>;

/**
 * Value type accepted by Trie.
 *
 * Values are stored directly in every node. A caller that wants indirect or
 * shared ownership can therefore select a pointer or smart pointer as the
 * value type without the trie imposing that policy.
 *
 * @tparam Type Candidate value type.
 */
template <typename Type>
concept TrieValue = std::default_initializable<Type> && std::movable<Type>;

/**
 * One node in a Trie-owned contiguous node array.
 *
 * Each child is an index into the owning trie's node array. Index zero is the
 * sentinel root, so its key does not represent a sequence element.
 * `sequence_end` and `children` independently describe whether a node is an
 * exact match and whether it is a prefix of longer matches.
 *
 * @tparam KeyType Element type used to form lookup sequences.
 * @tparam ValueType Value stored directly at complete sequences.
 */
template <TrieKey KeyType, TrieValue ValueType>
struct TrieNode {
  KeyType key{}; /**< Key labeling the edge from the parent to this node. */
  std::vector<std::size_t>
      children;              /**< Child indexes sorted by their nodes' keys. */
  bool sequence_end = false; /**< Whether this node is an exact match. */
  ValueType value{}; /**< Value meaningful when `sequence_end` is true. */
};

/**
 * Store direct values at nodes reached by arbitrary key sequences.
 *
 * All nodes occupy one append-only vector. Callers traverse with stable node
 * indexes rather than pointers, and child indexes remain valid when insertion
 * reallocates the node vector. References and pointers returned for inspection
 * remain valid only until the next insertion.
 *
 * Each node's child-index vector is kept in ascending key order when a missing
 * edge is inserted. `find_child()` scans lists of at most
 * `kLinearSearchMaximumChildren` entries and uses binary search for larger
 * fan-outs. Completion traversal consequently produces lexicographic results
 * without sorting during lookup.
 *
 * The caller is responsible for synchronization when a trie is accessed from
 * multiple threads. Exact lookup methods perform no dynamic allocation or
 * mutation; completion lookup allocates only its returned key sequences. Node
 * and child-vector growth occur only during `insert()`.
 *
 * @tparam KeyType Regular, totally ordered sequence element.
 * @tparam ValueType Default-initializable, movable directly stored value.
 */
template <TrieKey KeyType, TrieValue ValueType>
class Trie {
 public:
  /** Public node representation. */
  using Node = TrieNode<KeyType, ValueType>;

  /** Stable index into the node vector. */
  using NodeIndex = std::size_t;

  static constexpr NodeIndex kRootNode = 0; /**< Sentinel root node index. */
  static constexpr NodeIndex kInvalidNode =
      std::numeric_limits<NodeIndex>::max(); /**< Missing-node result. */
  /** Largest child list searched linearly before lookup uses binary search. */
  static constexpr std::size_t kLinearSearchMaximumChildren = 8U;

  /** Construct an empty trie containing only its sentinel root. */
  Trie() { nodes_.emplace_back(); }

  /** Destroy the contiguous node array and directly stored values. */
  ~Trie() = default;

  /**
   * Return the sentinel root index used to begin traversal.
   *
   * @return Always `kRootNode`.
   */
  static constexpr NodeIndex root() noexcept { return kRootNode; }

  /**
   * Return the number of nodes, including the sentinel root.
   *
   * @return Current node count.
   */
  NodeIndex size() const noexcept { return nodes_.size(); }

  /**
   * Inspect a node by index.
   *
   * The returned reference may be invalidated by `insert()` because insertion
   * may reallocate the node vector. The index itself remains valid.
   *
   * @param[in] node_index Valid node index obtained from this trie.
   * @return Read-only node at `node_index`.
   * @pre `node_index < size()`.
   */
  const Node& node(NodeIndex node_index) const noexcept {
    return nodes_[node_index];
  }

  /**
   * Find the node reached by a sequence path.
   *
   * The result may be prefix-only. Inspect its `sequence_end` member to test
   * for an exact match and its children to test for longer matches.
   *
   * @param[in] key_sequence Sequence path to traverse from the root.
   * @return Final node index, or `kInvalidNode` at the first missing key.
   */
  NodeIndex find_node(const std::vector<KeyType>& key_sequence) const {
    NodeIndex current = kRootNode;
    for (const KeyType& key : key_sequence) {
      current = find_child(current, key);
      if (current == kInvalidNode) {
        return kInvalidNode;
      }
    }
    return current;
  }

  /**
   * Find the directly stored value for an exact key sequence.
   *
   * The returned pointer refers into the node vector and may be invalidated by
   * `insert()`. A non-null pointer can itself point to a null smart-pointer
   * value; `sequence_end`, rather than value nullness, defines an exact match.
   *
   * @param[in] key_sequence Sequence to look up.
   * @return Stored value address for an exact match, or `nullptr` otherwise.
   */
  const ValueType* find(const std::vector<KeyType>& key_sequence) const {
    const NodeIndex node_index = find_node(key_sequence);
    if (node_index == kInvalidNode || !nodes_[node_index].sequence_end) {
      return nullptr;
    }
    return &nodes_[node_index].value;
  }

  /**
   * Find one direct child without allocating or modifying the trie.
   *
   * Small child lists use a linear scan; lists above
   * `kLinearSearchMaximumChildren` use binary search over their insertion-time
   * ordering.
   *
   * @param[in] node_index Parent node index.
   * @param[in] key Key to match against the parent's children.
   * @return Matching child index, or `kInvalidNode` when absent or invalid.
   */
  NodeIndex find_child(NodeIndex node_index, const KeyType& key) const {
    if (node_index >= nodes_.size()) {
      return kInvalidNode;
    }

    const std::vector<NodeIndex>& children = nodes_[node_index].children;
    if (children.size() <= kLinearSearchMaximumChildren) {
      for (const NodeIndex child_index : children) {
        if (nodes_[child_index].key == key) {
          return child_index;
        }
      }
      return kInvalidNode;
    }

    const auto candidate = std::lower_bound(
        children.begin(), children.end(), key,
        [this](NodeIndex child_index, const KeyType& sought_key) {
          return nodes_[child_index].key < sought_key;
        });
    return candidate != children.end() && nodes_[*candidate].key == key
               ? *candidate
               : kInvalidNode;
  }

  /**
   * Insert a sequence or replace its existing terminal value.
   *
   * Missing nodes are appended in traversal order. Existing prefix and
   * descendant indexes remain unchanged when a value is inserted or replaced.
   * Each missing edge is placed into its parent's sorted child-index vector.
   *
   * @param[in] key_sequence Sequence to store; an empty sequence selects root.
   * @param[in] value Value moved into the sequence's terminal node.
   * @return Stable terminal node index.
   */
  NodeIndex insert(const std::vector<KeyType>& key_sequence, ValueType value) {
    NodeIndex current = kRootNode;
    for (const KeyType& key : key_sequence) {
      NodeIndex child = find_child(current, key);
      if (child == kInvalidNode) {
        child = nodes_.size();
        nodes_.emplace_back();
        nodes_.back().key                = key;
        std::vector<NodeIndex>& children = nodes_[current].children;
        const auto position              = std::lower_bound(
            children.begin(), children.end(), key,
            [this](NodeIndex child_index, const KeyType& inserted_key) {
              return nodes_[child_index].key < inserted_key;
            });
        children.insert(position, child);
      }
      current = child;
    }
    nodes_[current].sequence_end = true;
    nodes_[current].value        = std::move(value);
    return current;
  }

  /**
   * Remove the value associated with an exact sequence.
   *
   * Nodes are intentionally retained so indexes held by active cursors remain
   * valid and a later insertion can reuse the existing path. Descendant
   * sequences are unaffected.
   *
   * @param[in] key_sequence Exact sequence whose value should be removed.
   * @return `true` when an exact value existed and was removed.
   */
  bool erase(const std::vector<KeyType>& key_sequence) {
    const NodeIndex node_index = find_node(key_sequence);
    if (node_index == kInvalidNode || !nodes_[node_index].sequence_end) {
      return false;
    }
    nodes_[node_index].sequence_end = false;
    nodes_[node_index].value        = ValueType{};
    return true;
  }

  /**
   * Test whether a sequence terminates at an exact-match node.
   *
   * This tests `sequence_end`, not the stored value. An exact sequence holding
   * a null smart pointer therefore still counts as present.
   *
   * @param[in] key_sequence Sequence to inspect.
   * @return `true` only for an inserted complete sequence.
   */
  bool contains(const std::vector<KeyType>& key_sequence) const {
    const NodeIndex node_index = find_node(key_sequence);
    return node_index != kInvalidNode && nodes_[node_index].sequence_end;
  }

  /**
   * Return every stored sequence beginning with a prefix.
   *
   * Results are complete sequences from the root in lexicographic key order,
   * independent of insertion order. An exact prefix is included when it is a
   * stored sequence, followed by any longer descendants. An empty prefix
   * enumerates the entire trie, including the empty sequence when one was
   * inserted.
   *
   * @param[in] prefix Sequence path whose terminal descendants to collect.
   * @return Complete stored sequences below `prefix`, or an empty vector when
   *         the prefix is absent.
   */
  std::vector<std::vector<KeyType>> completions(
      const std::vector<KeyType>& prefix = {}) const {
    std::vector<std::vector<KeyType>> result;
    const NodeIndex prefix_node = find_node(prefix);
    if (prefix_node == kInvalidNode) {
      return result;
    }

    std::vector<KeyType> current_sequence = prefix;
    collect_completions(prefix_node, current_sequence, result);
    return result;
  }

 private:
  /** Append terminal descendants of one valid node in sorted branch order. */
  void collect_completions(
      NodeIndex node_index, std::vector<KeyType>& current_sequence,
      std::vector<std::vector<KeyType>>& completions) const {
    if (nodes_[node_index].sequence_end) {
      completions.push_back(current_sequence);
    }
    for (const NodeIndex child_index : nodes_[node_index].children) {
      current_sequence.push_back(nodes_[child_index].key);
      collect_completions(child_index, current_sequence, completions);
      current_sequence.pop_back();
    }
  }

  /** Append-only storage whose indexes identify every node. */
  std::vector<Node> nodes_;
};

}  // namespace containers
}  // namespace puc
