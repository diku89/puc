/**
 * @file screen_test.cpp
 * @brief Pseudo-terminal tests for Screen lifecycle, rendering, and events.
 */

#include "puc-cli/tui/screen.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <util.h>
#else
#include <pty.h>
#endif

#include "gtest/gtest.h"

namespace puc::tui {
namespace {

/**
 * RAII pseudo-terminal pair used to exercise real POSIX terminal behavior.
 *
 * Screen operates on the slave descriptor while tests read emitted ANSI bytes
 * from the nonblocking master descriptor.
 */
class PseudoTerminal {
 public:
  /** Open a master/slave pair and make master reads nonblocking. */
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

  /** Close any descriptors successfully opened by the constructor. */
  ~PseudoTerminal() {
    if (master_fd_ >= 0) {
      static_cast<void>(::close(master_fd_));
    }
    if (slave_fd_ >= 0) {
      static_cast<void>(::close(slave_fd_));
    }
  }

  /** Return whether both sides of the pseudo-terminal were opened. */
  bool valid() const noexcept { return master_fd_ >= 0 && slave_fd_ >= 0; }

  /** Return the borrowed slave descriptor passed to Screen. */
  int slave_fd() const noexcept { return slave_fd_; }

  /**
   * Set cell and optional total pixel dimensions reported by `TIOCGWINSZ`.
   *
   * @return `false` when a value cannot fit `winsize` or the ioctl fails.
   */
  bool set_dimensions(size_t width, size_t height, size_t pixel_width = 0,
                      size_t pixel_height = 0) const noexcept {
    if (width > std::numeric_limits<unsigned short>::max() ||
        height > std::numeric_limits<unsigned short>::max() ||
        pixel_width > std::numeric_limits<unsigned short>::max() ||
        pixel_height > std::numeric_limits<unsigned short>::max()) {
      return false;
    }
    const struct winsize dimensions {
      .ws_row    = static_cast<unsigned short>(height),
      .ws_col    = static_cast<unsigned short>(width),
      .ws_xpixel = static_cast<unsigned short>(pixel_width),
      .ws_ypixel = static_cast<unsigned short>(pixel_height),
    };
    return ::ioctl(slave_fd_, TIOCSWINSZ, &dimensions) == 0;
  }

