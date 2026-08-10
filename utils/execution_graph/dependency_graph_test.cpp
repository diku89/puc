/**
 * @file dependency_graph_test.cpp
 * @brief Tests for reusable dependency topology and traversal layers.
 */

#include "utils/execution_graph/dependency_graph.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace puc::execution_graph {
namespace {

static_assert(DependencyGraphNode<std::string>);
static_assert(!DependencyGraphNode<std::unique_ptr<int>>);

TEST(DependencyGraphTest, ValidatesRegistrationAndEdges) {
  DependencyGraph<std::string> graph;

  EXPECT_EQ(graph.add_node("root"), Status::OK);
  EXPECT_EQ(graph.add_node("leaf"), Status::OK);
  EXPECT_EQ(graph.add_node("root"), Status::DUPLICATE_NODE);
  EXPECT_EQ(graph.add_dependency("missing", "leaf"), Status::NODE_NOT_FOUND);
  EXPECT_EQ(graph.add_dependency("root", "missing"), Status::NODE_NOT_FOUND);
  EXPECT_EQ(graph.add_dependency("root", "leaf"), Status::OK);
  EXPECT_EQ(graph.add_dependency("root", "leaf"), Status::DUPLICATE_DEPENDENCY);
  EXPECT_EQ(graph.size(), 2U);
  EXPECT_EQ(graph.dependency_count(), 1U);
}

TEST(DependencyGraphTest, ProducesStableForwardAndReverseLayers) {
  DependencyGraph<std::string> graph;
  for (const std::string& node :
       {"root", "independent", "left", "right", "leaf"}) {
    ASSERT_EQ(graph.add_node(node), Status::OK);
  }
  ASSERT_EQ(graph.add_dependency("root", "left"), Status::OK);
  ASSERT_EQ(graph.add_dependency("root", "right"), Status::OK);
  ASSERT_EQ(graph.add_dependency("independent", "right"), Status::OK);
  ASSERT_EQ(graph.add_dependency("left", "leaf"), Status::OK);
  ASSERT_EQ(graph.add_dependency("right", "leaf"), Status::OK);

  DependencyGraph<std::string>::Layers forward;
  ASSERT_EQ(graph.forward_layers(forward), Status::OK);
  EXPECT_EQ(forward,
            (DependencyGraph<std::string>::Layers{
                {"root", "independent"}, {"left", "right"}, {"leaf"}}));

  DependencyGraph<std::string>::Layers reverse;
  ASSERT_EQ(graph.reverse_layers(reverse), Status::OK);
  EXPECT_EQ(reverse,
            (DependencyGraph<std::string>::Layers{
                {"leaf"}, {"left", "right"}, {"root", "independent"}}));
}

TEST(DependencyGraphTest, ExposesDenseValidatedSnapshot) {
  DependencyGraph<int> graph;
  ASSERT_EQ(graph.add_node(10), Status::OK);
  ASSERT_EQ(graph.add_node(20), Status::OK);
  ASSERT_EQ(graph.add_node(30), Status::OK);
  ASSERT_EQ(graph.add_dependency(10, 20), Status::OK);
  ASSERT_EQ(graph.add_dependency(10, 30), Status::OK);
  ASSERT_EQ(graph.add_dependency(20, 30), Status::OK);

  DependencyGraphSnapshot<int> snapshot;
  ASSERT_EQ(graph.snapshot(snapshot), Status::OK);
  EXPECT_EQ(snapshot.nodes, (std::vector<int>{10, 20, 30}));
  EXPECT_EQ(snapshot.dependents,
            (std::vector<std::vector<std::size_t>>{{1U, 2U}, {2U}, {}}));
  EXPECT_EQ(snapshot.dependency_counts, (std::vector<std::size_t>{0U, 1U, 2U}));
}

TEST(DependencyGraphTest, RejectsCyclesAndClearsOutputs) {
  DependencyGraph<int> graph;
  ASSERT_EQ(graph.add_node(1), Status::OK);
  ASSERT_EQ(graph.add_node(2), Status::OK);
  ASSERT_EQ(graph.add_dependency(1, 2), Status::OK);
  ASSERT_EQ(graph.add_dependency(2, 1), Status::OK);

  DependencyGraph<int>::Layers layers{{99}};
  EXPECT_EQ(graph.forward_layers(layers), Status::DEPENDENCY_CYCLE);
  EXPECT_TRUE(layers.empty());
  layers = {{99}};
  EXPECT_EQ(graph.reverse_layers(layers), Status::DEPENDENCY_CYCLE);
  EXPECT_TRUE(layers.empty());

  DependencyGraphSnapshot<int> snapshot{
      .nodes = {99}, .dependents = {{}}, .dependency_counts = {0U}};
  EXPECT_EQ(graph.snapshot(snapshot), Status::DEPENDENCY_CYCLE);
  EXPECT_TRUE(snapshot.nodes.empty());
  EXPECT_TRUE(snapshot.dependents.empty());
  EXPECT_TRUE(snapshot.dependency_counts.empty());
}

TEST(DependencyGraphTest, InvalidatesCachedLayersAfterMutation) {
  DependencyGraph<int> graph;
  ASSERT_EQ(graph.add_node(1), Status::OK);
  DependencyGraph<int>::Layers layers;
  ASSERT_EQ(graph.forward_layers(layers), Status::OK);
  EXPECT_EQ(layers, (DependencyGraph<int>::Layers{{1}}));

  ASSERT_EQ(graph.add_node(2), Status::OK);
  ASSERT_EQ(graph.add_dependency(1, 2), Status::OK);
  ASSERT_EQ(graph.forward_layers(layers), Status::OK);
  EXPECT_EQ(layers, (DependencyGraph<int>::Layers{{1}, {2}}));
}

TEST(DependencyGraphTest, AcceptsAnEmptyGraph) {
  DependencyGraph<int> graph;
  DependencyGraph<int>::Layers layers{{1}};
  EXPECT_EQ(graph.forward_layers(layers), Status::OK);
  EXPECT_TRUE(layers.empty());
  EXPECT_EQ(graph.reverse_layers(layers), Status::OK);
  EXPECT_TRUE(layers.empty());
}

}  // namespace
}  // namespace puc::execution_graph
