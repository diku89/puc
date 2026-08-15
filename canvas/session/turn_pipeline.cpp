/**
 * @file turn_pipeline.cpp
 * @brief Concurrent runtime-extensible Turn execution-plan implementation.
 */

#include "canvas/session/turn_pipeline.hpp"

#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "utils/execution_graph/dependency_graph.hpp"
#include "utils/execution_graph/execution_plan.hpp"
#include "utils/multithreading/job_queue.hpp"

namespace puc::canvas {
namespace {

/** Shared state retained until a synchronous process() callback returns. */
struct ProcessCompletion final {
  std::mutex mutex;                /**< Protects all completion state. */
  std::condition_variable changed; /**< Signals callback completion. */
  bool complete = false;           /**< Whether the callback published. */
  datastore::Status result =
      datastore::Status::INVALID_STATE; /**< Published pipeline status. */
  proto::Turn turn;                     /**< Published committed Turn. */
};

/** Adapt one registered callback to one run-local worker Job. */
class HandlerJob final : public multithreading::Job {
 public:
  HandlerJob(TurnPipeline::Handler handler,
             std::shared_ptr<TurnContext> context)
      : handler_(std::move(handler)), context_(std::move(context)) {}

  /** Invoke the callback unless an earlier dependency already failed. */
  void execute() noexcept override {
    if (!datastore::is_ok(context_->status())) {
      return;
    }
    try {
      handler_(*context_);
    } catch (...) {
      context_->fail(datastore::Status::INVALID_STATE);
    }
  }

 private:
  TurnPipeline::Handler handler_;        /**< Copied immutable-plan callback. */
  std::shared_ptr<TurnContext> context_; /**< Independent run context. */
};

}  // namespace

/** Mutable registration and lifecycle state hidden from the public facade. */
class TurnPipeline::Impl final {
 public:
  /** One registered node independent of an individual graph run. */
  struct Registration {
    NodeId node;                      /**< Stable graph identity. */
    Handler handler;                  /**< Work invoked for each Turn. */
    std::vector<NodeId> dependencies; /**< Required predecessor nodes. */
  };

  /** Immutable callbacks and validated topology retained by accepted runs. */
  struct CompiledPlan {
    std::vector<Registration> registrations; /**< Stable job ordering. */
    execution_graph::ExecutionPlan<NodeId> execution; /**< Shared topology. */
  };

  /** Compile all registrations and dependencies before publication. */
  static execution_graph::Status compile(
      const std::vector<Registration>& registrations,
      std::shared_ptr<const CompiledPlan>& output) {
    output.reset();
    if (registrations.empty()) {
      return execution_graph::Status::OK;
    }

    execution_graph::DependencyGraph<NodeId> topology;
    for (const Registration& registration : registrations) {
      const execution_graph::Status status =
          topology.add_node(registration.node);
      if (!execution_graph::is_ok(status)) {
        return status;
      }
    }
    for (const Registration& registration : registrations) {
      for (const NodeId& dependency : registration.dependencies) {
        const execution_graph::Status status =
            topology.add_dependency(dependency, registration.node);
        if (!execution_graph::is_ok(status)) {
          return status;
        }
      }
    }

    auto compiled           = std::make_shared<CompiledPlan>();
    compiled->registrations = registrations;
    const execution_graph::Status status =
        execution_graph::ExecutionPlan<NodeId>::compile(topology,
                                                        compiled->execution);
    if (!execution_graph::is_ok(status)) {
      return status;
    }
    output = std::move(compiled);
    return execution_graph::Status::OK;
  }

  /** Complete one accepted run and wake detach() after its callback exits. */
  void complete_run() noexcept {
    {
      const std::lock_guard lock(mutex);
      --active_runs;
    }
    drained.notify_all();
  }

