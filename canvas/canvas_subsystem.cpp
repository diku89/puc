/**
 * @file canvas_subsystem.cpp
 * @brief Canvas lifecycle, persistence nodes, and IPC ingestion.
 */

#include "canvas/canvas_subsystem.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "canvas/canvas_id.hpp"
#include "canvas/datastore_subsystem.hpp"
#include "canvas/protos/channels.pb.h"
#include "canvas/protos/datastore/canvas_datastore.hpp"
#include "canvas/protos/datastore/database.hpp"
#include "canvas/protos/datastore/turn_datastore.hpp"
#include "canvas/protos/turn.pb.h"
#include "canvas/session/turn_pipeline.hpp"
#include "canvas/turn/turn_tree.hpp"
#include "utils/ipc/channel_path.hpp"
#include "utils/ipc/directory.hpp"
#include "utils/ipc/directory_subsystem.hpp"
#include "utils/ipc/smem_channel.hpp"
#include "utils/logger/logger.hpp"
#include "utils/multithreading/worker_subsystem.hpp"

/** @cond CANVAS_LOGGER_MODULE */
LOGGER_MODULE("Canvas");
/** @endcond */

namespace puc::app {
namespace {

/** Resolve one compile-time relative route beneath a Canvas namespace root. */
std::optional<std::string> resolve_route(std::string_view root,
                                         std::string_view relative) {
  const auto path = ipc::RelativeChannelPath::parse(relative);
  return path.has_value() ? path->resolve(root) : std::nullopt;
}

/** Open a shared global route once, or borrow the existing Directory entry. */
std::shared_ptr<ipc::Channel> open_shared_channel(ipc::Directory& directory,
                                                  std::string_view name,
                                                  std::size_t maximum_bytes) {
  if (std::shared_ptr<ipc::Channel> existing = directory.get_channel(name);
      existing != nullptr) {
    return existing;
  }
  auto candidate =
      std::make_shared<ipc::SmemChannel>(std::string{name}, maximum_bytes);
  ipc::ChannelId ignored   = 0U;
  const ipc::Status opened = directory.open_channel(candidate, ignored);
  if (ipc::is_ok(opened)) return candidate;
  return opened == ipc::Status::DUPLICATE_CHANNEL
             ? directory.get_channel(name)
             : std::shared_ptr<ipc::Channel>{};
}

/** Serialize and transmit one protobuf message over an open channel. */
template <typename Message>
bool publish_proto(const std::shared_ptr<ipc::Channel>& channel,
                   const Message& message) noexcept {
  try {
    std::string payload;
    if (channel == nullptr || !message.SerializeToString(&payload)) {
      return false;
    }
    const auto bytes = std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()};
    return ipc::is_ok(channel->transmit(bytes).status);
  } catch (...) {
    return false;
  }
}

/** Build and broadcast one Canvas namespace lifecycle announcement. */
bool publish_announcement(
    const std::shared_ptr<ipc::Channel>& channel, std::string_view canvas_uuid,
    std::string_view channel_root,
    canvas::proto::CanvasChannelAnnouncement::State state) noexcept {
  canvas::proto::CanvasChannelAnnouncement announcement;
  announcement.set_canvas_uuid(canvas_uuid);
  announcement.set_channel_root(channel_root);
  announcement.set_state(state);
  announcement.set_protocol_version(CanvasSubsystem::kChannelProtocolVersion);
  return publish_proto(channel, announcement);
}

}  // namespace

