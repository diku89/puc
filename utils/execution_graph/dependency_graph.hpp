#pragma once

/**
 * @file dependency_graph.hpp
 * @brief Reusable directed-acyclic topology independent of job execution.
 */

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "utils/execution_graph/status.hpp"

namespace puc::execution_graph {

/**
 * Node identities accepted by dependency and execution graphs.
 *
 * Values are copied into a graph, compared for equality, and indexed by
 * `std::hash`. They need no default value, ordering, formatting, inheritance,
 * or executable behavior.
 */
template <typename NodeType>
concept DependencyGraphNode =
    std::copyable<NodeType> && std::equality_comparable<NodeType> &&
    requires(const NodeType& node) {
      { std::hash<NodeType>{}(node) } -> std::convertible_to<std::size_t>;
    };

/** Immutable dense topology returned after successful DAG validation. */
template <DependencyGraphNode NodeType>
struct DependencyGraphSnapshot {
  std::vector<NodeType> nodes; /**< Nodes in stable registration order. */
  std::vector<std::vector<std::size_t>>
      dependents; /**< Outgoing edges represented by dense node indexes. */
  std::vector<std::size_t>
      dependency_counts; /**< Incoming edge count for each dense node. */
};

/**
 * Store and validate a reusable directed dependency graph.
 *
 * An edge from `prerequisite` to `dependent` means the prerequisite must
 * complete first. Validation produces deterministic forward layers in stable
 * node-registration order. Reversing those layers yields a teardown order in
 * which every dependent precedes all of its prerequisites.
 *
 * Construction, validation, snapshots, and layer queries are thread-safe.
 * Successful validation is cached until another node or edge is added.
 */
template <DependencyGraphNode NodeType>
class DependencyGraph {
 public:
  using Layer  = std::vector<NodeType>; /**< Mutually independent nodes. */
  using Layers = std::vector<Layer>;    /**< Complete dependency ordering. */

  DependencyGraph() = default;

  DependencyGraph(const DependencyGraph&)            = delete;
  DependencyGraph& operator=(const DependencyGraph&) = delete;
  DependencyGraph(DependencyGraph&&)                 = delete;
  DependencyGraph& operator=(DependencyGraph&&)      = delete;

  /** Register one uniquely identified node in stable insertion order. */
  Status add_node(NodeType node) {
    const std::lock_guard lock(mutex_);
    if (node_indices_.contains(node)) {
      return Status::DUPLICATE_NODE;
    }
    const std::size_t index = nodes_.size();
    node_indices_.emplace(node, index);
    nodes_.push_back(std::move(node));
    dependents_.emplace_back();
    dependency_counts_.push_back(0U);
    topology_valid_ = false;
    return Status::OK;
  }

  /** Declare that `dependent` may follow only after `prerequisite`. */
  Status add_dependency(const NodeType& prerequisite,
                        const NodeType& dependent) {
    const std::lock_guard lock(mutex_);
    const auto prerequisite_entry = node_indices_.find(prerequisite);
    const auto dependent_entry    = node_indices_.find(dependent);
    if (prerequisite_entry == node_indices_.end() ||
        dependent_entry == node_indices_.end()) {
      return Status::NODE_NOT_FOUND;
    }

    std::vector<std::size_t>& dependents =
        dependents_[prerequisite_entry->second];
    if (std::find(dependents.begin(), dependents.end(),
                  dependent_entry->second) != dependents.end()) {
      return Status::DUPLICATE_DEPENDENCY;
    }
    dependents.push_back(dependent_entry->second);
    ++dependency_counts_[dependent_entry->second];
    ++edge_count_;
    topology_valid_ = false;
    return Status::OK;
  }

  /**
   * Validate the DAG and copy its dense adjacency representation.
   *
   * `output` is reset before validation and remains empty when a cycle exists.
   */
  Status snapshot(DependencyGraphSnapshot<NodeType>& output) const {
    const std::lock_guard lock(mutex_);
    output                  = {};
    const Status validation = validate_locked();
    if (!is_ok(validation)) {
      return validation;
    }
    output = DependencyGraphSnapshot<NodeType>{
        .nodes             = nodes_,
        .dependents        = dependents_,
        .dependency_counts = dependency_counts_,
    };
    return Status::OK;
  }

  /** Return dependency-first layers, clearing `output` on failure. */
  Status forward_layers(Layers& output) const {
    const std::lock_guard lock(mutex_);
    output.clear();
    const Status validation = validate_locked();
    if (!is_ok(validation)) {
      return validation;
    }
    output.reserve(forward_layer_indices_.size());
    for (const std::vector<std::size_t>& indexes : forward_layer_indices_) {
      Layer& layer = output.emplace_back();
      layer.reserve(indexes.size());
      for (const std::size_t index : indexes) {
        layer.push_back(nodes_[index]);
      }
    }
    return Status::OK;
  }

  /** Return dependent-first layers, clearing `output` on failure. */
  Status reverse_layers(Layers& output) const {
    Layers forward;
    const Status status = forward_layers(forward);
    if (!is_ok(status)) {
      output.clear();
      return status;
    }
    output.assign(forward.rbegin(), forward.rend());
    return Status::OK;
  }

  /** Return the number of registered nodes. */
  std::size_t size() const noexcept {
    const std::lock_guard lock(mutex_);
    return nodes_.size();
  }

  /** Return the number of registered directed edges. */
  std::size_t dependency_count() const noexcept {
    const std::lock_guard lock(mutex_);
    return edge_count_;
  }

 private:
  /** Validate acyclicity and cache stable forward layer indexes. */
  Status validate_locked() const {
    if (topology_valid_) {
      return Status::OK;
    }

    std::vector<std::size_t> remaining_dependencies = dependency_counts_;
    std::vector<std::size_t> ready;
    ready.reserve(nodes_.size());
    for (std::size_t index = 0U; index < nodes_.size(); ++index) {
      if (remaining_dependencies[index] == 0U) {
        ready.push_back(index);
      }
    }

    std::vector<std::vector<std::size_t>> layers;
    std::size_t visited = 0U;
    while (!ready.empty()) {
      std::sort(ready.begin(), ready.end());
      layers.push_back(ready);
      visited += ready.size();

      std::vector<std::size_t> next;
      for (const std::size_t index : ready) {
        for (const std::size_t dependent : dependents_[index]) {
          --remaining_dependencies[dependent];
          if (remaining_dependencies[dependent] == 0U) {
            next.push_back(dependent);
          }
        }
      }
      ready = std::move(next);
    }

    if (visited != nodes_.size()) {
      forward_layer_indices_.clear();
      return Status::DEPENDENCY_CYCLE;
    }
    forward_layer_indices_ = std::move(layers);
    topology_valid_        = true;
    return Status::OK;
  }

  mutable std::mutex mutex_;    /**< Protects topology and validation cache. */
  std::vector<NodeType> nodes_; /**< Stable dense node-registration order. */
  std::unordered_map<NodeType, std::size_t>
      node_indices_; /**< Node-to-dense-index lookup. */
  std::vector<std::vector<std::size_t>>
      dependents_; /**< Outgoing adjacency list by dense index. */
  std::vector<std::size_t>
      dependency_counts_;       /**< Incoming edge count by dense index. */
  std::size_t edge_count_ = 0U; /**< Number of registered directed edges. */
  mutable bool topology_valid_ =
      false; /**< Whether the layer cache is valid. */
  mutable std::vector<std::vector<std::size_t>>
      forward_layer_indices_; /**< Cached dependency-first layer indexes. */
};

}  // namespace puc::execution_graph
