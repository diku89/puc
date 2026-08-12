#pragma once

/**
 * @file codec.hpp
 * @brief Typed payload codecs and message-id-based codec dispatch.
 */

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <shared_mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "msgs/status.hpp"
#include "utils/ipc/msg.hpp"

namespace puc::msg {

/** Globally registered payload schema identifiers. */
enum class MessageId : std::uint32_t {
  /**
   * Empty payload represented as an empty JSON object.
   *
   * \msg_id{0||puc::msg::NullMessage||Reserves the zero schema identifier for
   * an explicitly empty typed payload.||Components that need a typed no-data
   * envelope.||Generic dispatch and components expecting an explicit no-data
   * payload.}
   */
  NULL_MESSAGE = 0U,

  /**
   * One-way command from Screen to TerminalSession.
   *
   * \msg_id{1||puc::msg::ScreenCommand||Selects the portable schema for
   * terminal ownership, presentation, and release commands.||
   * \ref puc::tui::Screen "Screen".||
   * \ref puc::terminal::TerminalSession "TerminalSession".}
   */
  SCREEN_COMMAND = 1U,

  /**
   * Observed terminal geometry publication.
   *
   * \msg_id{2||puc::msg::ScreenResizeEvent||Selects the fixed-width schema for
   * the newest observed terminal geometry.||
   * \ref puc::terminal::TerminalSession "TerminalSession".||
   * \ref puc::tui::Screen "Screen" and subscribed geometry observers.}
   */
  SCREEN_RESIZE_EVENT = 2U,

  /**
   * Command-mode notification text.
   *
   * \msg_id{3||puc::msg::CmdFrameNotification||Selects the UTF-8 command
   * notification schema published on //cmdframe/notify.||
   * \ref puc::command::send_notification "Command notification producers".||
   * The command-mode controller presenting \ref puc::tui::CmdFrame
   * "CmdFrame".}
   */
  CMD_FRAME_NOTIFICATION = 3U,

  /**
   * Normalized terminal input event.
   *
   * \msg_id{4||puc::msg::TerminalInputEvent||Selects the portable tagged
   * terminal-event schema published on //terminal/input_events.||The
   * lifecycle-owned terminal input producer.||The TUI Screen transport
   * consumer.}
   */
  TERMINAL_INPUT_EVENT = 4U,
};

/** Convert a payload schema identifier to its IPC wire representation. */
constexpr std::uint32_t to_wire_id(MessageId message_id) noexcept {
  return static_cast<std::uint32_t>(message_id);
}

/** Convert an IPC wire identifier to a payload schema identifier. */
constexpr MessageId from_wire_id(std::uint32_t message_id) noexcept {
  return static_cast<MessageId>(message_id);
}

/**
 * Value contract accepted by Codec.
 *
 * Message values are ordinary regular C++ values. Each value type supplies a
 * `std::formatter<T, char>` specialization whose default format is valid JSON.
 * This keeps JSON next to the struct that defines the schema and lets the
 * generic codec use `std::format("{}", value)` without a JSON dependency.
 */
template <typename T>
concept MessageValue = std::regular<T> && std::formattable<T, char>;

/** Type-erased interface stored by MessageCodecCollection. */
class CodecBase {
 public:
  /** Destroy a codec through the type-erased interface. */
  virtual ~CodecBase() = default;

  /** Return the payload schema identifier implemented by this codec. */
  MessageId message_id() const noexcept { return message_id_; }

  /**
   * Decode a payload and format its typed value as JSON.
   *
   * @param[in] payload Encoded payload bytes, excluding the IPC envelope.
   * @param[out] output JSON text, cleared before decoding and on failure.
   * @return Status::OK or the concrete payload decoder's failure status.
   */
  virtual Status decode_to_json(std::span<const std::uint8_t> payload,
                                std::string& output) const = 0;

 protected:
  /** Construct a codec for one immutable payload schema identifier. */
  explicit constexpr CodecBase(MessageId message_id) noexcept
      : message_id_(message_id) {}

 private:
  MessageId message_id_; /**< Payload schema selected by this codec. */
};

/**
 * Typed, status-returning payload codec facade.
 *
 * Derived codecs implement only `encode_payload()` and `decode_payload()`.
 * The facade owns output cleanup and provides JSON through the message value's
 * `std::formatter` specialization. Payload bytes must be portable; native C++
 * object layouts must not be copied into an encoded payload.
 *
 * @tparam T Regular message value whose default format is valid JSON.
 */
template <MessageValue T>
class Codec : public CodecBase {
 public:
  /** Type encoded and decoded by this codec. */
  using value_type = T;

  /** Destroy a typed codec through either typed or erased ownership. */
  ~Codec() override = default;

  /**
   * Encode a typed value into owned payload bytes.
   *
   * Output is cleared before encoding and remains empty on failure.
   */
  Status serialize(const T& object, std::vector<std::uint8_t>& output) const {
    output.clear();
    std::vector<std::uint8_t> encoded;
    const Status status = encode_payload(object, encoded);
    if (!is_ok(status)) {
      return status;
    }
    output = std::move(encoded);
    return Status::OK;
  }

