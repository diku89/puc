#include "utils/logger/logger.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <regex>
#include <sstream>
#include <string>

LOGGER_MODULE("LoggerTest");

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
  }
}

class CaptureStream {
 public:
  explicit CaptureStream(std::ostream& stream)
      : stream_(stream), old_buffer_(stream.rdbuf(buffer_.rdbuf())) {}

  CaptureStream(const CaptureStream&)            = delete;
  CaptureStream& operator=(const CaptureStream&) = delete;

  ~CaptureStream() { stream_.rdbuf(old_buffer_); }

  std::string str() const { return buffer_.str(); }

 private:
  std::ostream& stream_;
  std::ostringstream buffer_;
  std::streambuf* old_buffer_;
};

void TestStdoutAndFormatting() {
  LOGGER_INIT((puc::logger::LoggerConf{
      .global_level = puc::logger::LogLevel::DEBUG,
      .logfile      = std::nullopt,
  }));

  CaptureStream stdout_capture(std::cout);
  Logger<INFO> << "Hello " << 42;

  const std::regex expected(
      "^\\x1b\\[32m[0-9]{4}-[0-9]{2}-[0-9]{2} "
      "[0-9]{2}:[0-9]{2}:[0-9]{2}:[0-9]{3} "
      "LoggerTest Hello 42\\x1b\\[0m\\n$");
  Expect(std::regex_match(stdout_capture.str(), expected),
         "INFO should be green, timestamped, and written to stdout");
}

void TestErrorUsesStderr() {
  LOGGER_INIT((puc::logger::LoggerConf{
      .global_level = puc::logger::LogLevel::DEBUG,
      .logfile      = std::nullopt,
  }));

  CaptureStream stdout_capture(std::cout);
  CaptureStream stderr_capture(std::cerr);
  Logger<ERROR> << "Broken";

  Expect(stdout_capture.str().empty(), "ERROR should not use stdout");
  Expect(stderr_capture.str().find("\x1b[31m") == 0,
         "ERROR should be red and written to stderr");
  Expect(stderr_capture.str().find("LoggerTest Broken") != std::string::npos,
         "ERROR should include its module and message");
}

void TestGlobalLevelFiltersMessages() {
  LOGGER_INIT((puc::logger::LoggerConf{
      .global_level = puc::logger::LogLevel::WARN,
      .logfile      = std::nullopt,
  }));

  CaptureStream stdout_capture(std::cout);
  Logger<DEBUG> << "hidden";
  Logger<INFO> << "also hidden";
  Logger<WARN> << "visible";

  Expect(stdout_capture.str().find("hidden") == std::string::npos,
         "messages below the global level should be filtered");
  Expect(stdout_capture.str().find("LoggerTest visible") != std::string::npos,
         "messages at the global level should be emitted");
}

void TestLogfileIsUncolored() {
  const char* test_tmpdir = std::getenv("TEST_TMPDIR");
  Expect(test_tmpdir != nullptr, "Bazel should provide TEST_TMPDIR");
  if (test_tmpdir == nullptr) {
    return;
  }

  const std::filesystem::path logfile =
      std::filesystem::path(test_tmpdir) / "logger_test.log";
  std::filesystem::remove(logfile);
  LOGGER_INIT((puc::logger::LoggerConf{
      .global_level = puc::logger::LogLevel::DEBUG,
      .logfile      = logfile,
  }));

  {
    CaptureStream stdout_capture(std::cout);
    Logger<DEBUG> << "File message";
  }

  std::ifstream input(logfile);
  const std::string contents((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
  Expect(contents.find("LoggerTest File message\n") != std::string::npos,
         "the logfile should receive the message");
  Expect(contents.find("\x1b[") == std::string::npos,
         "the logfile should not contain terminal colors");
}

}  // namespace

int main() {
  TestStdoutAndFormatting();
  TestErrorUsesStderr();
  TestGlobalLevelFiltersMessages();
  TestLogfileIsUncolored();
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
