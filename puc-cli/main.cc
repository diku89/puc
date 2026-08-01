#include "utils/logger/logger.hpp"

LOGGER_MODULE("Launcher");

int main(int argc, char** argv) {
  const puc::logger::LoggerConf config{
      .global_level = puc::logger::LogLevel::INFO,
      .logfile      = "puc.log",
  };

  LOGGER_INIT(config);

  Logger<INFO> << "Hello world.";
  return 0;
}
