/**
 * @file orchestration_subsystem.cpp
 * @brief Presentation orchestration-node implementation.
 */

#include "canvas/session/orchestration_subsystem.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "canvas/canvas_subsystem.hpp"
#include "canvas/datastore_subsystem.hpp"
#include "canvas/presentation/presentation_tree.hpp"
#include "canvas/protos/datastore/database.hpp"
#include "canvas/protos/datastore/presentation_datastore.hpp"
#include "canvas/protos/presentation.pb.h"
#include "canvas/session/turn_pipeline.hpp"
#include "canvas/turn/address.hpp"
#include "utils/hash/sha256.hpp"
#include "utils/logger/logger.hpp"

/** @cond ORCHESTRATION_LOGGER_MODULE */
LOGGER_MODULE("Orchestration");
/** @endcond */

namespace puc::app {
namespace {

constexpr std::string_view kPendingPresentationState =
    "orchestration.pending_presentation";

}  // namespace

/** Hidden state shared by the two registered orchestration callbacks. */
class OrchestrationSubsystem::Impl {
 public:
  CanvasSubsystem* canvas = nullptr; /**< Borrowed owning Canvas adapter. */
  std::unique_ptr<canvas::datastore::PresentationDatastore>
      presentations; /**< Durable nodes and commits. */
  std::unique_ptr<canvas::PresentationTree>
      presentation_tree; /**< Current materialized presentation order. */
  std::mutex presentation_mutex; /**< Serializes one root advancement. */
  std::vector<std::uint8_t>
      presentation_uuid;   /**< Current Canvas-owned tree identity. */
  bool registered = false; /**< Whether both owned nodes are in the pipeline. */

  /** Derive the next immutable presentation root for the committed Turn. */
  void linearize(canvas::TurnContext& context) {
    canvas::PendingPresentation pending;
    {
      const std::lock_guard lock(presentation_mutex);
      const canvas::datastore::Status status =
          presentation_tree->prepare_insert(context.turn().id(), pending);
      if (!canvas::datastore::is_ok(status)) {
        context.fail(status);
        return;
      }
    }
    context.store(std::string{kPendingPresentationState}, std::move(pending));
  }

  /** Persist, materialize, and broadcast a prepared Presentation commit. */
  void commit(canvas::TurnContext& context) {
    std::optional<canvas::PendingPresentation> pending =
        context.take<canvas::PendingPresentation>(kPendingPresentationState);
    if (!pending.has_value()) {
      context.fail(canvas::datastore::Status::INVALID_STATE);
      return;
    }

    canvas::proto::Presentation update;
    {
      const std::lock_guard lock(presentation_mutex);
      if (pending->previous_root != presentation_tree->root()) {
        const canvas::datastore::Status prepared =
            presentation_tree->prepare_insert(context.turn().id(), *pending);
        if (!canvas::datastore::is_ok(prepared)) {
          context.fail(prepared);
          return;
        }
      }
      canvas::datastore::Status status = presentations->commit(
          presentation_uuid, pending->previous_root, pending->new_root,
          context.turn().id(), pending->nodes);
      if (status == canvas::datastore::Status::INVALID_STATE) {
        hashing::Hash256 durable_root;
        status = presentations->load_root(presentation_uuid, durable_root);
        if (canvas::datastore::is_ok(status)) {
          presentation_tree->reset(durable_root);
          status =
              presentation_tree->prepare_insert(context.turn().id(), *pending);
        }
        if (canvas::datastore::is_ok(status)) {
          status = presentations->commit(
              presentation_uuid, pending->previous_root, pending->new_root,
              context.turn().id(), pending->nodes);
        }
      }
      if (!canvas::datastore::is_ok(status)) {
        context.fail(status);
        return;
      }
      presentation_tree->commit(*pending);

      update.set_presentation_uuid(presentation_uuid.data(),
                                   presentation_uuid.size());
      update.set_canvas_uuid(canvas->canvas().canvas_uuid());
      if (!pending->previous_root.empty()) {
        update.set_previous_root_hash(pending->previous_root.bytes.data(),
                                      pending->previous_root.bytes.size());
      }
      update.set_root_hash(pending->new_root.bytes.data(),
                           pending->new_root.bytes.size());
      *update.mutable_committed_turn_id() = context.turn().id();
    }
    canvas->materialize_presentation(update);
    if (!canvas->publish_committed_presentation(update)) {
      Logger<WARN> << "Could not broadcast Presentation commit for Turn '"
                   << context.turn().id().human_address() << "'";
    }
  }

