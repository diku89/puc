/**
 * @file codec.cpp
 * @brief Payload codec registration and IPC-envelope dispatch.
 */

#include "msgs/codec.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <utility>

#include "msgs/null_message.hpp"
#include "utils/ipc/msg.hpp"
#include "utils/logger/logger.hpp"

/** @cond MESSAGE_CODEC_LOGGER_MODULE */
LOGGER_MODULE("Message Codec");
/** @endcond */

namespace puc::msg {

MessageCodecCollection::MessageCodecCollection(
    std::size_t maximum_payload_bytes)
    : maximum_payload_bytes_(maximum_payload_bytes) {
  codecs_.emplace(MessageId::NULL_MESSAGE,
                  std::make_unique<NullMessageCodec>());
}

Status MessageCodecCollection::register_codec(
    std::unique_ptr<CodecBase> codec) {
  if (codec == nullptr) {
    Logger<ERROR> << "Cannot register a null payload codec";
    return Status::INVALID_ARGUMENT;
  }

  const MessageId message_id = codec->message_id();
  const std::unique_lock lock(mutex_);
  if (codecs_.contains(message_id)) {
    Logger<WARN> << "Payload codec id " << to_wire_id(message_id)
                 << " is already registered";
    return Status::DUPLICATE_MESSAGE_ID;
  }
  codecs_.emplace(message_id, std::move(codec));
  Logger<DEBUG> << "Registered payload codec id " << to_wire_id(message_id);
  return Status::OK;
}

std::size_t MessageCodecCollection::size() const {
  const std::shared_lock lock(mutex_);
  return codecs_.size();
}

Status MessageCodecCollection::decode_payload_to_json(
    MessageId message_id, std::span<const std::uint8_t> payload,
    std::string& output) const {
  output.clear();
  const CodecBase* codec = find_codec(message_id);
  if (codec == nullptr) {
    Logger<WARN> << "No payload codec is registered for id "
                 << to_wire_id(message_id);
    return Status::MESSAGE_ID_NOT_FOUND;
  }
  const Status status = codec->decode_to_json(payload, output);
  if (!is_ok(status)) {
    output.clear();
    Logger<WARN> << "Payload codec " << to_wire_id(message_id)
                 << " could not decode its payload: " << status_message(status);
  }
  return status;
}

Status MessageCodecCollection::decode_to_json(
    std::span<const std::uint8_t>& data, std::string& output) const {
  output.clear();
  ipc::DecodedMessage message;
  std::size_t consumed_bytes    = 0U;
  const ipc::Status wire_status = ipc::deserialize_message(
      data, maximum_payload_bytes_, message, consumed_bytes);
  if (!ipc::is_ok(wire_status)) {
    if (wire_status == ipc::Status::TRUNCATED_MESSAGE) {
      Logger<DEBUG> << "IPC envelope needs more input bytes";
      return Status::INCOMPLETE_IPC_MESSAGE;
    }
    Logger<WARN> << "Could not decode an IPC envelope: "
                 << ipc::status_message(wire_status);
    return Status::INVALID_IPC_MESSAGE;
  }

  const Status status = decode_payload_to_json(
      from_wire_id(message.header.message_id), message.payload, output);
  if (!is_ok(status)) {
    return status;
  }
  data = data.subspan(consumed_bytes);
  return Status::OK;
}

const CodecBase* MessageCodecCollection::find_codec(
    MessageId message_id) const {
  const std::shared_lock lock(mutex_);
  const auto codec = codecs_.find(message_id);
  return codec == codecs_.end() ? nullptr : codec->second.get();
}

}  // namespace puc::msg
