/**
 * @file state_test.cpp
 * @brief Tests for typed subsystem registration and lifecycle ordering.
 */

#include "puc-cli/state/state.hpp"

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

namespace puc::app {
namespace {

/** Shared ordered hook observations across concrete subsystem test doubles. */
struct Trace {
  std::vector<std::string> calls;
};

/** Configurable lifecycle implementation inherited by distinct adapter types.
 */
class RecordingSubsystem : public AppSubsystem {
 public:
  RecordingSubsystem(std::string name, std::vector<SubsystemId> dependencies,
                     Trace& trace)
      : AppSubsystem(std::move(name), std::move(dependencies)),
        trace_(&trace) {}

  Status initialize(AppState& app) override {
    record("initialize", app.operating_mode());
    if (throw_during_initialize) {
      throw std::runtime_error("initialize failed");
    }
    return initialize_status;
  }

  Status start(AppState& app) override {
    record("start", app.operating_mode());
    if (throw_during_start) {
      throw std::runtime_error("start failed");
    }
    return start_status;
  }

  Status stop(AppState& app) noexcept override {
    record("stop", app.operating_mode());
    return stop_status;
  }

  Status terminate(AppState& app) noexcept override {
    record("terminate", app.operating_mode());
    return terminate_status;
  }

  Status initialize_status     = Status::OK;
  Status start_status          = Status::OK;
  Status stop_status           = Status::OK;
  Status terminate_status      = Status::OK;
  bool throw_during_initialize = false;
  bool throw_during_start      = false;

 private:
  void record(std::string_view hook, OperatingMode mode) noexcept {
    trace_->calls.push_back(std::string{name()} + "." + std::string{hook});
    observed_modes.push_back(mode);
  }

 public:
  std::vector<OperatingMode> observed_modes;

