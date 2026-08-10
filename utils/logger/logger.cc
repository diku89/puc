#include "utils/logger/logger.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace puc::logger {
namespace {

std::mutex global_logger_mutex;
std::shared_ptr<Logger> global_logger;

constexpr std::string_view color(LogLevel level) {
  switch (level) {
    case LogLevel::DEBUG:
      return "\x1b[36m";
    case LogLevel::INFO:
      return "\x1b[32m";
    case LogLevel::WARN:
      return "\x1b[33m";
    case LogLevel::ERROR:
      return "\x1b[31m";
  }
  return {};
}

std::string timestamp() {
  const auto now         = std::chrono::system_clock::now();
  const std::time_t time = std::chrono::system_clock::to_time_t(now);
  std::tm local_time{};
  localtime_r(&time, &local_time);

  const auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now.time_since_epoch()) %
      std::chrono::seconds(1);

  std::ostringstream result;
  result << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S") << ':'
         << std::setfill('0') << std::setw(3) << milliseconds.count();
  return result.str();
}

bool enabled(LogLevel message_level, LogLevel module_level,
             LogLevel global_level) {
  return message_level >= module_level && message_level >= global_level;
}

}  // namespace

Logger::Logger(const LoggerConf& config) : config_(config) {
  if (!config_.logfile.has_value()) {
    return;
  }

  logfile_.open(*config_.logfile, std::ios::app);
  if (!logfile_.is_open()) {
    throw std::runtime_error("could not open logger file: " +
                             config_.logfile->string());
  }
}

void Logger::emit(LogLevel level, const ModuleConf& module,
                  const std::string& message) {
  if (!enabled(level, module.level, config_.global_level)) {
    return;
  }

  const std::string line = timestamp() + " " + module.name + " " + message;
  const std::scoped_lock lock(mutex_);

  std::ostream& console = level == LogLevel::ERROR ? std::cerr : std::cout;
  console << color(level) << line << "\x1b[0m\n";
  console.flush();

  if (logfile_.is_open()) {
    logfile_ << line << '\n';
    logfile_.flush();
  }
}

void init_logger(const LoggerConf& config) {
  auto logger = std::make_shared<Logger>(config);
  const std::scoped_lock lock(global_logger_mutex);
  global_logger = std::move(logger);
}

std::shared_ptr<Logger> get_logger() {
  const std::scoped_lock lock(global_logger_mutex);
  return global_logger;
}

bool clear_logger(const std::shared_ptr<Logger>& expected) noexcept {
  if (expected == nullptr) {
    return false;
  }
  const std::scoped_lock lock(global_logger_mutex);
  if (global_logger != expected) {
    return false;
  }
  global_logger.reset();
  return true;
}

}  // namespace puc::logger
