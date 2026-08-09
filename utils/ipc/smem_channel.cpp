/**
 * @file smem_channel.cpp
 * @brief In-process IPC channel with selectable direct or bounded delivery.
 */

#include "utils/ipc/smem_channel.hpp"

#include <utility>

#include "utils/logger/logger.hpp"

/** @cond IPC_SMEM_LOGGER_MODULE */
LOGGER_MODULE("IPC Shared Memory Channel");
/** @endcond */

namespace puc::ipc {

SmemChannel::SmemChannel(std::string name, std::size_t maximum_message_bytes,
                         ChannelOptions options)
    : Channel(std::move(name), std::move(options)),
      maximum_message_bytes_(maximum_message_bytes) {
  if (is_ok(status()) && maximum_message_bytes_ == 0U) {
    set_status(Status::INVALID_ARGUMENT);
    Logger<ERROR> << "A shared-memory channel requires a positive message "
                     "limit";
  }
}

SmemChannel::~SmemChannel() = default;

TransferResult SmemChannel::transmit(Bytes data) noexcept {
  const Status current_status = status();
  if (!is_ok(current_status)) {
    return TransferResult{.status = current_status};
  }
  if (data.size() > maximum_message_bytes_) {
    Logger<WARN> << "Rejected " << data.size() << " byte message on " << name()
                 << " because its limit is " << maximum_message_bytes_;
    return TransferResult{.status = Status::MESSAGE_TOO_LARGE};
  }
  const Status delivery_status = deliver(data);
  return is_ok(delivery_status)
             ? TransferResult{.status = Status::OK, .bytes = data.size()}
             : TransferResult{.status = delivery_status};
}

}  // namespace puc::ipc
