/**
 * @file session_test.cpp
 * @brief Pseudo-terminal tests for POSIX session lifecycle and transport.
 */

#include "puc-cli/tui/terminal/session.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) ||      \
    defined(__NetBSD__) || defined(__DragonFly__)
#include <util.h>
#else
#include <pty.h>
#endif

#include "gtest/gtest.h"
#include "puc-cli/tui/terminal/clipboard.hpp"

namespace puc::terminal {
namespace {

/** Build the runtime-configured decoder used by transport tests. */
Decoder configured_decoder() {
  Decoder decoder;
  std::error_code error;
  const std::filesystem::path root = std::filesystem::current_path(error);
  if (error) {
    ADD_FAILURE() << "Could not resolve the test runfiles directory";
    return decoder;
  }
  properties::Properties configurations{root, root / "missing_user_overrides"};
  EXPECT_EQ(decoder.setup(configurations, "xterm-256color"), Status::OK);
  return decoder;
}

/** RAII pseudo-terminal pair for exercising real kernel terminal behavior. */
class PseudoTerminal {
 public:
  PseudoTerminal() {
    if (::openpty(&master_fd_, &slave_fd_, nullptr, nullptr, nullptr) != 0) {
      master_fd_ = -1;
      slave_fd_  = -1;
      return;
    }
    const int flags = ::fcntl(master_fd_, F_GETFL, 0);
    if (flags >= 0) {
      static_cast<void>(::fcntl(master_fd_, F_SETFL, flags | O_NONBLOCK));
    }
  }

  PseudoTerminal(const PseudoTerminal&)            = delete;
  PseudoTerminal& operator=(const PseudoTerminal&) = delete;

  ~PseudoTerminal() {
    if (master_fd_ >= 0) {
      static_cast<void>(::close(master_fd_));
    }
    if (slave_fd_ >= 0) {
      static_cast<void>(::close(slave_fd_));
    }
  }

  bool valid() const noexcept { return master_fd_ >= 0 && slave_fd_ >= 0; }
  int slave_fd() const noexcept { return slave_fd_; }

  bool set_slave_nonblocking() const noexcept {
    const int flags = ::fcntl(slave_fd_, F_GETFL, 0);
    return flags >= 0 && ::fcntl(slave_fd_, F_SETFL, flags | O_NONBLOCK) == 0;
  }

  int open_slave(int flags) const noexcept {
    const char* path = ::ttyname(slave_fd_);
    return path == nullptr ? -1 : ::open(path, flags | O_NOCTTY);
  }

  bool set_size(std::size_t width, std::size_t height) const noexcept {
    if (width > std::numeric_limits<unsigned short>::max() ||
        height > std::numeric_limits<unsigned short>::max()) {
      return false;
    }
    const struct winsize size{
        .ws_row    = static_cast<unsigned short>(height),
        .ws_col    = static_cast<unsigned short>(width),
        .ws_xpixel = 0,
        .ws_ypixel = 0,
    };
    return ::ioctl(slave_fd_, TIOCSWINSZ, &size) == 0;
  }

  bool write_input(std::string_view input) const noexcept {
    std::size_t offset = 0;
    while (offset < input.size()) {
      const ssize_t count =
          ::write(master_fd_, input.data() + offset, input.size() - offset);
      if (count > 0) {
        offset += static_cast<std::size_t>(count);
      } else if (count < 0 && errno == EINTR) {
        continue;
      } else {
        return false;
      }
    }
    return true;
  }

  std::string read_output() const {
    std::string output;
    char bytes[512];
    while (true) {
      const ssize_t count = ::read(master_fd_, bytes, sizeof(bytes));
      if (count > 0) {
        output.append(bytes, static_cast<std::size_t>(count));
        continue;
      }
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        break;
      }
      break;
    }
    return output;
  }