/** Hidden mutable resources behind the Canvas lifecycle adapter. */
class CanvasSubsystem::Impl {
 public:
  canvas::datastore::Database* database =
      nullptr; /**< Borrowed application SQLite handle. */
  std::unique_ptr<canvas::datastore::CanvasDatastore>
      canvases; /**< Aggregate and Merkle-root persistence. */
  std::unique_ptr<canvas::datastore::TurnDatastore>
      turns; /**< Atomic numbering and Turn persistence. */
  std::unique_ptr<canvas::TurnPipeline>
      pipeline; /**< Runtime-extensible graph registration surface. */
  canvas::proto::Canvas canvas;   /**< Materialized aggregate descriptor. */
  canvas::TurnTree turn_tree;     /**< Materialized reply topology. */
  std::vector<std::uint8_t> uuid; /**< Current Canvas identity bytes. */
  std::vector<std::uint8_t>
      turn_trie_uuid; /**< Current Canvas-owned Turn Trie identity. */
  ipc::Directory* directory = nullptr; /**< Borrowed active IPC directory. */
  std::shared_ptr<ipc::Channel>
      announcement_channel; /**< Shared Canvas discovery event transport. */
  std::shared_ptr<ipc::Channel>
      query_channel; /**< Shared late-subscriber discovery query transport. */
  std::shared_ptr<ipc::Channel>
      turn_submission_channel; /**< Owned unnumbered-Turn transport. */
  std::shared_ptr<ipc::Channel>
      committed_turn_channel; /**< Owned durable-Turn broadcast transport. */
  std::shared_ptr<ipc::Channel> committed_presentation_channel; /**< Owned
                                        durable-Presentation transport. */
  ipc::Subscription
      turn_subscription; /**< Active Turn-submission subscription. */
  ipc::Subscription
      query_subscription;   /**< Active current-state query subscription. */
  std::string channel_root; /**< Stable absolute Canvas namespace root. */
  std::string turn_submission_name;        /**< Unnumbered-Turn input route. */
  std::string committed_turn_name;         /**< Durable-Turn output route. */
  std::string committed_presentation_name; /**< Durable-Presentation route. */
  std::mutex mutex; /**< Protects pump control state and the pending queue. */
  std::condition_variable changed; /**< Signals queued work or shutdown. */
  std::deque<canvas::proto::Turn> pending; /**< FIFO submitted-Turn queue. */
  std::mutex numbering_mutex; /**< Serializes address allocation and insert. */
  std::condition_variable numbering_changed; /**< Advances FIFO numbering. */
  std::uint64_t next_numbering_ticket = 0U;  /**< Next accepted FIFO ticket. */
  mutable std::mutex
      state_mutex;        /**< Protects Trie and Canvas materialization. */
  bool accepting = false; /**< Whether callbacks may enqueue new Turns. */
  bool closing   = false; /**< Whether the pump exits after draining. */
  bool announced = false; /**< Whether OPENED was successfully broadcast. */
  std::thread pump;       /**< Serialized IPC-to-graph ingestion thread. */

  /** Copy one submitted Turn out of the IPC callback and wake the pump. */
  void enqueue(ipc::Channel::Bytes bytes) noexcept {
    try {
      canvas::proto::Turn turn;
      if (!turn.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
        return;
      }
      const std::lock_guard lock(mutex);
      if (!accepting) return;
      pending.push_back(std::move(turn));
      changed.notify_one();
    } catch (...) {
    }
  }

  /** Submit queued Turns without waiting for their independent graph runs. */
  void run() noexcept {
    while (true) {
      canvas::proto::Turn submitted;
      {
        std::unique_lock lock(mutex);
        changed.wait(lock, [this] { return closing || !pending.empty(); });
        if (pending.empty() && closing) return;
        submitted = std::move(pending.front());
        pending.pop_front();
      }
      const execution_graph::Status status =
          pipeline->submit(submitted, [](canvas::datastore::Status run_status,
                                         canvas::proto::Turn) noexcept {
            if (!canvas::datastore::is_ok(run_status)) {
              Logger<ERROR> << "Turn graph failed: "
                            << canvas::datastore::status_message(run_status);
            }
          });
      if (!execution_graph::is_ok(status)) {
        Logger<ERROR> << "Could not submit Turn graph: "
                      << execution_graph::status_message(status);
      }
    }
  }