 private:
  Trace* trace_; /**< Caller-owned trace that outlives AppState in each test. */
};

class RootSubsystem final : public RecordingSubsystem {
 public:
  explicit RootSubsystem(Trace& trace)
      : RecordingSubsystem("root", {}, trace) {}
};

class LeftSubsystem final : public RecordingSubsystem {
 public:
  explicit LeftSubsystem(Trace& trace)
      : RecordingSubsystem("left", subsystem_dependencies<RootSubsystem>(),
                           trace) {}
};

class RightSubsystem final : public RecordingSubsystem {
 public:
  explicit RightSubsystem(Trace& trace)
      : RecordingSubsystem("right", subsystem_dependencies<RootSubsystem>(),
                           trace) {}
};

class LeafSubsystem final : public RecordingSubsystem {
 public:
  explicit LeafSubsystem(Trace& trace)
      : RecordingSubsystem(
            "leaf", subsystem_dependencies<LeftSubsystem, RightSubsystem>(),
            trace) {}
};

class SameNameSubsystem final : public RecordingSubsystem {
 public:
  explicit SameNameSubsystem(Trace& trace)
      : RecordingSubsystem("root", {}, trace) {}
};

class UnregisteredSubsystem final : public RecordingSubsystem {
 public:
  explicit UnregisteredSubsystem(Trace& trace)
      : RecordingSubsystem("unregistered", {}, trace) {}
};

class MissingDependencySubsystem final : public RecordingSubsystem {
 public:
  explicit MissingDependencySubsystem(Trace& trace)
      : RecordingSubsystem(
            "missing", subsystem_dependencies<UnregisteredSubsystem>(), trace) {
  }
};

class DuplicateDependencySubsystem final : public RecordingSubsystem {
 public:
  explicit DuplicateDependencySubsystem(Trace& trace)
      : RecordingSubsystem(
            "duplicate-dependency",
            subsystem_dependencies<RootSubsystem, RootSubsystem>(), trace) {}
};

class CycleTwoSubsystem;

class CycleOneSubsystem final : public RecordingSubsystem {
 public:
  explicit CycleOneSubsystem(Trace& trace);
};

class CycleTwoSubsystem final : public RecordingSubsystem {
 public:
  explicit CycleTwoSubsystem(Trace& trace);
};

CycleOneSubsystem::CycleOneSubsystem(Trace& trace)
    : RecordingSubsystem("cycle-one",
                         subsystem_dependencies<CycleTwoSubsystem>(), trace) {}

CycleTwoSubsystem::CycleTwoSubsystem(Trace& trace)
    : RecordingSubsystem("cycle-two",
                         subsystem_dependencies<CycleOneSubsystem>(), trace) {}

static_assert(ApplicationSubsystem<RootSubsystem>);
static_assert(!ApplicationSubsystem<AppSubsystem>);

TEST(AppLifecycleStatusTest, ReportsStableHumanReadableResults) {
  EXPECT_TRUE(is_ok(Status::OK));
  for (const Status status : {
           Status::INVALID_ARGUMENT,
           Status::DUPLICATE_SUBSYSTEM,
           Status::DUPLICATE_SUBSYSTEM_NAME,
           Status::SUBSYSTEM_NOT_FOUND,
           Status::MISSING_DEPENDENCY,
           Status::DUPLICATE_DEPENDENCY,
           Status::DEPENDENCY_CYCLE,
           Status::REGISTRATION_FROZEN,
           Status::INVALID_LIFECYCLE_TRANSITION,
           Status::SUBSYSTEM_FAILURE,
           Status::INTERNAL_ERROR,
       }) {
    EXPECT_FALSE(is_ok(status));
    EXPECT_FALSE(status_message(status).empty());
    EXPECT_NE(status_message(status), "unknown application lifecycle status");
  }
  EXPECT_EQ(status_message(static_cast<Status>(-1)),
            "unknown application lifecycle status");
}

TEST(AppStateTest, RegistersAndResolvesAdaptersByConcreteType) {
  Trace trace;
  AppState app;
  std::unique_ptr<RootSubsystem> null_root;
  EXPECT_EQ(app.register_subsystem(std::move(null_root)),
            Status::INVALID_ARGUMENT);

  auto root              = std::make_unique<RootSubsystem>(trace);
  RootSubsystem* pointer = root.get();
  ASSERT_EQ(app.register_subsystem(std::move(root)), Status::OK);
  EXPECT_EQ(app.size(), 1U);
  EXPECT_EQ(app.get_subsystem<RootSubsystem>(), pointer);
  EXPECT_EQ(app.find_subsystem(SubsystemId{typeid(RootSubsystem)}), pointer);
  EXPECT_EQ(app.get_subsystem<LeafSubsystem>(), nullptr);

  EXPECT_EQ(app.register_subsystem(std::make_unique<RootSubsystem>(trace)),
            Status::DUPLICATE_SUBSYSTEM);
  EXPECT_EQ(app.register_subsystem(std::make_unique<SameNameSubsystem>(trace)),
            Status::DUPLICATE_SUBSYSTEM_NAME);

  const AppState& const_app = app;
  EXPECT_EQ(const_app.get_subsystem<RootSubsystem>(), pointer);
  EXPECT_EQ(const_app.find_subsystem(SubsystemId{typeid(LeafSubsystem)}),
            nullptr);
}

TEST(AppStateTest, RunsForwardAndReverseDependencyLayers) {
  Trace trace;
  AppState app;
  ASSERT_EQ(app.register_subsystem(std::make_unique<RootSubsystem>(trace)),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<LeftSubsystem>(trace)),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<RightSubsystem>(trace)),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<LeafSubsystem>(trace)),
            Status::OK);

  EXPECT_EQ(app.initialize(OperatingMode::TEST), Status::OK);
  EXPECT_EQ(app.lifecycle_state(), LifecycleState::INITIALIZED);
  EXPECT_EQ(app.operating_mode(), OperatingMode::TEST);
  EXPECT_TRUE(app.registration_frozen());
  EXPECT_EQ(trace.calls,
            (std::vector<std::string>{"root.initialize", "left.initialize",
                                      "right.initialize", "leaf.initialize"}));

  trace.calls.clear();
  EXPECT_EQ(app.start(), Status::OK);
  EXPECT_EQ(app.lifecycle_state(), LifecycleState::RUNNING);
  EXPECT_EQ(trace.calls,
            (std::vector<std::string>{"root.start", "left.start", "right.start",
                                      "leaf.start"}));

  trace.calls.clear();
  EXPECT_EQ(app.stop(), Status::OK);
  EXPECT_EQ(app.stop(), Status::OK);
  EXPECT_EQ(app.lifecycle_state(), LifecycleState::STOPPED);
  EXPECT_EQ(trace.calls, (std::vector<std::string>{"leaf.stop", "left.stop",
                                                   "right.stop", "root.stop"}));

  trace.calls.clear();
  EXPECT_EQ(app.terminate(), Status::OK);
  EXPECT_EQ(app.terminate(), Status::OK);
  EXPECT_EQ(app.lifecycle_state(), LifecycleState::TERMINATED);
  EXPECT_EQ(trace.calls,
            (std::vector<std::string>{"leaf.terminate", "left.terminate",
                                      "right.terminate", "root.terminate"}));
}

