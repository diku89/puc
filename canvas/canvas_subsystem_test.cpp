#include "canvas/canvas_subsystem.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "canvas/datastore_subsystem.hpp"
#include "canvas/presentation/presentation_tree.hpp"
#include "canvas/protos/canvas.pb.h"
#include "canvas/protos/channels.pb.h"
#include "canvas/protos/datastore/canvas_datastore.hpp"
#include "canvas/protos/datastore/database.hpp"
#include "canvas/protos/datastore/presentation_datastore.hpp"
#include "canvas/protos/datastore/turn_datastore.hpp"
#include "canvas/protos/presentation.pb.h"
#include "canvas/protos/turn.pb.h"
#include "canvas/session/orchestration_subsystem.hpp"
#include "canvas/session/turn_pipeline.hpp"
#include "properties/properties_subsystem.hpp"
#include "state/state.hpp"
#include "utils/ipc/directory.hpp"
#include "utils/ipc/directory_subsystem.hpp"
#include "utils/logger/logger_subsystem.hpp"
#include "utils/multithreading/worker_subsystem.hpp"

namespace puc::app {
namespace {

constexpr std::size_t kTestMaximumMessageBytes = 1024U;

class TemporaryConfiguration final {
 public:
  TemporaryConfiguration() {
    root = std::filesystem::temp_directory_path() /
           ("puc-canvas-subsystem-test-" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    database = root / "sessions.db";
    write_database_path(database);
    std::ofstream ipc_config{root / "ipc.toml"};
    ipc_config << "[channel]\nmaximum_message_bytes = "
               << kTestMaximumMessageBytes << "\n";
  }

  void write_database_path(const std::filesystem::path& path) const {
    std::ofstream config{root / "canvas.toml"};
    config << "[database]\npath = \"" << path.string() << "\"\n";
  }
  ~TemporaryConfiguration() { std::filesystem::remove_all(root); }

  std::filesystem::path root;
  std::filesystem::path database;
};

canvas::proto::Turn human_turn(const CanvasSubsystem& subsystem,
                               std::string text) {
  canvas::proto::Turn turn;
  turn.mutable_id()->set_canvas_uuid(subsystem.canvas_uuid().data(),
                                     subsystem.canvas_uuid().size());
  turn.mutable_actor()->set_id("human");
  turn.mutable_actor()->set_kind(canvas::proto::Actor::HUMAN);
  turn.mutable_actor()->set_display_name("Human");
  const std::array<std::uint8_t, 16U> session_uuid{};
  turn.mutable_actor()->set_session_uuid(session_uuid.data(),
                                         session_uuid.size());
  turn.set_observed_unix_time_ns(1U);
  turn.mutable_payload()->set_kind(canvas::proto::Payload::TEXT);
  turn.mutable_payload()->set_content(std::move(text));
  turn.mutable_payload()->set_lifespan(canvas::proto::Payload::PERSISTENT);
  turn.mutable_payload()->set_ownership(canvas::proto::Payload::ACTOR);
  turn.set_display_level(canvas::proto::Turn::ALWAYS_SHOW);
  return turn;
}

std::vector<std::uint8_t> test_uuid(std::uint8_t first) {
  std::vector<std::uint8_t> result(16U);
  for (std::size_t index = 0U; index < result.size(); ++index) {
    result[index] = static_cast<std::uint8_t>(first + index);
  }
  return result;
}

}  // namespace

TEST(CanvasSubsystemTest, ReceivesTurnAndPublishesPresentationCommit) {
  TemporaryConfiguration configuration;
  AppState app;
  ASSERT_EQ(app.register_subsystem(std::make_unique<LoggerSubsystem>()),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<PropertiesSubsystem>(
                PropertiesSubsystemOptions{
                    .primary_root        = configuration.root,
                    .user_overrides_root = configuration.root / "overrides"})),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<DatastoreSubsystem>()),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<WorkerSubsystem>(2U)),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<DirectorySubsystem>()),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<CanvasSubsystem>()),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<OrchestrationSubsystem>()),
            Status::OK);
  ASSERT_EQ(app.initialize(OperatingMode::TEST), Status::OK);
  EXPECT_TRUE(std::filesystem::is_regular_file(configuration.database));
  ASSERT_EQ(app.start(), Status::OK);

  CanvasSubsystem* canvas_subsystem = app.get_subsystem<CanvasSubsystem>();
  DirectorySubsystem* directory     = app.get_subsystem<DirectorySubsystem>();
  ASSERT_NE(canvas_subsystem, nullptr);
  ASSERT_NE(directory, nullptr);
  ASSERT_NE(directory->directory(), nullptr);
  EXPECT_EQ(directory->maximum_message_bytes(), kTestMaximumMessageBytes);
  ASSERT_NE(canvas_subsystem->pipeline(), nullptr);
  EXPECT_EQ(canvas_subsystem->pipeline()->size(), 4U);

  std::mutex mutex;
  std::condition_variable changed;
  canvas::proto::Turn observed_turn;
  canvas::proto::Presentation observed;
  std::vector<std::string> commit_order;
  std::vector<std::string> presented_addresses;
  ipc::Subscription turn_subscription;
  ipc::Subscription presentation_subscription;
  ASSERT_EQ(directory->directory()->subscribe(
                canvas_subsystem->committed_turn_channel_name(),
                [&mutex, &changed, &observed_turn,
                 &commit_order](ipc::Channel::Bytes bytes) noexcept {
                  canvas::proto::Turn turn;
                  if (!turn.ParseFromArray(bytes.data(),
                                           static_cast<int>(bytes.size()))) {
                    return;
                  }
                  const std::lock_guard lock(mutex);
                  observed_turn = std::move(turn);
                  commit_order.emplace_back("turn");
                  changed.notify_all();
                },
                turn_subscription),
            ipc::Status::OK);
  ASSERT_EQ(directory->directory()->subscribe(
                canvas_subsystem->committed_presentation_channel_name(),
                [&mutex, &changed, &observed, &commit_order,
                 &presented_addresses](ipc::Channel::Bytes bytes) noexcept {
                  canvas::proto::Presentation update;
                  if (!update.ParseFromArray(bytes.data(),
                                             static_cast<int>(bytes.size()))) {
                    return;
                  }
                  const std::lock_guard lock(mutex);
                  observed = std::move(update);
                  commit_order.emplace_back("presentation");
                  presented_addresses.push_back(
                      observed.committed_turn_id().human_address());
                  changed.notify_all();
                },
                presentation_subscription),
            ipc::Status::OK);

  EXPECT_EQ(canvas_subsystem->turn_submission_channel_name(),
            canvas_subsystem->channel_root_name() + "/turns/submit");
  EXPECT_EQ(canvas_subsystem->committed_turn_channel_name(),
            canvas_subsystem->channel_root_name() + "/turns/committed");
  EXPECT_EQ(canvas_subsystem->committed_presentation_channel_name(),
            canvas_subsystem->channel_root_name() + "/presentation/committed");

  const canvas::proto::Turn submitted =
      human_turn(*canvas_subsystem, "hello canvas");
  std::string payload;
  ASSERT_TRUE(submitted.SerializeToString(&payload));
  const auto bytes = std::span<const std::uint8_t>{
      reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()};
  ASSERT_EQ(
      directory->directory()
          ->transmit(canvas_subsystem->turn_submission_channel_name(), bytes)
          .status,
      ipc::Status::OK);
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds{5}, [&observed] {
      return observed.has_committed_turn_id();
    }));
  }
  EXPECT_EQ(observed_turn.id().human_address(), "1");
  EXPECT_EQ(observed.committed_turn_id().human_address(), "1");
  EXPECT_EQ(commit_order, (std::vector<std::string>{"turn", "presentation"}));
  EXPECT_EQ(observed.canvas_uuid().size(), 16U);
  EXPECT_EQ(observed.presentation_uuid().size(), 16U);
  EXPECT_FALSE(observed.root_hash().empty());
  EXPECT_EQ(canvas_subsystem->canvas().turn_trie_uuid().size(), 16U);
  EXPECT_EQ(canvas_subsystem->canvas().presentation_uuid().size(), 16U);
  EXPECT_NE(canvas_subsystem->canvas().turn_trie_uuid(),
            canvas_subsystem->canvas().presentation_uuid());
  ASSERT_EQ(canvas_subsystem->canvas().turns_size(), 1);
  ASSERT_EQ(canvas_subsystem->canvas().actors_size(), 1);
  EXPECT_EQ(canvas_subsystem->canvas().turns(0).payload().content(),
            "hello canvas");
  EXPECT_EQ(canvas_subsystem->canvas().actors(0).session_uuid().size(), 16U);
  EXPECT_TRUE(canvas_subsystem->canvas().has_presentation());

  // A committed Turn notification is an observation, not a prerequisite for
  // the authoritative graph. Force that notification over its transport limit
  // and prove the durable Presentation commit still completes.
  {
    const std::lock_guard lock(mutex);
    commit_order.clear();
  }
  canvas::proto::Turn oversized =
      human_turn(*canvas_subsystem, std::string(kTestMaximumMessageBytes, 'x'));
  canvas::proto::Turn oversized_committed;
  EXPECT_EQ(
      canvas_subsystem->pipeline()->process(oversized, oversized_committed),
      canvas::datastore::Status::OK);
  EXPECT_EQ(oversized_committed.id().human_address(), "2");
  EXPECT_EQ(canvas_subsystem->canvas()
                .presentation()
                .committed_turn_id()
                .human_address(),
            "2");
  {
    const std::lock_guard lock(mutex);
    EXPECT_EQ(commit_order, (std::vector<std::string>{"presentation"}));
  }

  // Burst several independent runs through the real IPC ingress. Numbering,
  // Trie mutation, and Presentation advancement serialize only their owned
  // resources while the Turns overlap between those boundaries.
  for (std::size_t index = 3U; index <= 10U; ++index) {
    const canvas::proto::Turn burst =
        human_turn(*canvas_subsystem, "burst " + std::to_string(index));
    std::string burst_payload;
    ASSERT_TRUE(burst.SerializeToString(&burst_payload));
    const auto burst_bytes = std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(burst_payload.data()),
        burst_payload.size()};
    ASSERT_EQ(directory->directory()
                  ->transmit(canvas_subsystem->turn_submission_channel_name(),
                             burst_bytes)
                  .status,
              ipc::Status::OK);
  }
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(changed.wait_for(
        lock, std::chrono::seconds{5},
        [&presented_addresses] { return presented_addresses.size() == 10U; }));
  }
  const canvas::proto::Canvas burst_snapshot = canvas_subsystem->canvas();
  EXPECT_EQ(burst_snapshot.turns_size(), 10);
  for (const canvas::proto::Turn& turn : burst_snapshot.turns()) {
    if (turn.payload().content().starts_with("burst ")) {
      EXPECT_EQ(turn.id().human_address(), turn.payload().content().substr(6U));
    }
  }
  OrchestrationSubsystem* orchestration =
      app.get_subsystem<OrchestrationSubsystem>();
  ASSERT_NE(orchestration, nullptr);
  ASSERT_NE(orchestration->presentation_tree(), nullptr);
  std::vector<canvas::proto::TurnId> presented;
  ASSERT_EQ(orchestration->presentation_tree()->ordered_turns(presented),
            canvas::datastore::Status::OK);
  ASSERT_EQ(presented.size(), 10U);
  for (std::size_t index = 0U; index < presented.size(); ++index) {
    EXPECT_EQ(presented[index].human_address(), std::to_string(index + 1U));
  }

  std::vector<canvas::proto::CanvasChannelAnnouncement::State>
      announcement_states;
  canvas::proto::CanvasChannelAnnouncement current_announcement;
  ipc::Subscription announcement_subscription;
  ASSERT_EQ(directory->directory()->subscribe(
                CanvasSubsystem::kChannelsAnnounceChannel,
                [&mutex, &changed, &announcement_states,
                 &current_announcement](ipc::Channel::Bytes bytes) noexcept {
                  canvas::proto::CanvasChannelAnnouncement announcement;
                  if (!announcement.ParseFromArray(
                          bytes.data(), static_cast<int>(bytes.size()))) {
                    return;
                  }
                  const std::lock_guard lock(mutex);
                  current_announcement = std::move(announcement);
                  announcement_states.push_back(current_announcement.state());
                  changed.notify_all();
                },
                announcement_subscription),
            ipc::Status::OK);
  canvas::proto::CanvasChannelsQuery query;
  std::string query_payload;
  ASSERT_TRUE(query.SerializeToString(&query_payload));
  const auto query_bytes = std::span<const std::uint8_t>{
      reinterpret_cast<const std::uint8_t*>(query_payload.data()),
      query_payload.size()};
  ASSERT_EQ(directory->directory()
                ->transmit(CanvasSubsystem::kChannelsQueryChannel, query_bytes)
                .status,
            ipc::Status::OK);
  ASSERT_FALSE(announcement_states.empty());
  EXPECT_EQ(announcement_states.back(),
            canvas::proto::CanvasChannelAnnouncement::OPENED);
  EXPECT_EQ(current_announcement.canvas_uuid(),
            canvas_subsystem->canvas().canvas_uuid());
  EXPECT_EQ(current_announcement.channel_root(),
            canvas_subsystem->channel_root_name());
  EXPECT_EQ(current_announcement.protocol_version(),
            CanvasSubsystem::kChannelProtocolVersion);

  turn_subscription.reset();
  presentation_subscription.reset();
  EXPECT_EQ(app.stop(), Status::OK);
  ASSERT_FALSE(announcement_states.empty());
  EXPECT_EQ(announcement_states.back(),
            canvas::proto::CanvasChannelAnnouncement::CLOSING);
  announcement_subscription.reset();
  EXPECT_EQ(app.start(), Status::OK);
  ASSERT_EQ(directory->directory()->subscribe(
                CanvasSubsystem::kChannelsAnnounceChannel,
                [&mutex, &changed, &announcement_states,
                 &current_announcement](ipc::Channel::Bytes bytes) noexcept {
                  canvas::proto::CanvasChannelAnnouncement announcement;
                  if (!announcement.ParseFromArray(
                          bytes.data(), static_cast<int>(bytes.size()))) {
                    return;
                  }
                  const std::lock_guard lock(mutex);
                  current_announcement = std::move(announcement);
                  announcement_states.push_back(current_announcement.state());
                  changed.notify_all();
                },
                announcement_subscription),
            ipc::Status::OK);
  ASSERT_EQ(directory->directory()
                ->transmit(CanvasSubsystem::kChannelsQueryChannel, query_bytes)
                .status,
            ipc::Status::OK);
  ASSERT_FALSE(announcement_states.empty());
  EXPECT_EQ(announcement_states.back(),
            canvas::proto::CanvasChannelAnnouncement::OPENED);
  EXPECT_EQ(app.stop(), Status::OK);
  EXPECT_EQ(announcement_states.back(),
            canvas::proto::CanvasChannelAnnouncement::CLOSING);
  announcement_subscription.reset();
  EXPECT_EQ(app.terminate(), Status::OK);
}

