/**
 * @file socket_channel.cpp
 * @brief Unix-domain socket channel implementation.
 */

#include "utils/ipc/socket_channel.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "utils/ipc/framed_io.hpp"
#include "utils/logger/logger.hpp"
#include "utils/timer/poller.hpp"

/** @cond IPC_SOCKET_LOGGER_MODULE */
LOGGER_MODULE("IPC Socket Channel");
/** @endcond */

namespace puc::ipc {
namespace {

constexpr int kListenBacklog = 1;
constexpr std::chrono::milliseconds kAcceptPollInterval{50};

/** Close a valid descriptor and mark it invalid. */
void close_descriptor(int& descriptor) noexcept {
  if (descriptor >= 0) {
    static_cast<void>(::close(descriptor));
    descriptor = -1;
  }
}

}  // namespace

/** Unix socket descriptors, connection worker, and endpoint configuration. */
class SocketChannel::Impl {
 public:
  /** Retain immutable endpoint configuration. */
  Impl(SocketChannel& owner, std::filesystem::path configured_path,
       SocketRole configured_role, std::size_t configured_maximum)
      : owner_(owner),
        path_(std::move(configured_path)),
        role_(configured_role),
        maximum_message_bytes_(configured_maximum) {}

  Impl(const Impl&)            = delete;
  Impl& operator=(const Impl&) = delete;

  /** Ensure all resources are stopped if outer construction later fails. */
  ~Impl() { stop(); }

  /** Validate, open, and start the endpoint. */
  Status start() {
    if (maximum_message_bytes_ == 0U ||
        maximum_message_bytes_ > std::numeric_limits<std::uint32_t>::max()) {
      return Status::INVALID_ARGUMENT;
    }
    path_text_ = path_.string();
    if (path_text_.empty() || path_text_.find('\0') != std::string::npos) {
      Logger<ERROR> << "Invalid Unix socket path '" << path_text_ << "'";
      return Status::INVALID_TRANSPORT_PATH;
    }
    if (path_.is_relative()) {
      std::error_code path_error;
      path_ = std::filesystem::absolute(path_, path_error).lexically_normal();
      if (path_error) {
        Logger<ERROR> << "Could not resolve Unix socket path '" << path_text_
                      << "': " << path_error.message();
        return Status::INVALID_TRANSPORT_PATH;
      }
      path_text_ = path_.string();
    }
    if (path_text_.size() >= sizeof(sockaddr_un::sun_path)) {
      Logger<ERROR> << "Unix socket path is too long: '" << path_text_ << "'";
      return Status::INVALID_TRANSPORT_PATH;
    }
    const Status opened =
        role_ == SocketRole::SERVER ? open_server() : open_client();
    if (!is_ok(opened)) {
      stop();
      return opened;
    }
    reader_ = std::thread([this] {
      if (role_ == SocketRole::SERVER) {
        run_server();
      } else {
        run_peer();
      }
    });
    return Status::OK;
  }

  /** Stop the worker, then close and unlink owned resources. */
  void stop() noexcept {
    stopping_.store(true, std::memory_order_release);
    if (reader_.joinable()) {
      reader_.join();
    }
    {
      const std::lock_guard lock(peer_mutex_);
      close_descriptor(peer_descriptor_);
    }
    close_descriptor(listener_descriptor_);
    if (owns_path_) {
      if (::unlink(path_text_.c_str()) != 0 && errno != ENOENT) {
        Logger<WARN> << "Could not remove owned IPC socket '" << path_text_
                     << "' (errno " << errno << ')';
      }
      owns_path_ = false;
    }
  }

  /** Serialize writes and send one complete frame. */
  TransferResult transmit(Bytes data) noexcept {
    const std::lock_guard lock(peer_mutex_);
    const TransferResult result =
        detail::write_frame(peer_descriptor_, data, maximum_message_bytes_,
                            detail::StreamKind::SOCKET, stopping_);
    if (!is_ok(result.status) && result.status != Status::MESSAGE_TOO_LARGE &&
        peer_descriptor_ >= 0) {
      // A failed stream write may have emitted only part of a frame. Force the
      // reader to retire this connection so no subsequent frame can be parsed
      // against a corrupt boundary.
      static_cast<void>(::shutdown(peer_descriptor_, SHUT_RDWR));
    }
    return result;
  }