  /** Stop accepting work and synchronously drain the IPC pump. */
  void stop_pump() noexcept {
    turn_subscription.reset();
    {
      const std::lock_guard lock(mutex);
      accepting = false;
      closing   = true;
      changed.notify_all();
    }
    if (pump.joinable()) pump.join();
    if (pipeline != nullptr) pipeline->detach();
    {
      const std::lock_guard lock(mutex);
      pending.clear();
      closing = false;
    }
  }

  /** Broadcast this Canvas namespace in one lifecycle state. */
  bool announce(
      canvas::proto::CanvasChannelAnnouncement::State state) const noexcept {
    const std::string_view canvas_uuid{
        reinterpret_cast<const char*>(uuid.data()), uuid.size()};
    return publish_announcement(announcement_channel, canvas_uuid, channel_root,
                                state);
  }

  /** Atomically number and persist the submitted Turn. */
  void number_and_persist(canvas::TurnContext& context) {
    std::unique_lock lock(numbering_mutex);
    numbering_changed.wait(lock, [this, &context] {
      return context.ingress_ticket() == next_numbering_ticket;
    });
    try {
      const canvas::datastore::Status status =
          turns->number_and_persist(uuid, context.submitted(), context.turn());
      if (!canvas::datastore::is_ok(status)) context.fail(status);
    } catch (...) {
      context.fail(canvas::datastore::Status::INVALID_STATE);
    }
    ++next_numbering_ticket;
    lock.unlock();
    numbering_changed.notify_all();
  }

  /** Update and durably checkpoint the materialized Turn Trie. */
  void update_trie(canvas::TurnContext& context) {
    canvas::proto::Turn committed;
    {
      const std::lock_guard lock(state_mutex);
      if (turn_tree.insert(context.turn()) != canvas::TurnTree::Status::OK) {
        std::vector<canvas::proto::Turn> stored;
        const canvas::datastore::Status loaded = turns->load_all(uuid, stored);
        if (!canvas::datastore::is_ok(loaded) ||
            turn_tree.rebuild(stored) != canvas::TurnTree::Status::OK) {
          context.fail(canvas::datastore::Status::CORRUPT_DATA);
          return;
        }
      }
      const canvas::datastore::Status updated = canvases->update_turn_trie_root(
          turn_trie_uuid, turn_tree.root_hash());
      if (!canvas::datastore::is_ok(updated)) {
        context.fail(updated);
        return;
      }
      canvas.set_turn_trie_root_hash(turn_tree.root_hash().bytes.data(),
                                     turn_tree.root_hash().bytes.size());
      *canvas.add_turns() = context.turn();
      if (context.turn().has_actor()) {
        const std::string actor = context.turn().actor().SerializeAsString();
        const bool known =
            std::any_of(canvas.actors().begin(), canvas.actors().end(),
                        [&actor](const canvas::proto::Actor& candidate) {
                          return candidate.SerializeAsString() == actor;
                        });
        if (!known) *canvas.add_actors() = context.turn().actor();
      }
      committed = context.turn();
    }

    // This announces a state transition that has already completed. Channel
    // delivery is deliberately not part of the authoritative Turn graph: a
    // missing observer must not prevent later durable graph nodes from
    // running.
    if (!publish_proto(committed_turn_channel, committed)) {
      Logger<WARN> << "Could not broadcast committed Turn '"
                   << committed.id().human_address() << "'";
    }
  }
};

CanvasSubsystem::CanvasSubsystem()
    : AppSubsystem("canvas",
                   subsystem_dependencies<DatastoreSubsystem, WorkerSubsystem,
                                          DirectorySubsystem>()),
      impl_(std::make_unique<Impl>()) {}

CanvasSubsystem::~CanvasSubsystem() = default;

