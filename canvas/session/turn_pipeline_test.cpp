#include "canvas/session/turn_pipeline.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "canvas/presentation/presentation_tree.hpp"
#include "canvas/protos/canvas.pb.h"
#include "canvas/protos/datastore/canvas_datastore.hpp"
#include "canvas/protos/datastore/database.hpp"
#include "canvas/protos/datastore/presentation_datastore.hpp"
#include "canvas/protos/datastore/turn_datastore.hpp"
#include "canvas/turn/turn_tree.hpp"
#include "utils/multithreading/job_queue.hpp"

namespace puc::canvas {
namespace {

using namespace std::chrono_literals;

/** Reusable condition-variable gate for deterministic overlapping runs. */
class Gate final {
 public:
  /** Block until open() releases all current and future waiters. */
  void wait() {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] { return open_; });
  }

  /** Release every waiting pipeline node. */
  void open() {
    {
      const std::lock_guard lock(mutex_);
      open_ = true;
    }
    changed_.notify_all();
  }

 private:
  std::mutex mutex_;                /**< Protects open_. */
  std::condition_variable changed_; /**< Signals gate opening. */
  bool open_ = false;               /**< Whether waiters may continue. */
};

/** Synchronized counter supporting bounded asynchronous observations. */
class Counter final {
 public:
  /** Increment and wake all observers. */
  void increment() {
    {
      const std::lock_guard lock(mutex_);
      ++value_;
    }
    changed_.notify_all();
  }

  /** Wait until the counter reaches at least one expected value. */
  bool wait_for(std::size_t expected) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, 2s,
                             [this, expected] { return value_ >= expected; });
  }

 private:
  std::mutex mutex_;                /**< Protects value_. */
  std::condition_variable changed_; /**< Signals counter changes. */
  std::size_t value_ = 0U;          /**< Current observation count. */
};

std::filesystem::path database_path() {
  return std::filesystem::temp_directory_path() /
         ("puc-pipeline-test-" + std::to_string(::getpid()) + ".sqlite");
}

std::vector<std::uint8_t> uuid(std::uint8_t first) {
  std::vector<std::uint8_t> result(16U);
  for (std::size_t index = 0U; index < result.size(); ++index) {
    result[index] = static_cast<std::uint8_t>(first + index);
  }
  return result;
}

proto::Turn submitted(const std::vector<std::uint8_t>& canvas_uuid,
                      std::string text, const proto::TurnId* parent = nullptr) {
  proto::Turn result;
  result.mutable_id()->set_canvas_uuid(canvas_uuid.data(), canvas_uuid.size());
  if (parent != nullptr) *result.mutable_parent() = *parent;
  result.mutable_actor()->set_id("human");
  result.mutable_actor()->set_kind(proto::Actor::HUMAN);
  result.mutable_payload()->set_kind(proto::Payload::TEXT);
  result.mutable_payload()->set_content(std::move(text));
  result.set_display_level(proto::Turn::ALWAYS_SHOW);
  return result;
}

}  // namespace

TEST(TurnPipelineTest, OverlapsRunsAndSnapshotsTopologyPerSubmission) {
  multithreading::JobQueue workers{4U};
  TurnPipeline pipeline;
  Gate gate;
  Counter started;
  Counter completed;
  std::atomic<std::size_t> observer_runs{0U};
  std::atomic<std::size_t> failed_runs{0U};

  ASSERT_EQ(
      pipeline.register_node("blocked_resource",
                             [&](TurnContext& context) {
                               started.increment();
                               gate.wait();
                               context.turn() = context.submitted();
                               context.turn().mutable_id()->set_human_address(
                                   context.turn().payload().content());
                             }),
      execution_graph::Status::OK);
  ASSERT_EQ(pipeline.attach(workers), execution_graph::Status::OK);

  const auto completion = [&](datastore::Status status, proto::Turn) {
    if (!datastore::is_ok(status)) failed_runs.fetch_add(1U);
    completed.increment();
  };
  const std::vector<std::uint8_t> canvas_uuid = uuid(1U);
  ASSERT_EQ(pipeline.submit(submitted(canvas_uuid, "1"), completion),
            execution_graph::Status::OK);
  ASSERT_EQ(pipeline.submit(submitted(canvas_uuid, "2"), completion),
            execution_graph::Status::OK);
  ASSERT_TRUE(started.wait_for(2U));

  ASSERT_EQ(
      pipeline.register_node("late_observer",
                             [&](TurnContext&) { observer_runs.fetch_add(1U); },
                             {"blocked_resource"}),
      execution_graph::Status::OK);
  ASSERT_EQ(pipeline.submit(submitted(canvas_uuid, "3"), completion),
            execution_graph::Status::OK);
  ASSERT_TRUE(started.wait_for(3U));

  gate.open();
  ASSERT_TRUE(completed.wait_for(3U));
  EXPECT_EQ(failed_runs.load(), 0U);
  EXPECT_EQ(observer_runs.load(), 1U);

  pipeline.detach();
  workers.wait();
}

