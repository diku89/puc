/**
 * @file command_test.cpp
 * @brief Tests for command registration, completion, metadata, and dispatch.
 */

#include "commands/command.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "commands/quit.hpp"
#include "gtest/gtest.h"
#include "msgs/cmdframe_msgs.hpp"
#include "puc-cli/state/application_control.hpp"
#include "utils/ipc/channel.hpp"
#include "utils/ipc/directory.hpp"
#include "utils/ipc/smem_channel.hpp"
#include "utils/multithreading/job_queue.hpp"

namespace puc::command {
namespace {

using namespace std::chrono_literals;

/** Mutable test command recording dispatch while exposing fixed metadata. */
class RecordingCommand final : public CommandApp {
 public:
  RecordingCommand(std::string description, std::string usage,
                   Status result = Status::OK)
      : description_(std::move(description)),
        usage_(std::move(usage)),
        result_(result) {}

  Status run(CommonCommandArgs, std::span<const std::string> args) override {
    ++run_count_;
    arguments_.assign(args.begin(), args.end());
    return result_;
  }

  std::string get_description() const override { return description_; }

  std::string get_usage() const override { return usage_; }

  std::size_t run_count() const noexcept { return run_count_; }

  const std::vector<std::string>& arguments() const noexcept {
    return arguments_;
  }

 private:
  std::string description_;
  std::string usage_;
  Status result_         = Status::OK;
  std::size_t run_count_ = 0U;
  std::vector<std::string> arguments_;
};

/** Command that exercises registry reentry from inside dispatch. */
class RegisteringCommand final : public CommandApp {
 public:
  RegisteringCommand(CommandDispatcher& dispatcher,
                     std::shared_ptr<CommandApp> nested)
      : dispatcher_(dispatcher), nested_(std::move(nested)) {}

  Status run(CommonCommandArgs, std::span<const std::string>) override {
    return dispatcher_.register_command("nested", {}, nested_);
  }

  std::string get_description() const override { return "Register nested"; }

  std::string get_usage() const override { return {}; }