Status CanvasSubsystem::initialize(AppState& app) {
  DatastoreSubsystem* datastore = app.get_subsystem<DatastoreSubsystem>();
  if (datastore == nullptr || datastore->database() == nullptr) {
    return Status::SUBSYSTEM_FAILURE;
  }
  impl_->database = datastore->database();
  impl_->canvases =
      std::make_unique<canvas::datastore::CanvasDatastore>(*impl_->database);
  impl_->turns =
      std::make_unique<canvas::datastore::TurnDatastore>(*impl_->database);

  canvas::datastore::Status stored = impl_->canvases->first(impl_->canvas);
  if (stored == canvas::datastore::Status::NOT_FOUND) {
    const canvas::CanvasUuid canvas_uuid       = canvas::random_canvas_uuid();
    const canvas::CanvasUuid turn_uuid         = canvas::random_canvas_uuid();
    const canvas::CanvasUuid presentation_uuid = canvas::random_canvas_uuid();
    impl_->canvas.set_canvas_uuid(canvas_uuid.data(), canvas_uuid.size());
    impl_->canvas.set_turn_trie_uuid(turn_uuid.data(), turn_uuid.size());
    impl_->canvas.set_presentation_uuid(presentation_uuid.data(),
                                        presentation_uuid.size());
    stored = impl_->canvases->create(impl_->canvas);
  }
  if (!canvas::datastore::is_ok(stored)) {
    static_cast<void>(terminate(app));
    return Status::SUBSYSTEM_FAILURE;
  }

  impl_->uuid.assign(impl_->canvas.canvas_uuid().begin(),
                     impl_->canvas.canvas_uuid().end());
  impl_->turn_trie_uuid.assign(impl_->canvas.turn_trie_uuid().begin(),
                               impl_->canvas.turn_trie_uuid().end());
  std::vector<canvas::proto::Turn> turns;
  stored = impl_->turns->load_all(impl_->uuid, turns);
  if (!canvas::datastore::is_ok(stored) ||
      impl_->turn_tree.rebuild(turns) != canvas::TurnTree::Status::OK) {
    static_cast<void>(terminate(app));
    return Status::SUBSYSTEM_FAILURE;
  }
  impl_->canvas.clear_turns();
  impl_->canvas.clear_actors();
  for (const canvas::proto::Turn& turn : turns) {
    *impl_->canvas.add_turns() = turn;
    if (!turn.has_actor()) continue;
    const std::string actor = turn.actor().SerializeAsString();
    const bool known        = std::any_of(
        impl_->canvas.actors().begin(), impl_->canvas.actors().end(),
        [&actor](const canvas::proto::Actor& candidate) {
          return candidate.SerializeAsString() == actor;
        });
    if (!known) *impl_->canvas.add_actors() = turn.actor();
  }
  if (!turns.empty()) {
    stored = impl_->canvases->update_turn_trie_root(
        impl_->turn_trie_uuid, impl_->turn_tree.root_hash());
    if (!canvas::datastore::is_ok(stored)) {
      static_cast<void>(terminate(app));
      return Status::SUBSYSTEM_FAILURE;
    }
  }

  impl_->pipeline = std::make_unique<canvas::TurnPipeline>();
  if (!execution_graph::is_ok(impl_->pipeline->register_node(
          std::string{kNumberAndPersistNode},
          [implementation = impl_.get()](canvas::TurnContext& context) {
            implementation->number_and_persist(context);
          })) ||
      !execution_graph::is_ok(impl_->pipeline->register_node(
          std::string{kUpdateTrieNode},
          [implementation = impl_.get()](canvas::TurnContext& context) {
            implementation->update_trie(context);
          },
          {std::string{kNumberAndPersistNode}}))) {
    static_cast<void>(terminate(app));
    return Status::SUBSYSTEM_FAILURE;
  }
  return Status::OK;
}

