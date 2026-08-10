#pragma once

/**
 * @file logger.hpp
 * Logging configuration, initialization, and stream-style logging interfaces.
 *
 * @code{.cpp}
 * #include "utils/logger/logger.hpp"
 *
 * LOGGER_MODULE("Scheduler");
 *
 * int main() {
 *   const puc::logger::LoggerConf config{
 *       .global_level = puc::logger::LogLevel::INFO,
 *       .logfile = "puc.log",
 *   };
 *   LOGGER_INIT(config);
 *
 *   Logger<INFO> << "Scheduler started";
 *   Logger<ERROR> << "Scheduler stopped unexpectedly";
 * }
 * @endcode
 */

#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace puc::logger {

/**
 * Log levels.
 */
enum class LogLevel {
  DEBUG, /**< Debug logs. */
  INFO,  /**< Informational logs. */
  WARN,  /**< Warning logs. */
  ERROR, /**< Error logs. */
};

/**
 * Logger configuration.
 */
struct LoggerConf {
  /** Minimum log level enabled globally. */
  LogLevel global_level = LogLevel::INFO;

  /** Optional file to which uncolored log lines are appended. */
  std::optional<std::filesystem::path> logfile;
};

/**
 * Module specific configuration.
 */
struct ModuleConf {
  /** Module name. */
  std::string name;

  /** Minimum log level enabled for this module. */
  LogLevel level = LogLevel::DEBUG;
};

/**
 * Thread-safe logger that writes messages to the configured output streams.
 *
 * Messages below either the global or module-specific log level are ignored.
 * Error messages are written to standard error; all other messages are written
 * to standard output. If configured, every emitted message is also appended to
 * the log file without terminal color codes.
 */
class Logger {
 public:
  /**
   * Construct a logger from a configuration.
   *
   * @param[in] config Logger configuration.
   * @throws std::runtime_error If the configured log file cannot be opened.
   */
  explicit Logger(const LoggerConf& config);

  /**
   * Emit a timestamped log line if its level is enabled.
   *
   * @param[in] level Log level.
   * @param[in] module Module configuration.
   * @param[in] message Log message.
   */
  void emit(LogLevel level, const ModuleConf& module,
            const std::string& message);

 private:
  /** Immutable logger configuration. */
  LoggerConf config_;

  /** Serializes writes to the console and log file. */
  std::mutex mutex_;

  /** Configured log file stream, or a closed stream if no file is configured.
   */
  std::ofstream logfile_;
};

/**
 * Initialize the global logger with the provided configuration.
 *
 * Use LOGGER_INIT() instead of calling this function directly. Calling it
 * again atomically replaces the global logger for subsequent messages.
 *
 * @param[in] config Global logger configuration.
 * @throws std::runtime_error If the configured log file cannot be opened.
 */
void init_logger(const LoggerConf& config);

/**
 * Get the current global logger instance.
 *
 * @return The logger, or an empty std::shared_ptr if LOGGER_INIT() has not yet
 *         been called.
 */
std::shared_ptr<Logger> get_logger();

/**
 * Clear the global logger only when it is still the expected instance.
 *
 * This compare-and-clear operation lets a lifecycle owner release the logger
 * it installed without erasing a newer logger installed by another owner.
 * Passing an empty pointer is harmless and returns false.
 *
 * @param[in] expected Logger instance the caller previously installed.
 * @return True when the matching global logger was cleared.
 */
bool clear_logger(const std::shared_ptr<Logger>& expected) noexcept;

/**
 * Accumulates one stream-style log line and emits it at end of expression.
 *
 * Applications normally create log lines through the Logger variable template
 * declared by LOGGER_MODULE(), rather than constructing this class directly.
 *
 * @tparam Level Log level assigned to the line.
 */
template <LogLevel Level>
class LogLine {
 public:
  /**
   * Construct a log line and append its first value.
   *
   * @tparam Value Type of the first streamed value.
   * @param[in] module Module configuration associated with the line.
   * @param[in] value First value to append.
   */
  template <typename Value>
  LogLine(const ModuleConf& module, Value&& value) : module_(module) {
    stream_ << std::forward<Value>(value);
  }

  /** Log lines cannot be copied. */
  LogLine(const LogLine&) = delete;

  /** Log lines cannot be copy-assigned. */
  LogLine& operator=(const LogLine&) = delete;

  /** Emit the accumulated message without propagating logging failures. */
  ~LogLine() noexcept {
    try {
      if (const auto logger = get_logger()) {
        logger->emit(Level, module_, stream_.str());
      }
    } catch (...) {
      // Logging must not terminate the application.
    }
  }

  /**
   * Append a value to this log line.
   *
   * @tparam Value Type of the streamed value.
   * @param[in] value Value to append.
   * @return This log line, allowing additional streaming operations.
   */
  template <typename Value>
  LogLine& operator<<(Value&& value) {
    stream_ << std::forward<Value>(value);
    return *this;
  }

 private:
  /** Module configuration associated with this line. */
  ModuleConf module_;

  /** Buffer holding the message until the line is emitted. */
  std::ostringstream stream_;
};

/**
 * Starts a log line at a compile-time log level.
 *
 * LOGGER_MODULE() creates one variable-template instance of this class for each
 * log level used by a translation unit.
 *
 * @tparam Level Log level assigned to each line started by this object.
 */
template <LogLevel Level>
class LogStarter {
 public:
  /**
   * Construct a log starter for a module.
   *
   * @param[in] module Module configuration copied into the starter.
   */
  explicit LogStarter(ModuleConf module) : module_(std::move(module)) {}

  /**
   * Start a log line with its first value.
   *
   * @tparam Value Type of the first streamed value.
   * @param[in] value First value to append.
   * @return A temporary log line that emits at end of expression.
   */
  template <typename Value>
  LogLine<Level> operator<<(Value&& value) const {
    return LogLine<Level>(module_, std::forward<Value>(value));
  }

 private:
  /** Module configuration copied into each new log line. */
  ModuleConf module_;
};

}  // namespace puc::logger

/**
 * @def LOGGER_INIT
 * Initialize the process-wide logger.
 *
 * Call this macro from the program entry point after constructing a
 * puc::logger::LoggerConf.
 *
 * @param config Global logger configuration.
 */
#define LOGGER_INIT(config) ::puc::logger::init_logger((config))

/**
 * @def LOGGER_MODULE
 * Declare the logger module used by the current translation unit.
 *
 * Invoke this macro once at namespace scope in each implementation file that
 * logs messages. It declares the file-local Logger variable template and makes
 * the DEBUG, INFO, WARN, and ERROR enumerators available to it.
 *
 * @code{.cpp}
 * LOGGER_MODULE("Scheduler");
 *
 * void run() {
 *   Logger<INFO> << "Scheduler started";
 * }
 * @endcode
 *
 * @param module_name Name included in log lines from this translation unit.
 */
#define LOGGER_MODULE(module_name)                                             \
  namespace {                                                                  \
  using enum ::puc::logger::LogLevel;                                          \
  template <::puc::logger::LogLevel Level>                                     \
  const ::puc::logger::LogStarter<Level> Logger{                               \
      ::puc::logger::ModuleConf{(module_name)}};                               \
  }