TEST(AppStateTest, InitializesAndTerminatesOnceAcrossRepeatedRunCycles) {
  Trace trace;
  AppState app;
  ASSERT_EQ(app.register_subsystem(std::make_unique<RootSubsystem>(trace)),
            Status::OK);

  ASSERT_EQ(app.initialize(OperatingMode::TEST), Status::OK);
  for (std::size_t generation = 0U; generation < 3U; ++generation) {
    ASSERT_EQ(app.start(), Status::OK);
    EXPECT_EQ(app.lifecycle_state(), LifecycleState::RUNNING);
    ASSERT_EQ(app.stop(), Status::OK);
    EXPECT_EQ(app.lifecycle_state(), LifecycleState::STOPPED);
  }
  ASSERT_EQ(app.terminate(), Status::OK);
  EXPECT_EQ(app.terminate(), Status::OK);
  EXPECT_EQ(app.initialize(OperatingMode::TEST),
            Status::INVALID_LIFECYCLE_TRANSITION);

  EXPECT_EQ(trace.calls,
            (std::vector<std::string>{
                "root.initialize", "root.start", "root.stop", "root.start",
                "root.stop", "root.start", "root.stop", "root.terminate"}));
}

TEST(AppStateTest, FreezesRegistrationAtInitialization) {
  Trace trace;
  AppState app;
  ASSERT_EQ(app.register_subsystem(std::make_unique<RootSubsystem>(trace)),
            Status::OK);
  ASSERT_EQ(app.initialize(), Status::OK);
  EXPECT_EQ(app.register_subsystem(std::make_unique<LeftSubsystem>(trace)),
            Status::REGISTRATION_FROZEN);
  EXPECT_EQ(app.initialize(), Status::INVALID_LIFECYCLE_TRANSITION);
}

TEST(AppStateTest, RejectsMissingDuplicateAndCyclicDependencies) {
  {
    Trace trace;
    AppState app;
    ASSERT_EQ(app.register_subsystem(
                  std::make_unique<MissingDependencySubsystem>(trace)),
              Status::OK);
    EXPECT_EQ(app.initialize(), Status::MISSING_DEPENDENCY);
    EXPECT_EQ(app.lifecycle_state(), LifecycleState::CRASHED);
    EXPECT_TRUE(trace.calls.empty());
  }
  {
    Trace trace;
    AppState app;
    ASSERT_EQ(app.register_subsystem(std::make_unique<RootSubsystem>(trace)),
              Status::OK);
    ASSERT_EQ(app.register_subsystem(
                  std::make_unique<DuplicateDependencySubsystem>(trace)),
              Status::OK);
    EXPECT_EQ(app.initialize(), Status::DUPLICATE_DEPENDENCY);
    EXPECT_TRUE(trace.calls.empty());
  }
  {
    Trace trace;
    AppState app;
    ASSERT_EQ(
        app.register_subsystem(std::make_unique<CycleOneSubsystem>(trace)),
        Status::OK);
    ASSERT_EQ(
        app.register_subsystem(std::make_unique<CycleTwoSubsystem>(trace)),
        Status::OK);
    EXPECT_EQ(app.initialize(), Status::DEPENDENCY_CYCLE);
    EXPECT_TRUE(trace.calls.empty());
  }
}

TEST(AppStateTest, RollsBackSuccessfulInitializationPrefix) {
  Trace trace;
  AppState app;
  ASSERT_EQ(app.register_subsystem(std::make_unique<RootSubsystem>(trace)),
            Status::OK);
  auto left               = std::make_unique<LeftSubsystem>(trace);
  left->initialize_status = Status::SUBSYSTEM_FAILURE;
  ASSERT_EQ(app.register_subsystem(std::move(left)), Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<RightSubsystem>(trace)),
            Status::OK);

  EXPECT_EQ(app.initialize(), Status::SUBSYSTEM_FAILURE);
  EXPECT_EQ(app.lifecycle_state(), LifecycleState::CRASHED);
  EXPECT_EQ(trace.calls,
            (std::vector<std::string>{"root.initialize", "left.initialize",
                                      "root.terminate"}));
  EXPECT_EQ(app.terminate(), Status::OK);
  EXPECT_EQ(app.lifecycle_state(), LifecycleState::TERMINATED);
}

TEST(AppStateTest, ConvertsInitializationExceptionsAndRollsBack) {
  Trace trace;
  AppState app;
  auto root                     = std::make_unique<RootSubsystem>(trace);
  root->throw_during_initialize = true;
  ASSERT_EQ(app.register_subsystem(std::move(root)), Status::OK);

  EXPECT_EQ(app.initialize(), Status::SUBSYSTEM_FAILURE);
  EXPECT_EQ(app.lifecycle_state(), LifecycleState::CRASHED);
  EXPECT_EQ(trace.calls, (std::vector<std::string>{"root.initialize"}));
}