Status CanvasSubsystem::start(AppState& app) {
  WorkerSubsystem* workers      = app.get_subsystem<WorkerSubsystem>();
  DirectorySubsystem* directory = app.get_subsystem<DirectorySubsystem>();
  if (workers == nullptr || workers->workers() == nullptr ||
      directory == nullptr || directory->directory() == nullptr ||
      impl_->pipeline == nullptr ||
      !execution_graph::is_ok(impl_->pipeline->attach(*workers->workers()))) {
    return Status::SUBSYSTEM_FAILURE;
  }
  impl_->directory                        = directory->directory();
  const std::size_t maximum_message_bytes = directory->maximum_message_bytes();
  if (maximum_message_bytes == 0U) {
    static_cast<void>(stop(app));
    return Status::SUBSYSTEM_FAILURE;
  }
  canvas::CanvasUuid uuid{};
  std::copy(impl_->uuid.begin(), impl_->uuid.end(), uuid.begin());
  impl_->channel_root = "//canvas/" + canvas::canvas_uuid_hex(uuid);
  const auto turn_submission =
      resolve_route(impl_->channel_root, kTurnSubmissionPath);
  const auto committed_turn =
      resolve_route(impl_->channel_root, kCommittedTurnPath);
  const auto committed_presentation =
      resolve_route(impl_->channel_root, kCommittedPresentationPath);
  if (!turn_submission.has_value() || !committed_turn.has_value() ||
      !committed_presentation.has_value()) {
    static_cast<void>(stop(app));
    return Status::SUBSYSTEM_FAILURE;
  }
  impl_->turn_submission_name        = *turn_submission;
  impl_->committed_turn_name         = *committed_turn;
  impl_->committed_presentation_name = *committed_presentation;

  impl_->announcement_channel = open_shared_channel(
      *impl_->directory, kChannelsAnnounceChannel, maximum_message_bytes);
  impl_->query_channel = open_shared_channel(
      *impl_->directory, kChannelsQueryChannel, maximum_message_bytes);
  impl_->turn_submission_channel = std::make_shared<ipc::SmemChannel>(
      impl_->turn_submission_name, maximum_message_bytes);
  impl_->committed_turn_channel = std::make_shared<ipc::SmemChannel>(
      impl_->committed_turn_name, maximum_message_bytes);
  impl_->committed_presentation_channel = std::make_shared<ipc::SmemChannel>(
      impl_->committed_presentation_name, maximum_message_bytes);
  ipc::ChannelId ignored = 0U;
  if (impl_->announcement_channel == nullptr ||
      impl_->query_channel == nullptr ||
      !ipc::is_ok(impl_->directory->open_channel(impl_->turn_submission_channel,
                                                 ignored)) ||
      !ipc::is_ok(impl_->directory->open_channel(impl_->committed_turn_channel,
                                                 ignored)) ||
      !ipc::is_ok(impl_->directory->open_channel(
          impl_->committed_presentation_channel, ignored))) {
    static_cast<void>(stop(app));
    return Status::SUBSYSTEM_FAILURE;
  }
  {
    const std::lock_guard lock(impl_->mutex);
    impl_->accepting = true;
    impl_->closing   = false;
  }
  if (!ipc::is_ok(impl_->turn_submission_channel->subscribe(
          [implementation = impl_.get()](ipc::Channel::Bytes bytes) noexcept {
            implementation->enqueue(bytes);
          },
          impl_->turn_subscription))) {
    static_cast<void>(stop(app));
    return Status::SUBSYSTEM_FAILURE;
  }
  impl_->pump =
      std::thread{[implementation = impl_.get()] { implementation->run(); }};
  const std::shared_ptr<ipc::Channel> announcement =
      impl_->announcement_channel;
  const std::string announced_canvas_uuid = impl_->canvas.canvas_uuid();
  const std::string announced_root        = impl_->channel_root;
  if (!ipc::is_ok(impl_->query_channel->subscribe(
          [announcement, announced_canvas_uuid,
           announced_root](ipc::Channel::Bytes bytes) noexcept {
            canvas::proto::CanvasChannelsQuery query;
            if (!query.ParseFromArray(bytes.data(),
                                      static_cast<int>(bytes.size())) ||
                (query.has_canvas_uuid() &&
                 query.canvas_uuid() != announced_canvas_uuid)) {
              return;
            }
            static_cast<void>(publish_announcement(
                announcement, announced_canvas_uuid, announced_root,
                canvas::proto::CanvasChannelAnnouncement::OPENED));
          },
          impl_->query_subscription)) ||
      !impl_->announce(canvas::proto::CanvasChannelAnnouncement::OPENED)) {
    static_cast<void>(stop(app));
    return Status::SUBSYSTEM_FAILURE;
  }
  impl_->announced = true;
  return Status::OK;
}