TEST(TurnPipelineTest, RegisteredNodesPersistAndOrderLateReplies) {
  const std::filesystem::path path = database_path();
  std::filesystem::remove(path);
  const std::vector<std::uint8_t> canvas_uuid       = uuid(1U);
  const std::vector<std::uint8_t> turn_trie_uuid    = uuid(21U);
  const std::vector<std::uint8_t> presentation_uuid = uuid(41U);
  datastore::Database database;
  const std::array migration_sets{
      datastore::CanvasDatastore::migrations(),
      datastore::TurnDatastore::migrations(),
      datastore::PresentationDatastore::migrations(),
  };
  ASSERT_EQ(database.initialize(path, migration_sets), datastore::Status::OK);
  datastore::CanvasDatastore canvases{database};
  datastore::TurnDatastore turns{database};
  datastore::PresentationDatastore presentations{database};
  proto::Canvas canvas;
  canvas.set_canvas_uuid(canvas_uuid.data(), canvas_uuid.size());
  canvas.set_turn_trie_uuid(turn_trie_uuid.data(), turn_trie_uuid.size());
  canvas.set_presentation_uuid(presentation_uuid.data(),
                               presentation_uuid.size());
  ASSERT_EQ(canvases.create(canvas), datastore::Status::OK);

  multithreading::JobQueue workers{2U};
  TurnTree turn_tree;
  PresentationTree presentation_tree{presentations, presentation_uuid, {}};
  PendingPresentation pending;
  TurnPipeline pipeline;
  ASSERT_EQ(pipeline.attach(workers), execution_graph::Status::OK);
  ASSERT_EQ(pipeline.register_node("number_and_persist",
                                   [&](TurnContext& context) {
                                     context.fail(turns.number_and_persist(
                                         canvas_uuid, context.submitted(),
                                         context.turn()));
                                   }),
            execution_graph::Status::OK);
  ASSERT_EQ(
      pipeline.register_node("update_trie",
                             [&](TurnContext& context) {
                               if (turn_tree.insert(context.turn()) !=
                                   TurnTree::Status::OK) {
                                 context.fail(datastore::Status::CORRUPT_DATA);
                                 return;
                               }
                               context.fail(canvases.update_turn_trie_root(
                                   turn_trie_uuid, turn_tree.root_hash()));
                             },
                             {"number_and_persist"}),
      execution_graph::Status::OK);
  ASSERT_EQ(
      pipeline.register_node("linearize",
                             [&](TurnContext& context) {
                               context.fail(presentation_tree.prepare_insert(
                                   context.turn().id(), pending));
                             },
                             {"update_trie"}),
      execution_graph::Status::OK);
  ASSERT_EQ(pipeline.register_node(
                "commit_presentation",
                [&](TurnContext& context) {
                  const datastore::Status status = presentations.commit(
                      presentation_uuid, pending.previous_root,
                      pending.new_root, context.turn().id(), pending.nodes);
                  context.fail(status);
                  if (datastore::is_ok(status))
                    presentation_tree.commit(pending);
                },
                {"linearize"}),
            execution_graph::Status::OK);

  proto::Turn one;
  proto::Turn two;
  proto::Turn late_reply;
  ASSERT_EQ(pipeline.process(submitted(canvas_uuid, "one"), one),
            datastore::Status::OK);
  ASSERT_EQ(one.id().human_address(), "1");
  std::size_t runtime_observations = 0U;
  ASSERT_EQ(pipeline.register_node("runtime_observer",
                                   [&runtime_observations](TurnContext&) {
                                     ++runtime_observations;
                                   },
                                   {"commit_presentation"}),
            execution_graph::Status::OK);
  ASSERT_EQ(pipeline.process(submitted(canvas_uuid, "two"), two),
            datastore::Status::OK);
  ASSERT_EQ(two.id().human_address(), "2");
  ASSERT_EQ(
      pipeline.process(submitted(canvas_uuid, "late", &one.id()), late_reply),
      datastore::Status::OK);
  ASSERT_EQ(late_reply.id().human_address(), "1.1");
  EXPECT_EQ(runtime_observations, 2U);

  std::vector<proto::TurnId> ordered;
  ASSERT_EQ(presentation_tree.ordered_turns(ordered), datastore::Status::OK);
  ASSERT_EQ(ordered.size(), 3U);
  EXPECT_EQ(ordered[0].human_address(), "1");
  EXPECT_EQ(ordered[1].human_address(), "1.1");
  EXPECT_EQ(ordered[2].human_address(), "2");

  datastore::Statement root;
  ASSERT_EQ(database.prepare("SELECT length(root_hash) FROM turn_tries;", root),
            datastore::Status::OK);
  ASSERT_EQ(root.step(), datastore::Status::OK);
  EXPECT_EQ(root.integer(0), 32);

  hashing::Hash256 persisted_root;
  ASSERT_EQ(presentations.load_root(presentation_uuid, persisted_root),
            datastore::Status::OK);
  PresentationTree restored{presentations, presentation_uuid, persisted_root};
  std::vector<proto::TurnId> restored_order;
  ASSERT_EQ(restored.ordered_turns(restored_order), datastore::Status::OK);
  ASSERT_EQ(restored_order.size(), 3U);
  EXPECT_EQ(restored_order[1].human_address(), "1.1");

  EXPECT_EQ(pipeline.unregister_node("linearize"),
            execution_graph::Status::INVALID_ARGUMENT);
  EXPECT_EQ(pipeline.unregister_node("runtime_observer"),
            execution_graph::Status::OK);
  EXPECT_EQ(pipeline.unregister_node("commit_presentation"),
            execution_graph::Status::OK);
  EXPECT_EQ(pipeline.unregister_node("linearize"), execution_graph::Status::OK);
  pipeline.detach();
  workers.wait();
  database.close();
  std::filesystem::remove(path);
}

}  // namespace puc::canvas