  mutable std::mutex mutex; /**< Protects registration and lifecycle state. */
  std::condition_variable drained; /**< Signals zero accepted active runs. */
  multithreading::JobQueue* workers = nullptr; /**< Borrowed active executor. */
  bool accepting = false; /**< Whether submit() may accept another Turn. */
  std::size_t active_runs = 0U; /**< Accepted runs including completion. */
  std::vector<Registration> registrations; /**< Current registration order. */
  std::unordered_map<NodeId, std::size_t> indices; /**< Name lookup index. */
  std::shared_ptr<const CompiledPlan> plan; /**< Current immutable plan. */
};

void TurnContext::fail(datastore::Status status) noexcept {
  if (datastore::is_ok(status)) {
    return;
  }
  datastore::Status expected = datastore::Status::OK;
  static_cast<void>(status_.compare_exchange_strong(expected, status));
}

void TurnContext::reset(const proto::Turn& submitted) {
  submitted_ = submitted;
  turn_.Clear();
  status_.store(datastore::Status::OK);
  const std::lock_guard lock(state_mutex_);
  state_.clear();
}

TurnPipeline::TurnPipeline() : impl_(std::make_unique<Impl>()) {}

TurnPipeline::~TurnPipeline() { detach(); }

execution_graph::Status TurnPipeline::attach(
    multithreading::JobQueue& workers) noexcept {
  const std::lock_guard lock(impl_->mutex);
  if (impl_->workers != nullptr && impl_->workers != &workers) {
    return execution_graph::Status::INVALID_ARGUMENT;
  }
  impl_->workers   = &workers;
  impl_->accepting = true;
  return execution_graph::Status::OK;
}

void TurnPipeline::detach() noexcept {
  std::unique_lock lock(impl_->mutex);
  impl_->accepting = false;
  impl_->drained.wait(lock, [this] { return impl_->active_runs == 0U; });
  impl_->workers = nullptr;
}

execution_graph::Status TurnPipeline::register_node(
    NodeId node, Handler handler, std::vector<NodeId> dependencies) {
  const std::lock_guard lock(impl_->mutex);
  if (node.empty() || !handler) {
    return execution_graph::Status::INVALID_ARGUMENT;
  }
  if (impl_->indices.contains(node)) {
    return execution_graph::Status::DUPLICATE_NODE;
  }
  for (const NodeId& dependency : dependencies) {
    if (dependency.empty() || !impl_->indices.contains(dependency)) {
      return execution_graph::Status::NODE_NOT_FOUND;
    }
  }

  std::vector<Impl::Registration> replacement = impl_->registrations;
  replacement.push_back(
      Impl::Registration{.node         = std::move(node),
                         .handler      = std::move(handler),
                         .dependencies = std::move(dependencies)});
  std::shared_ptr<const Impl::CompiledPlan> compiled;
  const execution_graph::Status status = Impl::compile(replacement, compiled);
  if (!execution_graph::is_ok(status)) {
    return status;
  }

  impl_->registrations = std::move(replacement);
  impl_->indices.emplace(impl_->registrations.back().node,
                         impl_->registrations.size() - 1U);
  impl_->plan = std::move(compiled);
  return execution_graph::Status::OK;
}

execution_graph::Status TurnPipeline::unregister_node(std::string_view node) {
  const std::lock_guard lock(impl_->mutex);
  const auto found = impl_->indices.find(std::string{node});
  if (found == impl_->indices.end()) {
    return execution_graph::Status::NODE_NOT_FOUND;
  }
  for (const Impl::Registration& registration : impl_->registrations) {
    for (const NodeId& dependency : registration.dependencies) {
      if (dependency == node) {
        return execution_graph::Status::INVALID_ARGUMENT;
      }
    }
  }

  std::vector<Impl::Registration> replacement = impl_->registrations;
  replacement.erase(replacement.begin() +
                    static_cast<std::ptrdiff_t>(found->second));
  std::shared_ptr<const Impl::CompiledPlan> compiled;
  const execution_graph::Status status = Impl::compile(replacement, compiled);
  if (!execution_graph::is_ok(status)) {
    return status;
  }

  impl_->registrations = std::move(replacement);
  impl_->indices.clear();
  for (std::size_t index = 0U; index < impl_->registrations.size(); ++index) {
    impl_->indices.emplace(impl_->registrations[index].node, index);
  }
  impl_->plan = std::move(compiled);
  return execution_graph::Status::OK;
}

execution_graph::Status TurnPipeline::submit(const proto::Turn& submitted,
                                             Completion completion) {
  if (!completion) {
    return execution_graph::Status::INVALID_ARGUMENT;
  }

  std::shared_ptr<const Impl::CompiledPlan> plan;
  multithreading::JobQueue* workers = nullptr;
  {
    const std::lock_guard lock(impl_->mutex);
    if (!impl_->accepting || impl_->workers == nullptr ||
        impl_->plan == nullptr || !impl_->plan->execution.valid()) {
      return execution_graph::Status::INVALID_ARGUMENT;
    }
    plan    = impl_->plan;
    workers = impl_->workers;
    ++impl_->active_runs;
  }

  auto context = std::make_shared<TurnContext>();
  context->reset(submitted);
  std::vector<std::shared_ptr<multithreading::Job>> jobs;
  jobs.reserve(plan->registrations.size());
  for (const Impl::Registration& registration : plan->registrations) {
    jobs.push_back(std::make_shared<HandlerJob>(registration.handler, context));
  }

  const execution_graph::Status status = plan->execution.submit(
      *workers, std::move(jobs),
      [this, context, completion = std::move(completion)](
          execution_graph::Status run_status) mutable {
        if (!execution_graph::is_ok(run_status)) {
          context->fail(datastore::Status::INVALID_STATE);
        }
        proto::Turn committed;
        if (context->turn().has_id()) {
          committed = context->turn();
        }
        try {
          completion(context->status(), std::move(committed));
        } catch (...) {
        }
        impl_->complete_run();
      });
  if (!execution_graph::is_ok(status)) {
    impl_->complete_run();
  }
  return status;
}

datastore::Status TurnPipeline::process(const proto::Turn& submitted,
                                        proto::Turn& committed_turn) {
  committed_turn.Clear();
  {
    const std::lock_guard lock(impl_->mutex);
    if (impl_->workers != nullptr && impl_->workers->owns_current_thread()) {
      return datastore::Status::INVALID_STATE;
    }
  }

  auto completion = std::make_shared<ProcessCompletion>();
  const execution_graph::Status submitted_status = submit(
      submitted, [completion](datastore::Status status, proto::Turn turn) {
        {
          const std::lock_guard lock(completion->mutex);
          completion->result   = status;
          completion->turn     = std::move(turn);
          completion->complete = true;
        }
        completion->changed.notify_all();
      });
  if (!execution_graph::is_ok(submitted_status)) {
    return completion->result;
  }

  std::unique_lock lock(completion->mutex);
  completion->changed.wait(lock,
                           [&completion] { return completion->complete; });
  committed_turn = std::move(completion->turn);
  return completion->result;
}

std::size_t TurnPipeline::size() const noexcept {
  const std::lock_guard lock(impl_->mutex);
  return impl_->registrations.size();
}

}  // namespace puc::canvas