Status CanvasSubsystem::stop(AppState& app) noexcept {
  static_cast<void>(app);
  impl_->query_subscription.reset();
  impl_->stop_pump();
  Status result = Status::OK;
  if (impl_->announced &&
      !impl_->announce(canvas::proto::CanvasChannelAnnouncement::CLOSING)) {
    result = Status::SUBSYSTEM_FAILURE;
  }
  impl_->announced = false;
  if (impl_->directory != nullptr) {
    if (!impl_->committed_presentation_name.empty() &&
        !ipc::is_ok(impl_->directory->close_channel(
            impl_->committed_presentation_name))) {
      result = Status::SUBSYSTEM_FAILURE;
    }
    if (!impl_->committed_turn_name.empty() &&
        !ipc::is_ok(
            impl_->directory->close_channel(impl_->committed_turn_name))) {
      result = Status::SUBSYSTEM_FAILURE;
    }
    if (!impl_->turn_submission_name.empty() &&
        !ipc::is_ok(
            impl_->directory->close_channel(impl_->turn_submission_name))) {
      result = Status::SUBSYSTEM_FAILURE;
    }
  }
  impl_->turn_submission_channel.reset();
  impl_->committed_turn_channel.reset();
  impl_->committed_presentation_channel.reset();
  impl_->announcement_channel.reset();
  impl_->query_channel.reset();
  impl_->directory = nullptr;
  return result;
}

Status CanvasSubsystem::terminate(AppState& app) noexcept {
  static_cast<void>(stop(app));
  impl_->pipeline.reset();
  impl_->turns.reset();
  impl_->canvases.reset();
  impl_->database = nullptr;
  impl_->canvas.Clear();
  impl_->uuid.clear();
  impl_->turn_trie_uuid.clear();
  impl_->turn_tree             = {};
  impl_->next_numbering_ticket = 0U;
  impl_->channel_root.clear();
  impl_->turn_submission_name.clear();
  impl_->committed_turn_name.clear();
  impl_->committed_presentation_name.clear();
  return Status::OK;
}

canvas::proto::Canvas CanvasSubsystem::canvas() const {
  const std::lock_guard lock(impl_->state_mutex);
  return impl_->canvas;
}

const std::vector<std::uint8_t>& CanvasSubsystem::canvas_uuid() const noexcept {
  return impl_->uuid;
}

std::string CanvasSubsystem::channel_root_name() const {
  return impl_->channel_root;
}

std::string CanvasSubsystem::turn_submission_channel_name() const {
  return impl_->turn_submission_name;
}

std::string CanvasSubsystem::committed_turn_channel_name() const {
  return impl_->committed_turn_name;
}

std::string CanvasSubsystem::committed_presentation_channel_name() const {
  return impl_->committed_presentation_name;
}

canvas::TurnPipeline* CanvasSubsystem::pipeline() noexcept {
  return impl_->pipeline.get();
}

void CanvasSubsystem::materialize_presentation(
    const canvas::proto::Presentation& presentation) noexcept {
  try {
    const std::lock_guard lock(impl_->state_mutex);
    *impl_->canvas.mutable_presentation() = presentation;
    if (presentation.has_root_hash()) {
      impl_->canvas.set_presentation_root_hash(presentation.root_hash());
    }
  } catch (...) {
  }
}

bool CanvasSubsystem::publish_committed_presentation(
    const canvas::proto::Presentation& presentation) noexcept {
  return publish_proto(impl_->committed_presentation_channel, presentation);
}

}  // namespace puc::app