  /** Return whether a peer descriptor is currently installed. */
  bool connected() const noexcept {
    const std::lock_guard lock(peer_mutex_);
    return peer_descriptor_ >= 0;
  }

  /** Return the configured Unix socket path. */
  const std::filesystem::path& path() const noexcept { return path_; }

  /** Return whether this endpoint connects or listens. */
  SocketRole role() const noexcept { return role_; }

  /** Return the per-frame payload limit. */
  std::size_t maximum_message_bytes() const noexcept {
    return maximum_message_bytes_;
  }

 private:
  /** Populate a portable sockaddr_un for path_text_. */
  sockaddr_un address() const noexcept {
    sockaddr_un result{};
    result.sun_family = AF_UNIX;
    std::memcpy(result.sun_path, path_text_.c_str(), path_text_.size() + 1U);
    return result;
  }

  /** Bind an unused filesystem path and begin listening. */
  Status open_server() noexcept {
    struct stat path_status{};
    if (::lstat(path_text_.c_str(), &path_status) == 0) {
      Logger<ERROR> << "Refusing to replace existing Unix socket path '"
                    << path_text_ << "'";
      return Status::INVALID_TRANSPORT_PATH;
    }
    if (errno != ENOENT) {
      Logger<ERROR> << "Could not inspect Unix socket path '" << path_text_
                    << "' (errno " << errno << ')';
      return Status::IO_ERROR;
    }

    listener_descriptor_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener_descriptor_ < 0) {
      Logger<ERROR> << "socket() failed with errno " << errno;
      return Status::IO_ERROR;
    }
    Status result = detail::configure_socket_descriptor(listener_descriptor_);
    if (!is_ok(result)) {
      return result;
    }
    const sockaddr_un socket_address = address();
    const socklen_t address_bytes    = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + path_text_.size() + 1U);
    if (::bind(listener_descriptor_,
               reinterpret_cast<const sockaddr*>(&socket_address),
               address_bytes) != 0) {
      Logger<ERROR> << "bind() failed for '" << path_text_ << "' with errno "
                    << errno;
      return Status::IO_ERROR;
    }
    owns_path_ = true;
    if (::listen(listener_descriptor_, kListenBacklog) != 0) {
      Logger<ERROR> << "listen() failed for '" << path_text_ << "' with errno "
                    << errno;
      return Status::IO_ERROR;
    }
    return Status::OK;
  }

  /** Connect synchronously to an existing listener, then switch nonblocking. */
  Status open_client() noexcept {
    int descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (descriptor < 0) {
      Logger<ERROR> << "socket() failed with errno " << errno;
      return Status::IO_ERROR;
    }
    const sockaddr_un socket_address = address();
    const socklen_t address_bytes    = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + path_text_.size() + 1U);
    if (::connect(descriptor,
                  reinterpret_cast<const sockaddr*>(&socket_address),
                  address_bytes) != 0) {
      Logger<ERROR> << "connect() failed for '" << path_text_ << "' with errno "
                    << errno;
      static_cast<void>(::close(descriptor));
      return Status::NOT_CONNECTED;
    }
    const Status configured = detail::configure_socket_descriptor(descriptor);
    if (!is_ok(configured)) {
      static_cast<void>(::close(descriptor));
      return configured;
    }
    const std::lock_guard lock(peer_mutex_);
    peer_descriptor_ = descriptor;
    return Status::OK;
  }

  /** Accept and serve one connection at a time until stopping. */
  void run_server() noexcept {
    while (!stopping_.load(std::memory_order_acquire)) {
      const timer::PollResult readiness =
          timer::poll_readable(listener_descriptor_, kAcceptPollInterval);
      if (readiness.status == timer::Status::TIMED_OUT) {
        continue;
      }
      if (!timer::is_ok(readiness.status) || !readiness.readable) {
        if (stopping_.load(std::memory_order_acquire)) {
          return;
        }
        Logger<ERROR> << "Accept polling failed for '" << path_text_
                      << "': " << timer::status_message(readiness.status);
        owner_.set_status(Status::IO_ERROR);
        return;
      }
      int accepted = ::accept(listener_descriptor_, nullptr, nullptr);
      if (accepted < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
          continue;
        }
        Logger<ERROR> << "accept() failed with errno " << errno;
        continue;
      }
      const Status configured = detail::configure_socket_descriptor(accepted);
      if (!is_ok(configured)) {
        static_cast<void>(::close(accepted));
        continue;
      }
      {
        const std::lock_guard lock(peer_mutex_);
        peer_descriptor_ = accepted;
      }
      Logger<DEBUG> << "Accepted IPC connection on '" << path_text_ << "'";
      run_peer();
    }
  }

  /** Receive frames from the installed peer until it closes or fails. */
  void run_peer() noexcept {
    std::vector<std::uint8_t> message;
    while (!stopping_.load(std::memory_order_acquire)) {
      int descriptor = -1;
      {
        const std::lock_guard lock(peer_mutex_);
        descriptor = peer_descriptor_;
      }
      if (descriptor < 0) {
        return;
      }
      const Status result =
          detail::read_frame(descriptor, message, maximum_message_bytes_,
                             detail::StreamKind::SOCKET, stopping_);
      if (is_ok(result)) {
        owner_.deliver(message);
        continue;
      }
      if (result != Status::CHANNEL_UNAVAILABLE &&
          result != Status::END_OF_STREAM) {
        Logger<WARN> << "Socket reader for '" << path_text_
                     << "' stopped: " << status_message(result);
      }
      break;
    }
    const std::lock_guard lock(peer_mutex_);
    close_descriptor(peer_descriptor_);
    if (role_ == SocketRole::CLIENT &&
        !stopping_.load(std::memory_order_acquire)) {
      owner_.set_status(Status::NOT_CONNECTED);
    }
  }

  SocketChannel& owner_;              /**< Channel receiving decoded frames. */
  std::filesystem::path path_;        /**< Configured socket filesystem path. */
  SocketRole role_;                   /**< Connect or listen behavior. */
  std::size_t maximum_message_bytes_; /**< Per-frame payload limit. */
  std::string path_text_;             /**< Native path passed to POSIX calls. */
  std::atomic<bool> stopping_ = false; /**< Reader shutdown request. */
  mutable std::mutex peer_mutex_; /**< Serializes peer changes and writes. */
  int listener_descriptor_ = -1;  /**< Server listener, otherwise invalid. */
  int peer_descriptor_     = -1;  /**< Current bidirectional connection. */
  bool owns_path_ = false; /**< Whether destruction should unlink path_. */
  std::thread reader_;     /**< Accept/read worker. */
};

SocketChannel::SocketChannel(std::string name,
                             std::filesystem::path socket_path, SocketRole role,
                             std::size_t maximum_message_bytes,
                             ChannelOptions options)
    : Channel(std::move(name), std::move(options)),
      impl_(std::make_unique<Impl>(*this, std::move(socket_path), role,
                                   maximum_message_bytes)) {
  if (is_ok(status())) {
    set_status(impl_->start());
  }
}

SocketChannel::~SocketChannel() = default;

TransferResult SocketChannel::transmit(Bytes data) noexcept {
  const Status current_status = status();
  return is_ok(current_status) ? impl_->transmit(data)
                               : TransferResult{.status = current_status};
}

bool SocketChannel::connected() const noexcept { return impl_->connected(); }

SocketRole SocketChannel::role() const noexcept { return impl_->role(); }

const std::filesystem::path& SocketChannel::socket_path() const noexcept {
  return impl_->path();
}

std::size_t SocketChannel::maximum_message_bytes() const noexcept {
  return impl_->maximum_message_bytes();
}

}  // namespace puc::ipc