  /** Drain and return all bytes currently available from the master side. */
  std::string read_available() const {
    std::string output;
    char bytes[512];
    while (true) {
      const ssize_t count = ::read(master_fd_, bytes, sizeof(bytes));
      if (count > 0) {
        output.append(bytes, static_cast<size_t>(count));
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
  /** Descriptor from which tests observe Screen output. */
  int master_fd_ = -1;
  /** Descriptor that behaves as Screen's terminal. */
  int slave_fd_ = -1;
};

/** Construct a Canvas cell used by ANSI serialization assertions. */
Canvas::Cell cell(char32_t character, uint32_t foreground,
                  uint32_t background) {
  return Canvas::Cell{
      .character        = character,
      .foreground_color = foreground,
      .background_color = background,
  };
}

TEST(ScreenTest, EventBufferUsesEverySlotAndPreservesFifoOrder) {
  const auto buffer = std::make_shared<Screen::EventBuffer>();
  Screen screen(buffer, -1, -1);

  for (size_t index = 0; index < Screen::EventBuffer::kMaxEvents; ++index) {
    Screen::Event event{
        .timestamp = index,
        .id        = index + 1,
        .type      = Screen::EventType::KEY_PRESS,
    };
    event.data.key = static_cast<uint32_t>(index);
    ASSERT_EQ(Screen::push_event(buffer, event), Status::OK);
  }

  Screen::Event overflow;
  EXPECT_EQ(Screen::push_event(buffer, overflow), Status::EVENT_BUFFER_FULL);
  EXPECT_EQ(screen.pending_events(), Screen::EventBuffer::kMaxEvents);

  for (size_t index = 0; index < Screen::EventBuffer::kMaxEvents; ++index) {
    const std::optional<Screen::Event> event = screen.pop_event();
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->id, index + 1);
    EXPECT_EQ(event->data.key, static_cast<uint32_t>(index));
  }
  EXPECT_FALSE(screen.pop_event().has_value());
  EXPECT_EQ(screen.pending_events(), 0U);
}

TEST(ScreenTest, NullEventBuffersAreRejectedSafely) {
  Screen screen(nullptr, -1, -1);
  Screen::Event event;

  EXPECT_EQ(Screen::push_event(nullptr, event), Status::INVALID_ARGUMENT);
  EXPECT_FALSE(screen.pop_event().has_value());
  EXPECT_EQ(screen.pending_events(), 0U);
  EXPECT_EQ(screen.take(nullptr), Status::INVALID_ARGUMENT);
}

TEST(ScreenTest, NonTerminalDescriptorsCannotBeTakenOrQueried) {
  int descriptors[2];
  ASSERT_EQ(::pipe(descriptors), 0);
  const auto buffer = std::make_shared<Screen::EventBuffer>();
  {
    Screen screen(buffer, descriptors[0], descriptors[1]);
    EXPECT_EQ(screen.take(buffer), Status::TERMINAL_NOT_AVAILABLE);
    EXPECT_FALSE(screen.is_taken());

    size_t width  = 1;
    size_t height = 1;
    EXPECT_EQ(screen.get_dimensions(width, height),
              Status::TERMINAL_QUERY_FAILED);
    EXPECT_EQ(width, 0U);
    EXPECT_EQ(height, 0U);
    EXPECT_EQ(screen.draw(), Status::TERMINAL_NOT_AVAILABLE);
  }
  EXPECT_EQ(::close(descriptors[0]), 0);
  EXPECT_EQ(::close(descriptors[1]), 0);
}

TEST(ScreenTest, CanvasAttachmentRejectsNullAndInvalidCanvases) {
  const auto buffer = std::make_shared<Screen::EventBuffer>();
  Screen screen(buffer, -1, -1);

  EXPECT_EQ(screen.set_canvas(nullptr), Status::CANVAS_NOT_SET);
  const auto invalid =
      std::make_shared<Canvas>(std::numeric_limits<size_t>::max(), 2);
  EXPECT_EQ(screen.set_canvas(invalid), Status::DIMENSION_OVERFLOW);
  EXPECT_EQ(screen.set_canvas(std::make_shared<Canvas>(2, 1)), Status::OK);
}

TEST(ScreenTest, ReportsRelativeCharacterCellDimensions) {
  PseudoTerminal terminal;
  ASSERT_TRUE(terminal.valid());
  ASSERT_TRUE(terminal.set_dimensions(80, 24, 960, 480));

  const auto buffer = std::make_shared<Screen::EventBuffer>();
  Screen screen(buffer, terminal.slave_fd(), terminal.slave_fd());
  size_t width  = 0;
  size_t height = 0;
  CellDimensions cell_dimensions;
  ASSERT_EQ(screen.get_dimensions(width, height, cell_dimensions), Status::OK);
  EXPECT_EQ(width, 80U);
  EXPECT_EQ(height, 24U);
  EXPECT_EQ(cell_dimensions.width, 3U);
  EXPECT_EQ(cell_dimensions.height, 5U);

  ASSERT_TRUE(terminal.set_dimensions(80, 24));
  ASSERT_EQ(screen.get_dimensions(width, height, cell_dimensions), Status::OK);
  EXPECT_EQ(cell_dimensions.width, 1U);
  EXPECT_EQ(cell_dimensions.height, 2U);
}

TEST(ScreenTest, TakeEnablesRawModeAndReleaseRestoresTerminal) {
  PseudoTerminal terminal;
  ASSERT_TRUE(terminal.valid());
  ASSERT_TRUE(terminal.set_dimensions(80, 24));

  struct termios before {};
  ASSERT_EQ(::tcgetattr(terminal.slave_fd(), &before), 0);

  const auto buffer = std::make_shared<Screen::EventBuffer>();
  Screen screen(buffer, terminal.slave_fd(), terminal.slave_fd());
  ASSERT_EQ(screen.take(buffer), Status::OK);
  EXPECT_TRUE(screen.is_taken());

  struct termios raw {};
  ASSERT_EQ(::tcgetattr(terminal.slave_fd(), &raw), 0);
  EXPECT_EQ(raw.c_lflag & (ECHO | ICANON), 0U);
  EXPECT_EQ(raw.c_lflag & ISIG, before.c_lflag & ISIG);

  size_t width  = 0;
  size_t height = 0;
  EXPECT_EQ(screen.get_dimensions(width, height), Status::OK);
  EXPECT_EQ(width, 80U);
  EXPECT_EQ(height, 24U);

  ASSERT_EQ(screen.release(), Status::OK);
  EXPECT_FALSE(screen.is_taken());

  struct termios after {};
  ASSERT_EQ(::tcgetattr(terminal.slave_fd(), &after), 0);
  EXPECT_EQ(after.c_iflag, before.c_iflag);
  EXPECT_EQ(after.c_oflag, before.c_oflag);
  EXPECT_EQ(after.c_cflag, before.c_cflag);
  tcflag_t ignored_local_flags = 0;
#ifdef PENDIN
  // Darwin may set PENDIN when canonical input is restored. It requests that
  // queued input be reprocessed and does not change the restored user mode.
  ignored_local_flags |= PENDIN;
#endif
  EXPECT_EQ(after.c_lflag & ~ignored_local_flags,
            before.c_lflag & ~ignored_local_flags);

  const std::string output = terminal.read_available();
  EXPECT_NE(output.find("\x1b[?1049h"), std::string::npos);
  EXPECT_NE(output.find("\x1b[?1049l"), std::string::npos);
}

TEST(ScreenTest, DrawRequiresAnAttachedCanvas) {
  PseudoTerminal terminal;
  ASSERT_TRUE(terminal.valid());
  ASSERT_TRUE(terminal.set_dimensions(10, 5));
  const auto buffer = std::make_shared<Screen::EventBuffer>();
  Screen screen(buffer, terminal.slave_fd(), terminal.slave_fd());

  ASSERT_EQ(screen.take(buffer), Status::OK);
  EXPECT_EQ(screen.draw(), Status::CANVAS_NOT_SET);
  EXPECT_EQ(screen.release(), Status::OK);
}

TEST(ScreenTest, DrawEmitsTrueColorAndUtf8Cells) {
  PseudoTerminal terminal;
  ASSERT_TRUE(terminal.valid());
  ASSERT_TRUE(terminal.set_dimensions(2, 1));
  const auto buffer = std::make_shared<Screen::EventBuffer>();
  Screen screen(buffer, terminal.slave_fd(), terminal.slave_fd());
  ASSERT_EQ(screen.take(buffer), Status::OK);
  static_cast<void>(terminal.read_available());

  auto canvas = std::make_shared<Canvas>(2, 1);
  std::vector<Canvas::Cell> row{
      cell(U'A', 0x112233, 0x445566),
      cell(U'λ', 0x112233, 0x445566),
  };
  std::vector<std::span<Canvas::Cell>> rows{std::span<Canvas::Cell>{row}};
  ASSERT_EQ(canvas->begin_frame(), Status::OK);
  ASSERT_EQ(
      canvas->write_cells(Canvas::Rect{.x = 0, .y = 0, .width = 2, .height = 1},
                          std::span<std::span<Canvas::Cell>>{rows}),
      Status::OK);
  ASSERT_EQ(canvas->end_frame(), Status::OK);
  ASSERT_EQ(screen.set_canvas(canvas), Status::OK);

  ASSERT_EQ(screen.draw(), Status::OK);
  const std::string output = terminal.read_available();
  EXPECT_NE(output.find("\x1b[H"), std::string::npos);
  EXPECT_NE(output.find("\x1b[38;2;17;34;51m"), std::string::npos);
  EXPECT_NE(output.find("\x1b[48;2;68;85;102m"), std::string::npos);
  EXPECT_NE(output.find("A\xce\xbb"), std::string::npos);
  EXPECT_NE(output.find("\x1b[0m"), std::string::npos);
  EXPECT_EQ(screen.release(), Status::OK);
}

TEST(ScreenTest, DrawQueuesResizeEventsWithTheNewDimensions) {
  PseudoTerminal terminal;
  ASSERT_TRUE(terminal.valid());
  ASSERT_TRUE(terminal.set_dimensions(80, 24));
  const auto buffer = std::make_shared<Screen::EventBuffer>();
  Screen screen(buffer, terminal.slave_fd(), terminal.slave_fd());
  ASSERT_EQ(screen.take(buffer), Status::OK);
  ASSERT_EQ(screen.set_canvas(std::make_shared<Canvas>(1, 1)), Status::OK);

  ASSERT_TRUE(terminal.set_dimensions(100, 40));
  ASSERT_EQ(screen.draw(), Status::OK);
  ASSERT_EQ(screen.pending_events(), 1U);

  const std::optional<Screen::Event> event = screen.pop_event();
  ASSERT_TRUE(event.has_value());
  EXPECT_EQ(event->type, Screen::EventType::RESIZE);
  EXPECT_EQ(event->id, 1U);
  EXPECT_NE(event->timestamp, 0U);
  EXPECT_EQ(event->data.resize.width, 100U);
  EXPECT_EQ(event->data.resize.height, 40U);
  EXPECT_EQ(screen.release(), Status::OK);
}

}  // namespace
}  // namespace puc::tui
