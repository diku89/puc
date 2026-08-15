/**
 * @file filebuffer_channel.cpp
 * @brief POSIX named-pipe channel implementation.
 */

#include "utils/ipc/filebuffer_channel.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "utils/ipc/framed_io.hpp"
#include "utils/logger/logger.hpp"

/** @cond IPC_FILE_BUFFER_LOGGER_MODULE */
LOGGER_MODULE("IPC File Buffer Channel");
/** @endcond */

namespace puc::ipc {
namespace {

/** Close a valid descriptor and mark it invalid. */
void close_fifo_descriptor(int& descriptor) noexcept {
  if (descriptor >= 0) {
    static_cast<void>(::close(descriptor));
    descriptor = -1;
  }
}

/** Open and verify one FIFO without trusting a racy pathname stat. */
Status open_fifo(const std::filesystem::path& path, int& descriptor,
                 struct stat& metadata) noexcept {
  const std::string native_path = path.string();
  if (native_path.empty() || native_path.find('\0') != std::string::npos) {
    return Status::INVALID_TRANSPORT_PATH;
  }
  // O_RDWR makes construction independent of which peer starts first.
  descriptor = ::open(native_path.c_str(), O_RDWR | O_NONBLOCK);
  if (descriptor < 0) {
    Logger<ERROR> << "Could not open IPC FIFO '" << native_path << "' (errno "
                  << errno << ')';
    return Status::INVALID_TRANSPORT_PATH;
  }
  if (::fstat(descriptor, &metadata) != 0) {
    Logger<ERROR> << "Could not inspect opened IPC FIFO '" << native_path
                  << "' (errno " << errno << ')';
    close_fifo_descriptor(descriptor);
    return Status::IO_ERROR;
  }
  if (!S_ISFIFO(metadata.st_mode)) {
    Logger<ERROR> << "IPC path '" << native_path << "' is not a FIFO";
    close_fifo_descriptor(descriptor);
    return Status::INVALID_TRANSPORT_PATH;
  }
  const Status configured = detail::configure_stream_descriptor(descriptor);
  if (!is_ok(configured)) {
    close_fifo_descriptor(descriptor);
  }
  return configured;
}

}  // namespace

/** FIFO descriptors, worker thread, and immutable endpoint configuration. */
class FileBufferChannel::Impl {
 public:
  /** Retain immutable FIFO endpoint configuration. */
  Impl(FileBufferChannel& owner, std::filesystem::path configured_read_path,
       std::filesystem::path configured_write_path,
       std::size_t configured_maximum)
      : owner_(owner),
        read_path_(std::move(configured_read_path)),
        write_path_(std::move(configured_write_path)),
        maximum_message_bytes_(configured_maximum) {}

  Impl(const Impl&)            = delete;
  Impl& operator=(const Impl&) = delete;

  /** Close resources if outer construction or normal ownership ends. */
  ~Impl() { stop(); }

  /** Validate paths, open both FIFO descriptors, and start reading. */
  Status start() {
    if (maximum_message_bytes_ == 0U ||
        maximum_message_bytes_ > std::numeric_limits<std::uint32_t>::max()) {
      return Status::INVALID_ARGUMENT;
    }
    if (read_path_ == write_path_) {
      Logger<ERROR> << "An IPC FIFO endpoint requires distinct read and write "
                       "paths";
      return Status::INVALID_TRANSPORT_PATH;
    }
    struct stat read_metadata{};
    struct stat write_metadata{};
    Status result = open_fifo(read_path_, read_descriptor_, read_metadata);
    if (is_ok(result)) {
      result = open_fifo(write_path_, write_descriptor_, write_metadata);
    }
    if (is_ok(result) && read_metadata.st_dev == write_metadata.st_dev &&
        read_metadata.st_ino == write_metadata.st_ino) {
      Logger<ERROR> << "An IPC FIFO endpoint's paths resolve to the same FIFO";
      result = Status::INVALID_TRANSPORT_PATH;
    }
    if (!is_ok(result)) {
      stop();
      return result;
    }
    reader_ = std::thread([this] { run_reader(); });
    return Status::OK;
  }

