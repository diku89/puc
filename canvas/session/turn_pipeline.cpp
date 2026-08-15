/**
 * @file turn_pipeline.cpp
 * @brief Runtime-extensible Turn-processing execution graph implementation.
 */

#include "canvas/session/turn_pipeline.hpp"

#include <algorithm>
#include <exception>
#include <memory>
#include <string>
#include <utility>

#include "utils/execution_graph/execution_graph.hpp"
#include "utils/multithreading/job_queue.hpp"

namespace puc::canvas {
namespace {

/** Adapt one registered callback to the worker Job interface. */
class HandlerJob final : public multithreading::Job {
 public:
  HandlerJob(TurnPipeline::Handler handler, TurnContext& context)
      : handler_(std::move(handler)), context_(context) {}

  /** Invoke the callback unless an earlier dependency already failed. */
  void execute() noexcept override {
    if (!datastore::is_ok(context_.status())) return;
    try {
      handler_(context_);
    } catch (...) {
      context_.fail(datastore::Status::INVALID_STATE);
    }
  }

 private:
  TurnPipeline::Handler handler_; /**< Copied registration callback. */
  TurnContext& context_;          /**< Run-local context owned by process(). */
};

}  // namespace

void TurnContext::fail(datastore::Status status) noexcept {
  if (datastore::is_ok(status)) return;
  datastore::Status expected = datastore::Status::OK;
  static_cast<void>(status_.compare_exchange_strong(expected, status));
}

void TurnContext::reset(const proto::Turn& submitted) {
  submitted_ = submitted;
  turn_.Clear();
  status_.store(datastore::Status::OK);
}

execution_graph::Status TurnPipeline::attach(
    multithreading::JobQueue& workers) noexcept {
  const std::lock_guard lock(mutex_);
  if (workers_ != nullptr && workers_ != &workers) {
    return execution_graph::Status::INVALID_ARGUMENT;
  }
  workers_ = &workers;
  return execution_graph::Status::OK;
}

void TurnPipeline::detach() noexcept {
  const std::lock_guard lock(mutex_);
  workers_ = nullptr;
}

execution_graph::Status TurnPipeline::register_node(
    NodeId node, Handler handler, std::vector<NodeId> dependencies) {
  const std::lock_guard lock(mutex_);
  if (node.empty() || !handler) {
    return execution_graph::Status::INVALID_ARGUMENT;
  }
  const auto named = [this](std::string_view candidate) {
    return std::find_if(registrations_.begin(), registrations_.end(),
                        [candidate](const Registration& registration) {
                          return registration.node == candidate;
                        });
  };
  if (named(node) != registrations_.end()) {
    return execution_graph::Status::DUPLICATE_NODE;
  }
  for (const NodeId& dependency : dependencies) {
    if (dependency.empty() || named(dependency) == registrations_.end()) {
      return execution_graph::Status::NODE_NOT_FOUND;
    }
  }
  registrations_.push_back(
      Registration{.node         = std::move(node),
                   .handler      = std::move(handler),
                   .dependencies = std::move(dependencies)});
  return execution_graph::Status::OK;
}

execution_graph::Status TurnPipeline::unregister_node(std::string_view node) {
  const std::lock_guard lock(mutex_);
  const auto found = std::find_if(registrations_.begin(), registrations_.end(),
                                  [node](const Registration& registration) {
                                    return registration.node == node;
                                  });
  if (found == registrations_.end()) {
    return execution_graph::Status::NODE_NOT_FOUND;
  }
  const bool required =
      std::any_of(registrations_.begin(), registrations_.end(),
                  [node](const Registration& registration) {
                    return std::find(registration.dependencies.begin(),
                                     registration.dependencies.end(),
                                     node) != registration.dependencies.end();
                  });
  if (required) return execution_graph::Status::INVALID_ARGUMENT;
  registrations_.erase(found);
  return execution_graph::Status::OK;
}

datastore::Status TurnPipeline::process(const proto::Turn& submitted,
                                        proto::Turn& committed_turn) {
  const std::lock_guard lock(mutex_);
  committed_turn.Clear();
  if (workers_ == nullptr || registrations_.empty()) {
    return datastore::Status::INVALID_STATE;
  }

  TurnContext context;
  context.reset(submitted);
  execution_graph::ExecutionGraph<NodeId> graph{*workers_};
  for (const Registration& registration : registrations_) {
    if (!execution_graph::is_ok(graph.add_node(
            registration.node,
            std::make_shared<HandlerJob>(registration.handler, context)))) {
      return datastore::Status::INVALID_STATE;
    }
  }
  for (const Registration& registration : registrations_) {
    for (const NodeId& dependency : registration.dependencies) {
      if (!execution_graph::is_ok(
              graph.add_dependency(dependency, registration.node))) {
        return datastore::Status::INVALID_STATE;
      }
    }
  }
  if (!execution_graph::is_ok(graph.start()) ||
      !execution_graph::is_ok(graph.wait())) {
    return datastore::Status::INVALID_STATE;
  }
  if (context.turn().has_id()) committed_turn = context.turn();
  return context.status();
}

std::size_t TurnPipeline::size() const noexcept {
  const std::lock_guard lock(mutex_);
  return registrations_.size();
}

}  // namespace puc::canvas