 private:
  CommandDispatcher& dispatcher_;
  std::shared_ptr<CommandApp> nested_;
};

/** Register the command notification route for producer-focused tests. */
std::shared_ptr<ipc::Channel> open_notification_channel(
    ipc::Directory& directory) {
  auto channel = std::make_shared<ipc::SmemChannel>(
      std::string{msg::kCmdFrameNotifyChannel},
      ipc::kDefaultMaximumMessageBytes,
      ipc::ChannelOptions{.channel_max_depth = 1U});
  ipc::ChannelId channel_id = 0U;
  return ipc::is_ok(directory.open_channel(channel, channel_id))
             ? std::move(channel)
             : nullptr;
}

TEST(CommandStatusTest, ReportsStableHumanReadableResults) {
  EXPECT_TRUE(is_ok(Status::OK));
  for (const Status status : {
           Status::INVALID_ARGUMENT,
           Status::DUPLICATE_COMMAND_NAME,
           Status::COMMAND_NOT_FOUND,
           Status::MESSAGE_ENCODING_FAILED,
           Status::NOTIFICATION_FAILED,
           Status::NOT_ALLOWED,
           Status::INTERNAL_ERROR,
       }) {
    EXPECT_FALSE(is_ok(status));
    EXPECT_FALSE(status_message(status).empty());
    EXPECT_NE(status_message(status), "unknown command status");
  }
  EXPECT_EQ(status_message(static_cast<Status>(-1)), "unknown command status");
}

TEST(QuitCommandTest, RequestsDeferredExitWithoutChangingLifecycleInline) {
  app::ApplicationControl control;
  QuitCommand command;

  EXPECT_FALSE(control.exit_requested());
  EXPECT_EQ(command.run(CommonCommandArgs{.control = &control}, {}),
            Status::OK);
  EXPECT_TRUE(control.exit_requested());
  EXPECT_EQ(command.run(CommonCommandArgs{.control = &control}, {}),
            Status::OK);
  EXPECT_TRUE(control.exit_requested());
}

TEST(QuitCommandTest, RejectsArgumentsAndMissingApplicationControl) {
  app::ApplicationControl control;
  QuitCommand command;
  const std::vector<std::string> arguments{"now"};

  EXPECT_EQ(command.run(CommonCommandArgs{.control = &control}, arguments),
            Status::INVALID_ARGUMENT);
  EXPECT_FALSE(control.exit_requested());
  EXPECT_EQ(command.run(CommonCommandArgs{}, {}), Status::INVALID_ARGUMENT);
  EXPECT_EQ(command.get_description(), "Quit puc.");
  EXPECT_TRUE(command.get_usage().empty());
  EXPECT_EQ(QuitCommand::get_aliases(),
            (std::vector<std::string>{"q", "exit"}));
}

TEST(CommandDispatcherTest, RegistersCanonicalNamesAndAliases) {
  CommandDispatcher dispatcher;
  auto command = std::make_shared<RecordingCommand>("Quit puc.", std::string{});

  EXPECT_EQ(dispatcher.register_command("quit", {"q", "exit"}, command),
            Status::OK);
  EXPECT_TRUE(dispatcher.contains("quit"));
  EXPECT_TRUE(dispatcher.contains("q"));
  EXPECT_TRUE(dispatcher.contains("exit"));
  EXPECT_FALSE(dispatcher.contains("missing"));
  EXPECT_FALSE(dispatcher.contains("bad name"));
}

TEST(CommandDispatcherTest, RejectsInvalidAndDuplicateSpellingsAtomically) {
  CommandDispatcher dispatcher;
  auto first  = std::make_shared<RecordingCommand>("First", "");
  auto second = std::make_shared<RecordingCommand>("Second", "");

  EXPECT_EQ(dispatcher.register_command("", {}, first),
            Status::INVALID_ARGUMENT);
  EXPECT_EQ(dispatcher.register_command("first", {"bad alias"}, first),
            Status::INVALID_ARGUMENT);
  EXPECT_EQ(dispatcher.register_command("first", {"first"}, first),
            Status::DUPLICATE_COMMAND_NAME);
  EXPECT_EQ(dispatcher.register_command("first", {"f"}, first), Status::OK);

  EXPECT_EQ(dispatcher.register_command("second", {"s", "f"}, second),
            Status::DUPLICATE_COMMAND_NAME);
  EXPECT_FALSE(dispatcher.contains("second"));
  EXPECT_FALSE(dispatcher.contains("s"));
  EXPECT_TRUE(dispatcher.contains("first"));
  EXPECT_TRUE(dispatcher.contains("f"));
  EXPECT_EQ(dispatcher.register_command("null", {}, nullptr),
            Status::INVALID_ARGUMENT);
}

TEST(CommandDispatcherTest, ListsPrefixCompletionsInStableBranchOrder) {
  CommandDispatcher dispatcher;
  auto quit = std::make_shared<RecordingCommand>("Quit puc.", "");
  auto config =
      std::make_shared<RecordingCommand>("Edit configuration.", "get\nset");
  ASSERT_EQ(dispatcher.register_command("quit", {"q", "exit"}, quit),
            Status::OK);
  ASSERT_EQ(dispatcher.register_command("config", {}, config), Status::OK);

  EXPECT_EQ(dispatcher.list_completions("q"),
            (std::vector<std::string>{"q", "quit"}));
  EXPECT_EQ(dispatcher.list_completions("qu"),
            (std::vector<std::string>{"quit"}));
  EXPECT_EQ(dispatcher.list_completions("con"),
            (std::vector<std::string>{"config"}));
  EXPECT_EQ(dispatcher.list_completions(),
            (std::vector<std::string>{"q", "quit", "exit", "config"}));
  EXPECT_TRUE(dispatcher.list_completions("unknown").empty());
  EXPECT_TRUE(dispatcher.list_completions("bad prefix").empty());
}

TEST(CommandDispatcherTest, ResolvesMetadataThroughAliases) {
  CommandDispatcher dispatcher;
  auto config = std::make_shared<RecordingCommand>(
      "Edit configuration.", "get <key>\nset <key> <value>");
  ASSERT_EQ(dispatcher.register_command("config", {"cfg"}, config), Status::OK);

  EXPECT_EQ(dispatcher.get_command_description("config"),
            "Edit configuration.");
  EXPECT_EQ(dispatcher.get_command_description("cfg"), "Edit configuration.");
  EXPECT_EQ(dispatcher.get_command_usage("cfg"),
            "get <key>\nset <key> <value>");
  EXPECT_TRUE(dispatcher.get_command_description("missing").empty());
  EXPECT_TRUE(dispatcher.get_command_usage("missing").empty());
}

TEST(CommandDispatcherTest, DispatchesAliasesAndPropagatesCommandResults) {
  CommandDispatcher dispatcher;
  auto command = std::make_shared<RecordingCommand>(
      "Restricted command", "<argument>", Status::NOT_ALLOWED);
  ASSERT_EQ(dispatcher.register_command("restricted", {"r"}, command),
            Status::OK);
  const std::vector<std::string> arguments{"one", "two"};

  EXPECT_EQ(dispatcher.dispatch("r", CommonCommandArgs{}, arguments),
            Status::NOT_ALLOWED);
  EXPECT_EQ(command->run_count(), 1U);
  EXPECT_EQ(command->arguments(), arguments);
  EXPECT_EQ(dispatcher.dispatch("missing", CommonCommandArgs{}, arguments),
            Status::COMMAND_NOT_FOUND);
  EXPECT_EQ(dispatcher.dispatch("bad name", CommonCommandArgs{}, arguments),
            Status::INVALID_ARGUMENT);
  EXPECT_EQ(command->run_count(), 1U);
}

TEST(CommandDispatcherTest, ReleasesRegistryLockBeforeRunningCommandCode) {
  CommandDispatcher dispatcher;
  auto nested      = std::make_shared<RecordingCommand>("Nested", "");
  auto registering = std::make_shared<RegisteringCommand>(dispatcher, nested);
  ASSERT_EQ(dispatcher.register_command("register", {}, registering),
            Status::OK);

  EXPECT_EQ(dispatcher.dispatch("register", CommonCommandArgs{}, {}),
            Status::OK);
  EXPECT_TRUE(dispatcher.contains("nested"));
}

TEST(CommandNotificationTest, PublishesTypedUtf8ThroughCanonicalChannel) {
  auto workers   = std::make_shared<multithreading::JobQueue>(2U);
  auto directory = std::make_shared<ipc::Directory>(*workers);
  const std::shared_ptr<ipc::Channel> channel =
      open_notification_channel(*directory);
  ASSERT_NE(channel, nullptr);

  std::mutex mutex;
  std::condition_variable changed;
  std::optional<msg::CmdFrameNotification> received;
  ipc::Subscription subscription;
  ASSERT_EQ(directory->subscribe(
                msg::kCmdFrameNotifyChannel,
                [&](ipc::Channel::Bytes payload) noexcept {
                  msg::CmdFrameNotification notification;
                  if (msg::is_ok(msg::CmdFrameNotificationCodec{}.deserialize(
                          payload, notification))) {
                    {
                      const std::lock_guard lock(mutex);
                      received = std::move(notification);
                    }
                    changed.notify_all();
                  }
                },
                subscription),
            ipc::Status::OK);

  EXPECT_EQ(send_notification(CommonCommandArgs{.directory = directory.get()},
                              "building ✓"),
            Status::OK);
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(
        changed.wait_for(lock, 2s, [&] { return received.has_value(); }));
    EXPECT_EQ(received->text, "building ✓");
  }

  subscription.reset();
  directory.reset();
  workers->wait();
}

TEST(CommandNotificationTest, ReportsSetupEncodingAndDeliveryFailures) {
  auto workers   = std::make_shared<multithreading::JobQueue>();
  auto directory = std::make_shared<ipc::Directory>(*workers);
  EXPECT_EQ(send_notification(CommonCommandArgs{}, "missing directory"),
            Status::INVALID_ARGUMENT);
  EXPECT_EQ(send_notification(CommonCommandArgs{.directory = directory.get()},
                              "missing channel"),
            Status::NOTIFICATION_FAILED);

  const std::shared_ptr<ipc::Channel> channel =
      open_notification_channel(*directory);
  ASSERT_NE(channel, nullptr);
  EXPECT_EQ(send_notification(CommonCommandArgs{.directory = directory.get()},
                              std::string{"\xf0\x28\x8c\x28", 4U}),
            Status::MESSAGE_ENCODING_FAILED);
  directory.reset();
  workers->wait();
}

}  // namespace
}  // namespace puc::command
