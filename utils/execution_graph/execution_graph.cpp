/**
 * @file execution_graph.cpp
 * @brief Non-template execution-graph diagnostics.
 */

#include "utils/execution_graph/execution_graph.hpp"

#include <cstddef>
#include <string_view>

#include "utils/logger/logger.hpp"

/** @cond EXECUTION_GRAPH_LOGGER_MODULE */
LOGGER_MODULE("Execution Graph");
/** @endcond */

namespace puc::execution_graph::detail {

void log_failure(std::string_view operation, Status status) noexcept {
  Logger<ERROR> << "Execution graph could not " << operation << ": "
                << status_message(status);
}

void log_transition(std::string_view operation,
                    std::size_t node_count) noexcept {
  Logger<DEBUG> << "Execution graph " << operation << " with " << node_count
                << " nodes";
}

}  // namespace puc::execution_graph::detail