 private:
  int master_fd_ = -1;
  int slave_fd_  = -1;
};

TEST(TerminalSessionTest, InactiveOperationsReturnNonThrowingStatuses) {
  TerminalSession session(-1, -1);
  Decoder decoder;
  std::vector<Event> events;
  std::size_t bytes_read = 99;
  bool end_of_input      = true;

  EXPECT_EQ(session.write("data"), Status::NOT_ACTIVE);
  EXPECT_EQ(session.set_clipboard(ClipboardSelection::CLIPBOARD, "data"),
            Status::NOT_ACTIVE);
  EXPECT_EQ(session.query_clipboard(ClipboardSelection::CLIPBOARD),
            Status::NOT_ACTIVE);
  EXPECT_EQ(session.query_keyboard_protocol(), Status::NOT_ACTIVE);
  EXPECT_EQ(session.read(decoder, events, bytes_read, end_of_input),
            Status::NOT_ACTIVE);
  EXPECT_EQ(bytes_read, 0U);
  EXPECT_FALSE(end_of_input);
  EXPECT_EQ(session.release(), Status::OK);
}

TEST(TerminalSessionTest, DefaultConstructedSessionIsSafelyInactive) {
  TerminalSession session;
  EXPECT_FALSE(session.active());
  EXPECT_EQ(session.release(), Status::OK);
}

TEST(TerminalSessionTest, RejectsNonTerminalDescriptors) {
  int descriptors[2];
  ASSERT_EQ(::pipe(descriptors), 0);
  {
    TerminalSession session(descriptors[0], descriptors[1]);
    EXPECT_EQ(session.take(), Status::TERMINAL_NOT_AVAILABLE);
    TerminalSize size{.width = 1, .height = 1};
    EXPECT_EQ(session.get_size(size), Status::TERMINAL_QUERY_FAILED);
    EXPECT_EQ(size, TerminalSize{});
  }
  EXPECT_EQ(::close(descriptors[0]), 0);
  EXPECT_EQ(::close(descriptors[1]), 0);
}

TEST(TerminalSessionTest, TakeActivatesAndReleaseReversesPucTerminalMode) {
  PseudoTerminal terminal;
  ASSERT_TRUE(terminal.valid());
  ASSERT_TRUE(terminal.set_size(80, 24));
  termios before{};
  ASSERT_EQ(::tcgetattr(terminal.slave_fd(), &before), 0);

  TerminalSession session(terminal.slave_fd(), terminal.slave_fd());
  ASSERT_EQ(session.take(), Status::OK);
  EXPECT_TRUE(session.active());

  termios raw{};
  ASSERT_EQ(::tcgetattr(terminal.slave_fd(), &raw), 0);
  EXPECT_EQ(raw.c_lflag & (ECHO | ICANON), 0U);
  EXPECT_EQ(raw.c_lflag & ISIG, before.c_lflag & ISIG);

  const std::string entered = terminal.read_output();
  EXPECT_NE(entered.find("\x1b[?1049h"), std::string::npos);
  EXPECT_NE(entered.find("\x1b[?25l"), std::string::npos);
  EXPECT_NE(entered.find("\x1b[?7l"), std::string::npos);
  EXPECT_NE(entered.find("\x1b[?2004h"), std::string::npos);
  EXPECT_NE(entered.find("\x1b[?1004h"), std::string::npos);
  EXPECT_NE(entered.find("\x1b[?1002h"), std::string::npos);
  EXPECT_NE(entered.find("\x1b[?1006h"), std::string::npos);
  EXPECT_NE(entered.find("\x1b[>29u"), std::string::npos);

  EXPECT_EQ(session.write("hello"), Status::OK);
  EXPECT_EQ(session.write(""), Status::OK);
  EXPECT_EQ(terminal.read_output(), "hello");

  EXPECT_EQ(session.take(), Status::ALREADY_ACTIVE);
  ASSERT_EQ(session.release(), Status::OK);
  EXPECT_FALSE(session.active());

  const std::string released = terminal.read_output();
  EXPECT_NE(released.find("\x1b[<u"), std::string::npos);
  EXPECT_NE(released.find("\x1b[?1006l"), std::string::npos);
  EXPECT_NE(released.find("\x1b[?1002l"), std::string::npos);
  EXPECT_NE(released.find("\x1b[?1004l"), std::string::npos);
  EXPECT_NE(released.find("\x1b[?2004l"), std::string::npos);
  EXPECT_NE(released.find("\x1b[?7h"), std::string::npos);
  EXPECT_NE(released.find("\x1b[?25h"), std::string::npos);
  EXPECT_NE(released.find("\x1b[?1049l"), std::string::npos);

  termios after{};
  ASSERT_EQ(::tcgetattr(terminal.slave_fd(), &after), 0);
  EXPECT_EQ(after.c_iflag, before.c_iflag);
  EXPECT_EQ(after.c_oflag, before.c_oflag);
  EXPECT_EQ(after.c_cflag, before.c_cflag);
  tcflag_t ignored_local_flags = 0;
#ifdef PENDIN
  ignored_local_flags |= PENDIN;
#endif
  EXPECT_EQ(after.c_lflag & ~ignored_local_flags,
            before.c_lflag & ~ignored_local_flags);
  EXPECT_EQ(session.release(), Status::OK);
}

TEST(TerminalSessionTest, QueriesCharacterCellDimensions) {
  PseudoTerminal terminal;
  ASSERT_TRUE(terminal.valid());
  ASSERT_TRUE(terminal.set_size(137, 27));
  TerminalSession session(terminal.slave_fd(), terminal.slave_fd());

  TerminalSize size;
  ASSERT_EQ(session.get_size(size), Status::OK);
  EXPECT_EQ(size, (TerminalSize{.width = 137, .height = 27}));

  ASSERT_TRUE(terminal.set_size(0, 0));
  EXPECT_EQ(session.get_size(size), Status::TERMINAL_QUERY_FAILED);
  EXPECT_EQ(size, TerminalSize{});
}

TEST(TerminalSessionTest, SizeQueryFallsBackToTheInputDescriptor) {
  PseudoTerminal terminal;
  ASSERT_TRUE(terminal.valid());
  ASSERT_TRUE(terminal.set_size(91, 37));
  TerminalSession session(terminal.slave_fd(), -1);

  TerminalSize size;
  ASSERT_EQ(session.get_size(size), Status::OK);
  EXPECT_EQ(size, (TerminalSize{.width = 91, .height = 37}));
}

TEST(TerminalSessionTest, ReadsBytesIntoTheStreamingDecoder) {
  PseudoTerminal terminal;
  ASSERT_TRUE(terminal.valid());
  ASSERT_TRUE(terminal.set_size(80, 24));
  TerminalSession session(terminal.slave_fd(), terminal.slave_fd());
  ASSERT_EQ(session.take(), Status::OK);
  ASSERT_TRUE(terminal.write_input("hi\x1b[A"));

  Decoder decoder = configured_decoder();
  std::vector<Event> events;
  std::size_t bytes_read = 0;
  bool end_of_input      = false;
  ASSERT_EQ(session.read(decoder, events, bytes_read, end_of_input),
            Status::OK);
  EXPECT_EQ(bytes_read, 5U);
  EXPECT_FALSE(end_of_input);
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(std::get<TextEvent>(events[0]).utf8, "hi");
  EXPECT_EQ(std::get<NamedKey>(std::get<KeyEvent>(events[1]).key.value),
            NamedKey::UP);
  EXPECT_EQ(session.release(), Status::OK);
}

TEST(TerminalSessionTest, NonblockingReadWithNoInputIsNotAnError) {
  PseudoTerminal terminal;
  ASSERT_TRUE(terminal.valid());
  ASSERT_TRUE(terminal.set_slave_nonblocking());
  TerminalSession session(terminal.slave_fd(), terminal.slave_fd());
  ASSERT_EQ(session.take(), Status::OK);

  Decoder decoder;
  std::vector<Event> events;
  std::size_t bytes_read = 99U;
  bool end_of_input      = true;
  EXPECT_EQ(session.read(decoder, events, bytes_read, end_of_input),
            Status::OK);
  EXPECT_EQ(bytes_read, 0U);
  EXPECT_FALSE(end_of_input);
  EXPECT_TRUE(events.empty());
  EXPECT_EQ(session.release(), Status::OK);
}

TEST(TerminalSessionTest, ReadFailureIsReturnedWithoutThrowing) {
  PseudoTerminal terminal;
  ASSERT_TRUE(terminal.valid());
  const int write_only_input = terminal.open_slave(O_WRONLY | O_NONBLOCK);
  ASSERT_GE(write_only_input, 0);
  TerminalSession session(write_only_input, terminal.slave_fd());
  ASSERT_EQ(session.take(), Status::OK);

  Decoder decoder;
  std::vector<Event> events;
  std::size_t bytes_read = 99U;
  bool end_of_input      = true;
  EXPECT_EQ(session.read(decoder, events, bytes_read, end_of_input),
            Status::TERMINAL_READ_FAILED);
  EXPECT_EQ(bytes_read, 0U);
  EXPECT_FALSE(end_of_input);
  EXPECT_EQ(session.release(), Status::OK);
  EXPECT_EQ(::close(write_only_input), 0);
}

TEST(TerminalSessionTest, WritesAndQueriesClipboardWithoutLoggingPayload) {
  PseudoTerminal terminal;
  ASSERT_TRUE(terminal.valid());
  ASSERT_TRUE(terminal.set_size(80, 24));
  TerminalSession session(terminal.slave_fd(), terminal.slave_fd());
  ASSERT_EQ(session.take(), Status::OK);
  static_cast<void>(terminal.read_output());
  ASSERT_EQ(session.set_clipboard(ClipboardSelection::CLIPBOARD, "secret"),
            Status::OK);
  ASSERT_EQ(session.query_clipboard(ClipboardSelection::PRIMARY), Status::OK);
  ASSERT_EQ(session.query_keyboard_protocol(), Status::OK);

  EXPECT_EQ(terminal.read_output(),
            "\x1b]52;c;c2VjcmV0\x1b\\\x1b]52;p;?\x1b\\\x1b[?u");
  EXPECT_EQ(session.release(), Status::OK);
}

TEST(TerminalSessionTest, ClipboardOutputLimitIsPropagated) {
  PseudoTerminal terminal;
  ASSERT_TRUE(terminal.valid());
  TerminalSession session(terminal.slave_fd(), terminal.slave_fd());
  ASSERT_EQ(session.take(), Status::OK);
  static_cast<void>(terminal.read_output());

  const std::string too_large(kDefaultMaximumClipboardBytes + 1U, 'x');
  EXPECT_EQ(session.set_clipboard(ClipboardSelection::CLIPBOARD, too_large),
            Status::OUTPUT_LIMIT_EXCEEDED);
  EXPECT_TRUE(terminal.read_output().empty());
  EXPECT_EQ(session.release(), Status::OK);
}

TEST(TerminalSessionTest, FailedSetupRollsBackRawTerminalState) {
  PseudoTerminal terminal;
  ASSERT_TRUE(terminal.valid());
  const int read_only_output = terminal.open_slave(O_RDONLY);
  ASSERT_GE(read_only_output, 0);
  termios before{};
  ASSERT_EQ(::tcgetattr(terminal.slave_fd(), &before), 0);

  TerminalSession session(terminal.slave_fd(), read_only_output);
  EXPECT_EQ(session.take(), Status::TERMINAL_WRITE_FAILED);
  EXPECT_FALSE(session.active());
  termios after{};
  ASSERT_EQ(::tcgetattr(terminal.slave_fd(), &after), 0);
  tcflag_t ignored_local_flags = 0;
#ifdef PENDIN
  ignored_local_flags |= PENDIN;
#endif
  EXPECT_EQ(after.c_lflag & ~ignored_local_flags,
            before.c_lflag & ~ignored_local_flags);
  EXPECT_EQ(::close(read_only_output), 0);
}

TEST(TerminalSessionTest, ReleaseRestoresTermiosEvenWhenOutputFails) {
  PseudoTerminal terminal;
  ASSERT_TRUE(terminal.valid());
  const int output = terminal.open_slave(O_WRONLY);
  ASSERT_GE(output, 0);
  termios before{};
  ASSERT_EQ(::tcgetattr(terminal.slave_fd(), &before), 0);

  TerminalSession session(terminal.slave_fd(), output);
  ASSERT_EQ(session.take(), Status::OK);
  EXPECT_EQ(::close(output), 0);
  EXPECT_EQ(session.release(), Status::TERMINAL_WRITE_FAILED);

  termios after{};
  ASSERT_EQ(::tcgetattr(terminal.slave_fd(), &after), 0);
  tcflag_t ignored_local_flags = 0;
#ifdef PENDIN
  ignored_local_flags |= PENDIN;
#endif
  EXPECT_EQ(after.c_lflag & ~ignored_local_flags,
            before.c_lflag & ~ignored_local_flags);
}

TEST(TerminalSessionTest, DestructorRestoresRawModeAndPresentationModes) {
  PseudoTerminal terminal;
  ASSERT_TRUE(terminal.valid());
  ASSERT_TRUE(terminal.set_size(80, 24));
  termios before{};
  ASSERT_EQ(::tcgetattr(terminal.slave_fd(), &before), 0);

  {
    TerminalSession session(terminal.slave_fd(), terminal.slave_fd());
    ASSERT_EQ(session.take(), Status::OK);
    static_cast<void>(terminal.read_output());
  }

  termios after{};
  ASSERT_EQ(::tcgetattr(terminal.slave_fd(), &after), 0);
  tcflag_t ignored_local_flags = 0;
#ifdef PENDIN
  ignored_local_flags |= PENDIN;
#endif
  EXPECT_EQ(after.c_lflag & ~ignored_local_flags,
            before.c_lflag & ~ignored_local_flags);
  EXPECT_NE(terminal.read_output().find("\x1b[?1049l"), std::string::npos);
}

}  // namespace
}  // namespace puc::terminal