  /** Ask the reader to stop, join it, then close both caller-owned FIFOs. */
  void stop() noexcept {
    stopping_.store(true, std::memory_order_release);
    if (reader_.joinable()) {
      reader_.join();
    }
    close_fifo_descriptor(read_descriptor_);
    const std::lock_guard lock(write_mutex_);
    close_fifo_descriptor(write_descriptor_);
  }

  /** Serialize writes so frame prefixes and bodies cannot interleave. */
  TransferResult transmit(Bytes data) noexcept {
    const std::lock_guard lock(write_mutex_);
    const TransferResult result =
        detail::write_frame(write_descriptor_, data, maximum_message_bytes_,
                            detail::StreamKind::FILE_DESCRIPTOR, stopping_);
    if (!is_ok(result.status) && result.status != Status::MESSAGE_TOO_LARGE &&
        result.status != Status::CHANNEL_UNAVAILABLE) {
      // A FIFO cannot be reconnected to discard a partially written frame.
      // Mark the endpoint unusable instead of appending future frames to a
      // potentially corrupt stream boundary.
      owner_.set_status(result.status);
    }
    return result;
  }

  /** Return the incoming FIFO path. */
  const std::filesystem::path& read_path() const noexcept { return read_path_; }

  /** Return the outgoing FIFO path. */
  const std::filesystem::path& write_path() const noexcept {
    return write_path_;
  }

  /** Return the per-frame payload limit. */
  std::size_t maximum_message_bytes() const noexcept {
    return maximum_message_bytes_;
  }

 private:
  /** Repeatedly read complete FIFO frames and distribute them to subscribers.
   */
  void run_reader() noexcept {
    std::vector<std::uint8_t> message;
    while (!stopping_.load(std::memory_order_acquire)) {
      const Status result =
          detail::read_frame(read_descriptor_, message, maximum_message_bytes_,
                             detail::StreamKind::FILE_DESCRIPTOR, stopping_);
      if (is_ok(result)) {
        owner_.deliver(message);
        continue;
      }
      if (result == Status::CHANNEL_UNAVAILABLE) {
        return;
      }
      Logger<ERROR> << "FIFO reader for '" << read_path_.string()
                    << "' stopped: " << status_message(result);
      owner_.set_status(result);
      return;
    }
  }

  FileBufferChannel& owner_;           /**< Channel receiving decoded frames. */
  std::filesystem::path read_path_;    /**< Incoming caller-owned FIFO. */
  std::filesystem::path write_path_;   /**< Outgoing caller-owned FIFO. */
  std::size_t maximum_message_bytes_;  /**< Per-frame payload limit. */
  std::atomic<bool> stopping_ = false; /**< Reader shutdown request. */
  int read_descriptor_  = -1; /**< Nonblocking incoming FIFO descriptor. */
  int write_descriptor_ = -1; /**< Nonblocking outgoing FIFO descriptor. */
  std::mutex write_mutex_;    /**< Serializes frames and descriptor closing. */
  std::thread reader_;        /**< Incoming frame worker. */
};

FileBufferChannel::FileBufferChannel(std::string name,
                                     std::filesystem::path read_path,
                                     std::filesystem::path write_path,
                                     std::size_t maximum_message_bytes,
                                     ChannelOptions options)
    : Channel(std::move(name), std::move(options)),
      impl_(std::make_unique<Impl>(*this, std::move(read_path),
                                   std::move(write_path),
                                   maximum_message_bytes)) {
  if (is_ok(status())) {
    set_status(impl_->start());
  }
}

FileBufferChannel::~FileBufferChannel() = default;

TransferResult FileBufferChannel::transmit(Bytes data) noexcept {
  const Status current_status = status();
  return is_ok(current_status) ? impl_->transmit(data)
                               : TransferResult{.status = current_status};
}

const std::filesystem::path& FileBufferChannel::read_path() const noexcept {
  return impl_->read_path();
}

const std::filesystem::path& FileBufferChannel::write_path() const noexcept {
  return impl_->write_path();
}

std::size_t FileBufferChannel::maximum_message_bytes() const noexcept {
  return impl_->maximum_message_bytes();
}

}  // namespace puc::ipc