  /**
   * Decode complete payload bytes into a typed value.
   *
   * Output is reset before decoding and remains default-initialized on
   * failure. A codec must reject trailing bytes that are not part of its
   * schema.
   */
  Status deserialize(std::span<const std::uint8_t> payload, T& output) const {
    output = T{};
    T decoded{};
    const Status status = decode_payload(payload, decoded);
    if (!is_ok(status)) {
      return status;
    }
    output = std::move(decoded);
    return Status::OK;
  }

  /** Format a typed value as JSON through its `std::formatter` specialization.
   */
  std::string to_json(const T& object) const {
    return std::format("{}", object);
  }

  /** Implement type-erased payload decoding followed by struct formatting. */
  Status decode_to_json(std::span<const std::uint8_t> payload,
                        std::string& output) const final {
    output.clear();
    T decoded{};
    const Status status = deserialize(payload, decoded);
    if (!is_ok(status)) {
      return status;
    }
    output = to_json(decoded);
    return Status::OK;
  }

 protected:
  /** Construct a typed codec for one payload schema identifier. */
  explicit constexpr Codec(MessageId message_id) noexcept
      : CodecBase(message_id) {}

  /** Encode one typed value into a temporary owned payload. */
  virtual Status encode_payload(const T& object,
                                std::vector<std::uint8_t>& output) const = 0;

  /** Decode all supplied payload bytes into a temporary typed value. */
  virtual Status decode_payload(std::span<const std::uint8_t> payload,
                                T& output) const = 0;
};

/**
 * Setup-time registry of typed payload codecs.
 *
 * The default collection contains NullMessageCodec. Additional codecs may be
 * registered by message id before the collection is shared with consumers.
 * Registration and const lookup are synchronized; registered codecs are never
 * removed, so concurrent readers retain stable codec addresses.
 */
class MessageCodecCollection {
 public:
  /** Construct a collection containing the built-in null-message codec. */
  MessageCodecCollection();

  /** Destroy every registered codec. */
  ~MessageCodecCollection() = default;

  /** Collections have unique codec ownership and cannot be copied. */
  MessageCodecCollection(const MessageCodecCollection&) = delete;

  /** Collections have unique codec ownership and cannot be copy-assigned. */
  MessageCodecCollection& operator=(const MessageCodecCollection&) = delete;

  /** A collection contains a mutex and cannot be moved. */
  MessageCodecCollection(MessageCodecCollection&&) = delete;

  /** A collection contains a mutex and cannot be move-assigned. */
  MessageCodecCollection& operator=(MessageCodecCollection&&) = delete;

  /**
   * Add an owning codec without replacing an existing message id.
   *
   * @return Status::OK, Status::INVALID_ARGUMENT for a null codec, or
   *         Status::DUPLICATE_MESSAGE_ID when the id is already registered.
   */
  Status register_codec(std::unique_ptr<CodecBase> codec);

  /** Return the number of registered payload schema identifiers. */
  std::size_t size() const;

  /**
   * Serialize a typed payload with its registered codec.
   *
   * Output is empty on every failure, including a missing id or type mismatch.
   */
  template <MessageValue T>
  Status serialize(MessageId message_id, const T& object,
                   std::vector<std::uint8_t>& output) const {
    output.clear();
    const Codec<T>* codec = nullptr;
    const Status status   = get_codec(message_id, codec);
    return is_ok(status) ? codec->serialize(object, output) : status;
  }

  /**
   * Deserialize a typed payload with its registered codec.
   *
   * Output is default-initialized on every failure.
   */
  template <MessageValue T>
  Status deserialize(MessageId message_id,
                     std::span<const std::uint8_t> payload, T& output) const {
    output                = T{};
    const Codec<T>* codec = nullptr;
    const Status status   = get_codec(message_id, codec);
    return is_ok(status) ? codec->deserialize(payload, output) : status;
  }

  /**
   * Decode payload bytes by id and format the resulting struct as JSON.
   *
   * Output is cleared before lookup and remains empty on failure.
   */
  Status decode_payload_to_json(MessageId message_id,
                                std::span<const std::uint8_t> payload,
                                std::string& output) const;

  /**
   * Decode the first complete IPC message and format its payload as JSON.
   *
   * The IPC header's message id selects the payload codec. On success, `data`
   * advances by exactly one complete IPC message, permitting repeated decoding
   * of a concatenated stream. On failure, `data` is unchanged and output is
   * empty.
   */
  Status decode_to_json(std::span<const std::uint8_t>& data,
                        std::string& output) const;

 private:
  /** Find a type-erased codec while protecting the registry index. */
  const CodecBase* find_codec(MessageId message_id) const;

  /** Resolve an id and verify that its codec owns exactly T. */
  template <MessageValue T>
  Status get_codec(MessageId message_id, const Codec<T>*& output) const {
    output                  = nullptr;
    const CodecBase* erased = find_codec(message_id);
    if (erased == nullptr) {
      return Status::MESSAGE_ID_NOT_FOUND;
    }
    output = dynamic_cast<const Codec<T>*>(erased);
    return output == nullptr ? Status::CODEC_TYPE_MISMATCH : Status::OK;
  }

  mutable std::shared_mutex mutex_; /**< Synchronizes registry access. */
  std::unordered_map<MessageId, std::unique_ptr<CodecBase>>
      codecs_; /**< Message-id index and codec ownership. */
};

}  // namespace puc::msg