TEST(AppStateTest, RollsBackSuccessfulStartupPrefixAndCanRetry) {
  Trace trace;
  AppState app;
  ASSERT_EQ(app.register_subsystem(std::make_unique<RootSubsystem>(trace)),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<LeftSubsystem>(trace)),
            Status::OK);
  auto right                    = std::make_unique<RightSubsystem>(trace);
  RightSubsystem* right_pointer = right.get();
  right->start_status           = Status::SUBSYSTEM_FAILURE;
  ASSERT_EQ(app.register_subsystem(std::move(right)), Status::OK);
  ASSERT_EQ(app.initialize(), Status::OK);

  trace.calls.clear();
  EXPECT_EQ(app.start(), Status::SUBSYSTEM_FAILURE);
  EXPECT_EQ(app.lifecycle_state(), LifecycleState::INITIALIZED);
  EXPECT_EQ(trace.calls,
            (std::vector<std::string>{"root.start", "left.start", "right.start",
                                      "left.stop", "root.stop"}));

  right_pointer->start_status = Status::OK;
  trace.calls.clear();
  EXPECT_EQ(app.start(), Status::OK);
  EXPECT_EQ(app.stop(), Status::OK);
  EXPECT_EQ(trace.calls,
            (std::vector<std::string>{"root.start", "left.start", "right.start",
                                      "left.stop", "right.stop", "root.stop"}));
}

TEST(AppStateTest, StopFailurePreservesPrerequisitesUntilTermination) {
  Trace trace;
  AppState app;
  ASSERT_EQ(app.register_subsystem(std::make_unique<RootSubsystem>(trace)),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<LeftSubsystem>(trace)),
            Status::OK);
  ASSERT_EQ(app.register_subsystem(std::make_unique<RightSubsystem>(trace)),
            Status::OK);
  auto leaf                   = std::make_unique<LeafSubsystem>(trace);
  LeafSubsystem* leaf_pointer = leaf.get();
  leaf->stop_status           = Status::SUBSYSTEM_FAILURE;
  ASSERT_EQ(app.register_subsystem(std::move(leaf)), Status::OK);
  ASSERT_EQ(app.initialize(), Status::OK);
  ASSERT_EQ(app.start(), Status::OK);

  trace.calls.clear();
  EXPECT_EQ(app.stop(), Status::SUBSYSTEM_FAILURE);
  EXPECT_EQ(app.lifecycle_state(), LifecycleState::CRASHED);
  EXPECT_EQ(trace.calls, (std::vector<std::string>{"leaf.stop"}));

  leaf_pointer->stop_status = Status::OK;
  trace.calls.clear();
  EXPECT_EQ(app.terminate(), Status::OK);
  EXPECT_EQ(app.lifecycle_state(), LifecycleState::TERMINATED);
  EXPECT_EQ(trace.calls, (std::vector<std::string>{
                             "leaf.stop", "left.stop", "right.stop",
                             "root.stop", "leaf.terminate", "left.terminate",
                             "right.terminate", "root.terminate"}));
}

TEST(AppStateTest, RejectsInvalidTransitionsWithoutFreezingValidRetry) {
  Trace trace;
  AppState app;
  EXPECT_EQ(app.start(), Status::INVALID_LIFECYCLE_TRANSITION);
  EXPECT_EQ(app.stop(), Status::OK);
  EXPECT_EQ(app.initialize(static_cast<OperatingMode>(-1)),
            Status::INVALID_ARGUMENT);
  EXPECT_FALSE(app.registration_frozen());

  ASSERT_EQ(app.register_subsystem(std::make_unique<RootSubsystem>(trace)),
            Status::OK);
  EXPECT_EQ(app.initialize(), Status::OK);
  EXPECT_EQ(app.start(), Status::OK);
  EXPECT_EQ(app.initialize(), Status::INVALID_LIFECYCLE_TRANSITION);
  EXPECT_EQ(app.terminate(), Status::OK);
  EXPECT_EQ(app.start(), Status::INVALID_LIFECYCLE_TRANSITION);
}

TEST(AppStateTest, DestructorStopsAndTerminatesRunningSubsystems) {
  Trace trace;
  {
    AppState app;
    ASSERT_EQ(app.register_subsystem(std::make_unique<RootSubsystem>(trace)),
              Status::OK);
    ASSERT_EQ(app.initialize(), Status::OK);
    ASSERT_EQ(app.start(), Status::OK);
    trace.calls.clear();
  }
  EXPECT_EQ(trace.calls,
            (std::vector<std::string>{"root.stop", "root.terminate"}));
}

}  // namespace
}  // namespace puc::app