TEST(CanvasSubsystemTest,
     InitializationCommitsPersistedTurnsMissingFromPresentationLedger) {
  TemporaryConfiguration configuration;
  const std::vector<std::uint8_t> canvas_uuid       = test_uuid(1U);
  const std::vector<std::uint8_t> turn_trie_uuid    = test_uuid(21U);
  const std::vector<std::uint8_t> presentation_uuid = test_uuid(41U);
  {
    canvas::datastore::Database database;
    const std::array migration_sets{
        canvas::datastore::CanvasDatastore::migrations(),
        canvas::datastore::TurnDatastore::migrations(),
        canvas::datastore::PresentationDatastore::migrations(),
    };
    ASSERT_EQ(database.initialize(configuration.database, migration_sets),
              canvas::datastore::Status::OK);
    canvas::datastore::CanvasDatastore canvases{database};
    canvas::datastore::TurnDatastore turns{database};
    canvas::proto::Canvas stored_canvas;
    stored_canvas.set_canvas_uuid(canvas_uuid.data(), canvas_uuid.size());
    stored_canvas.set_turn_trie_uuid(turn_trie_uuid.data(),
                                     turn_trie_uuid.size());
    stored_canvas.set_presentation_uuid(presentation_uuid.data(),
                                        presentation_uuid.size());
    ASSERT_EQ(canvases.create(stored_canvas), canvas::datastore::Status::OK);

    canvas::proto::Turn submitted;
    submitted.mutable_id()->set_canvas_uuid(canvas_uuid.data(),
                                            canvas_uuid.size());
    submitted.mutable_actor()->set_id("human");
    submitted.mutable_actor()->set_kind(canvas::proto::Actor::HUMAN);
    submitted.mutable_payload()->set_kind(canvas::proto::Payload::TEXT);
    submitted.mutable_payload()->set_content("persisted before crash");
    submitted.set_display_level(canvas::proto::Turn::ALWAYS_SHOW);
    canvas::proto::Turn first;
    ASSERT_EQ(turns.number_and_persist(canvas_uuid, submitted, first),
              canvas::datastore::Status::OK);
    ASSERT_EQ(first.id().human_address(), "1");
    canvas::proto::Turn second;
    ASSERT_EQ(turns.number_and_persist(canvas_uuid, submitted, second),
              canvas::datastore::Status::OK);
    ASSERT_EQ(second.id().human_address(), "2");
    *submitted.mutable_parent() = first.id();
    canvas::proto::Turn late_reply;
    ASSERT_EQ(turns.number_and_persist(canvas_uuid, submitted, late_reply),
              canvas::datastore::Status::OK);
    ASSERT_EQ(late_reply.id().human_address(), "1.1");
  }

  AppState app;
  ASSERT_EQ(app.register_subsystem(std::make_unique<LoggerSubsystem>()),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<PropertiesSubsystem>(
                PropertiesSubsystemOptions{
                    .primary_root        = configuration.root,
                    .user_overrides_root = configuration.root / "overrides"})),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<DatastoreSubsystem>()),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<WorkerSubsystem>(2U)),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<DirectorySubsystem>()),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<CanvasSubsystem>()),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<OrchestrationSubsystem>()),
            Status::OK);
  ASSERT_EQ(app.initialize(OperatingMode::TEST), Status::OK);

  CanvasSubsystem* canvas_subsystem = app.get_subsystem<CanvasSubsystem>();
  DatastoreSubsystem* datastore     = app.get_subsystem<DatastoreSubsystem>();
  OrchestrationSubsystem* orchestration =
      app.get_subsystem<OrchestrationSubsystem>();
  ASSERT_NE(canvas_subsystem, nullptr);
  ASSERT_NE(datastore, nullptr);
  ASSERT_NE(datastore->database(), nullptr);
  ASSERT_NE(orchestration, nullptr);
  ASSERT_NE(orchestration->presentation_tree(), nullptr);
  ASSERT_TRUE(canvas_subsystem->canvas().has_presentation());
  EXPECT_EQ(canvas_subsystem->canvas()
                .presentation()
                .committed_turn_id()
                .human_address(),
            "2");

  canvas::datastore::PresentationDatastore presentations{
      *datastore->database()};
  std::vector<std::string> committed_addresses;
  ASSERT_EQ(presentations.load_committed_turn_addresses(presentation_uuid,
                                                        committed_addresses),
            canvas::datastore::Status::OK);
  EXPECT_EQ(committed_addresses.size(), 3U);
  std::vector<canvas::proto::TurnId> ordered;
  ASSERT_EQ(orchestration->presentation_tree()->ordered_turns(ordered),
            canvas::datastore::Status::OK);
  ASSERT_EQ(ordered.size(), 3U);
  EXPECT_EQ(ordered.front().human_address(), "1");
  EXPECT_EQ(ordered[1].human_address(), "1.1");
  EXPECT_EQ(ordered[2].human_address(), "2");

  EXPECT_EQ(app.terminate(), Status::OK);
}

TEST(DatastoreSubsystemTest,
     UnopenableDatabaseAbortsApplicationInitialization) {
  TemporaryConfiguration configuration;
  configuration.write_database_path(configuration.root);

  AppState app;
  ASSERT_EQ(app.register_subsystem(std::make_unique<LoggerSubsystem>()),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<PropertiesSubsystem>(
                PropertiesSubsystemOptions{
                    .primary_root        = configuration.root,
                    .user_overrides_root = configuration.root / "overrides"})),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<DatastoreSubsystem>()),
            Status::OK);

  EXPECT_EQ(app.initialize(OperatingMode::TEST), Status::SUBSYSTEM_FAILURE);
  EXPECT_EQ(app.lifecycle_state(), LifecycleState::CRASHED);
  EXPECT_EQ(app.get_subsystem<DatastoreSubsystem>()->database(), nullptr);
}

}  // namespace puc::app