  /** Commit every durable Turn absent from the Presentation recovery ledger. */
  canvas::datastore::Status reconcile() {
    const std::lock_guard lock(presentation_mutex);
    std::vector<std::string> committed_addresses;
    canvas::datastore::Status status =
        presentations->load_committed_turn_addresses(presentation_uuid,
                                                     committed_addresses);
    if (!canvas::datastore::is_ok(status)) {
      return status;
    }
    const std::unordered_set<std::string> committed{committed_addresses.begin(),
                                                    committed_addresses.end()};

    std::vector<std::pair<canvas::TurnAddress, canvas::proto::TurnId>> missing;
    const canvas::proto::Canvas snapshot = canvas->canvas();
    for (const canvas::proto::Turn& turn : snapshot.turns()) {
      if (!turn.has_id() || !turn.id().has_canvas_uuid() ||
          turn.id().canvas_uuid() != snapshot.canvas_uuid() ||
          !turn.id().has_human_address()) {
        return canvas::datastore::Status::CORRUPT_DATA;
      }
      const auto address =
          canvas::TurnAddress::parse(turn.id().human_address());
      if (!address.has_value()) {
        return canvas::datastore::Status::CORRUPT_DATA;
      }
      if (!committed.contains(turn.id().human_address())) {
        missing.emplace_back(*address, turn.id());
      }
    }
    std::sort(missing.begin(), missing.end(),
              [](const auto& left, const auto& right) {
                return left.first < right.first;
              });

    std::optional<canvas::PendingPresentation> last_commit;
    for (const auto& [address, turn_id] : missing) {
      static_cast<void>(address);
      canvas::PendingPresentation recovered;
      status = presentation_tree->prepare_insert(turn_id, recovered);
      if (!canvas::datastore::is_ok(status)) {
        return status;
      }
      status =
          presentations->commit(presentation_uuid, recovered.previous_root,
                                recovered.new_root, turn_id, recovered.nodes);
      if (!canvas::datastore::is_ok(status)) {
        return status;
      }
      presentation_tree->commit(recovered);
      last_commit = std::move(recovered);
    }

    canvas::proto::Presentation restored;
    restored.set_presentation_uuid(presentation_uuid.data(),
                                   presentation_uuid.size());
    restored.set_canvas_uuid(snapshot.canvas_uuid());
    if (!presentation_tree->root().empty()) {
      restored.set_root_hash(presentation_tree->root().bytes.data(),
                             presentation_tree->root().bytes.size());
    }
    if (last_commit.has_value()) {
      if (!last_commit->previous_root.empty()) {
        restored.set_previous_root_hash(
            last_commit->previous_root.bytes.data(),
            last_commit->previous_root.bytes.size());
      }
      *restored.mutable_committed_turn_id() = last_commit->inserted_turn;
    }
    canvas->materialize_presentation(restored);
    return canvas::datastore::Status::OK;
  }
};

OrchestrationSubsystem::OrchestrationSubsystem()
    : AppSubsystem(
          "orchestration",
          subsystem_dependencies<CanvasSubsystem, DatastoreSubsystem>()),
      impl_(std::make_unique<Impl>()) {}

OrchestrationSubsystem::~OrchestrationSubsystem() = default;

Status OrchestrationSubsystem::initialize(AppState& app) {
  impl_->canvas                 = app.get_subsystem<CanvasSubsystem>();
  DatastoreSubsystem* datastore = app.get_subsystem<DatastoreSubsystem>();
  const canvas::proto::Canvas snapshot = impl_->canvas == nullptr
                                             ? canvas::proto::Canvas{}
                                             : impl_->canvas->canvas();
  if (impl_->canvas == nullptr || datastore == nullptr ||
      datastore->database() == nullptr ||
      impl_->canvas->pipeline() == nullptr ||
      !snapshot.has_presentation_uuid() ||
      snapshot.presentation_uuid().size() != 16U) {
    return Status::SUBSYSTEM_FAILURE;
  }
  impl_->presentation_uuid.assign(snapshot.presentation_uuid().begin(),
                                  snapshot.presentation_uuid().end());
  impl_->presentations =
      std::make_unique<canvas::datastore::PresentationDatastore>(
          *datastore->database());
  hashing::Hash256 root;
  if (!canvas::datastore::is_ok(
          impl_->presentations->load_root(impl_->presentation_uuid, root))) {
    impl_->presentations.reset();
    impl_->presentation_uuid.clear();
    impl_->canvas = nullptr;
    return Status::SUBSYSTEM_FAILURE;
  }
  impl_->presentation_tree = std::make_unique<canvas::PresentationTree>(
      *impl_->presentations, impl_->presentation_uuid, root);
  if (!canvas::datastore::is_ok(impl_->reconcile())) {
    impl_->presentation_tree.reset();
    impl_->presentations.reset();
    impl_->presentation_uuid.clear();
    impl_->canvas = nullptr;
    return Status::SUBSYSTEM_FAILURE;
  }

  canvas::TurnPipeline* pipeline = impl_->canvas->pipeline();
  if (!execution_graph::is_ok(pipeline->register_node(
          std::string{kLinearizeNode},
          [implementation = impl_.get()](canvas::TurnContext& context) {
            implementation->linearize(context);
          },
          {std::string{CanvasSubsystem::kUpdateTrieNode}}))) {
    impl_->presentation_tree.reset();
    impl_->presentations.reset();
    impl_->presentation_uuid.clear();
    impl_->canvas = nullptr;
    return Status::SUBSYSTEM_FAILURE;
  }
  if (!execution_graph::is_ok(pipeline->register_node(
          std::string{kCommitPresentationNode},
          [implementation = impl_.get()](canvas::TurnContext& context) {
            implementation->commit(context);
          },
          {std::string{kLinearizeNode}}))) {
    static_cast<void>(pipeline->unregister_node(kLinearizeNode));
    impl_->presentation_tree.reset();
    impl_->presentations.reset();
    impl_->presentation_uuid.clear();
    impl_->canvas = nullptr;
    return Status::SUBSYSTEM_FAILURE;
  }
  impl_->registered = true;
  return Status::OK;
}

Status OrchestrationSubsystem::start(AppState& app) {
  static_cast<void>(app);
  return impl_->registered && impl_->canvas != nullptr
             ? Status::OK
             : Status::SUBSYSTEM_FAILURE;
}

Status OrchestrationSubsystem::stop(AppState& app) noexcept {
  static_cast<void>(app);
  return Status::OK;
}

Status OrchestrationSubsystem::terminate(AppState& app) noexcept {
  static_cast<void>(app);
  Status result = Status::OK;
  if (impl_->registered && impl_->canvas != nullptr &&
      impl_->canvas->pipeline() != nullptr) {
    canvas::TurnPipeline* pipeline = impl_->canvas->pipeline();
    if (!execution_graph::is_ok(
            pipeline->unregister_node(kCommitPresentationNode)) ||
        !execution_graph::is_ok(pipeline->unregister_node(kLinearizeNode))) {
      result = Status::SUBSYSTEM_FAILURE;
    }
  }
  impl_->registered = false;
  impl_->presentation_tree.reset();
  impl_->presentations.reset();
  impl_->presentation_uuid.clear();
  impl_->canvas = nullptr;
  return result;
}

canvas::PresentationTree* OrchestrationSubsystem::presentation_tree() noexcept {
  return impl_->presentation_tree.get();
}

}  // namespace puc::app
