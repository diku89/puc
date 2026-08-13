/**
 * @file config_test.cpp
 * @brief End-to-end tests for config get, set, list, and reload.
 */

#include "commands/config.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "msgs/cmdframe_msgs.hpp"
#include "properties/properties.hpp"
#include "utils/ipc/channel.hpp"
#include "utils/ipc/directory.hpp"
#include "utils/ipc/smem_channel.hpp"
#include "utils/multithreading/job_queue.hpp"

namespace puc::command {
namespace {

class ConfigCommandTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    root_ = std::filesystem::temp_directory_path() /
            ("puc-config-command-" + std::to_string(suffix));
    ASSERT_TRUE(std::filesystem::create_directories(root_));
    {
      std::ofstream defaults(root_ / "defaults.toml");
      defaults << "[settings]\nname = \"default\"\ncount = 2\n";
    }
    {
      std::ofstream immutable(root_ / "immutable.toml");
      immutable << "version = 1\n";
    }
    properties_ = std::make_unique<properties::Properties>(
        root_, root_ / ".no-user-overrides");
    ASSERT_EQ(properties_->load_mutable_defaults("app", "defaults.toml"),
              properties::Status::OK);
    ASSERT_TRUE(properties_->load_immutable("build", "immutable.toml"));

    workers_   = std::make_unique<multithreading::JobQueue>(1U);
    directory_ = std::make_unique<ipc::Directory>(*workers_);
    channel_   = std::make_shared<ipc::SmemChannel>(
        std::string{msg::kCmdFrameNotifyChannel},
        ipc::kDefaultMaximumMessageBytes);
    ipc::ChannelId channel_id = 0U;
    ASSERT_EQ(directory_->open_channel(channel_, channel_id), ipc::Status::OK);
    ASSERT_EQ(
        directory_->subscribe(
            msg::kCmdFrameNotifyChannel,
            [this](ipc::Channel::Bytes payload) noexcept {
              try {
                msg::CmdFrameNotification notification;
                if (msg::is_ok(codec_.deserialize(payload, notification))) {
                  notifications_.push_back(std::move(notification.text));
                }
              } catch (...) {
                callback_failed_ = true;
              }
            },
            subscription_),
        ipc::Status::OK);
  }

  void TearDown() override {
    EXPECT_FALSE(callback_failed_);
    subscription_.reset();
    directory_.reset();
    workers_->wait();
    workers_.reset();
    properties_.reset();
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  CommonCommandArgs args() {
    return CommonCommandArgs{.directory  = directory_.get(),
                             .properties = properties_.get()};
  }

  std::filesystem::path root_;
  std::unique_ptr<properties::Properties> properties_;
  std::unique_ptr<multithreading::JobQueue> workers_;
  std::unique_ptr<ipc::Directory> directory_;
  std::shared_ptr<ipc::Channel> channel_;
  ipc::Subscription subscription_;
  msg::CmdFrameNotificationCodec codec_;
  std::vector<std::string> notifications_;
  bool callback_failed_ = false;
};

TEST_F(ConfigCommandTest, GetsSetsAndListsMutableProperties) {
  ConfigCommand command;
  EXPECT_EQ(command.run(args(),
                        std::vector<std::string>{"get", "app.settings.count"}),
            Status::OK);
  ASSERT_EQ(notifications_.size(), 1U);
  EXPECT_EQ(notifications_.back(), "app.settings.count\t2");

  EXPECT_EQ(
      command.run(args(),
                  std::vector<std::string>{"set", "app.settings.count", "7"}),
      Status::OK);
  EXPECT_EQ(notifications_.back(), "app.settings.count\t7");

  EXPECT_EQ(
      command.run(args(), std::vector<std::string>{"list", "app.settings"}),
      Status::OK);
  EXPECT_EQ(notifications_.back(),
            "app.settings.count\t7\napp.settings.name\t\"default\"");
}

TEST_F(ConfigCommandTest, ReloadPreservesUserChangesAndRejectsBadRequests) {
  ConfigCommand command;
  ASSERT_EQ(
      command.run(args(),
                  std::vector<std::string>{"set", "app.settings.count", "9"}),
      Status::OK);
  EXPECT_EQ(command.run(args(), std::vector<std::string>{"reload"}),
            Status::OK);
  EXPECT_EQ(notifications_.back(), "Properties reloaded.");
  properties::Property property;
  ASSERT_EQ(properties_->get("app.settings.count", property),
            properties::Status::OK);
  EXPECT_EQ(std::get<std::int64_t>(property.value), 9);

  EXPECT_EQ(command.run(args(), std::vector<std::string>{"get", "missing"}),
            Status::INVALID_ARGUMENT);
  EXPECT_EQ(command.run(args(),
                        std::vector<std::string>{"set", "build.version", "2"}),
            Status::INVALID_ARGUMENT);
  EXPECT_EQ(command.run(args(), std::vector<std::string>{"reload", "extra"}),
            Status::INVALID_ARGUMENT);
  EXPECT_EQ(command.run(CommonCommandArgs{}, std::vector<std::string>{"list"}),
            Status::INVALID_ARGUMENT);
}

}  // namespace
}  // namespace puc::command
