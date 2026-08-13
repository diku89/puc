/**
 * @file screen_test.cpp
 * @brief Pseudo-terminal tests for asynchronous Screen commands and state.
 */

#include "puc-cli/tui/rendering/screen.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <util.h>
#else
#include <pty.h>
#endif

#include "gtest/gtest.h"
#include "msgs/screen_msgs.hpp"
#include "msgs/terminal_msgs.hpp"
#include "properties/properties.hpp"
#include "puc-cli/tui/rendering/frame.hpp"
#include "puc-cli/tui/rendering/zbuf.hpp"
#include "puc-cli/tui/terminal/event.hpp"
#include "utils/ipc/channel.hpp"
#include "utils/multithreading/job_queue.hpp"

namespace puc::tui {
namespace {

using namespace std::chrono_literals;

static_assert(kDefaultCellDimensions ==
              CellDimensions{.width = 1U, .height = 2U});

template <typename Predicate>
bool wait_until(Predicate predicate, std::chrono::milliseconds timeout = 2s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return predicate();
}

/** RAII pseudo-terminal pair used to observe real POSIX terminal behavior. */
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

  bool write_input(std::string_view input) const noexcept {
    std::size_t offset = 0U;
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

  bool set_dimensions(std::size_t width, std::size_t height,
                      std::size_t pixel_width  = 0U,
                      std::size_t pixel_height = 0U) const noexcept {
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

  std::string read_available() const {
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

Canvas::Cell cell(char32_t character, std::uint32_t foreground,
                  std::uint32_t background) {
  return Canvas::Cell{
      .character        = character,
      .foreground_color = foreground,
      .background_color = background,
  };
}

/** Selectable frame double used to observe explicit clipboard dispatch. */
class ClipboardFrame final : public Frame {
 public:
  ClipboardFrame() : Frame("clipboard") {}

  Status draw(const Theme&, Canvas&, const Canvas::Rect&) override {
    return Status::OK;
  }

  bool is_selectable() const noexcept override { return true; }

  Status update_selection(const SelectionEvent& event) override {
    selected_ = event.type != SelectionEventType::RESET;
    return Status::OK;
  }

  Status selected_text(std::string& output) const override {
    output.clear();
    if (!selected_) {
      return Status::NO_SELECTION;
    }
    output = "PUC";
    return Status::OK;
  }

 private:
  bool selected_ = false; /**< Whether a semantic range currently exists. */
};

bool raw_mode_enabled(int descriptor, const termios& original) {
  termios current{};
  return ::tcgetattr(descriptor, &current) == 0 &&
         (current.c_lflag & (ECHO | ICANON)) == 0U &&
         (current.c_lflag & ISIG) == (original.c_lflag & ISIG);
}

bool terminal_state_restored(int descriptor, const termios& original) {
  termios current{};
  if (::tcgetattr(descriptor, &current) != 0) {
    return false;
  }
  tcflag_t ignored_local_flags = 0U;
#ifdef PENDIN
  ignored_local_flags |= PENDIN;
#endif
  return current.c_iflag == original.c_iflag &&
         current.c_oflag == original.c_oflag &&
         current.c_cflag == original.c_cflag &&
         (current.c_lflag & ~ignored_local_flags) ==
             (original.c_lflag & ~ignored_local_flags);
}

TEST(ScreenTest, ConfiguresBoundedDirectionSpecificChannels) {
  multithreading::JobQueue workers;
  Screen screen(-1, -1, workers);
  const std::shared_ptr<ipc::Channel> commands =
      screen.ipc_directory().get_channel(msg::kScreenCommandChannel);
  const std::shared_ptr<ipc::Channel> resize =
      screen.ipc_directory().get_channel(msg::kScreenResizeEventChannel);
  const std::shared_ptr<ipc::Channel> input =
      screen.ipc_directory().get_channel(msg::kTerminalInputEventChannel);
  ASSERT_NE(commands, nullptr);
  ASSERT_NE(resize, nullptr);
  ASSERT_NE(input, nullptr);
  EXPECT_EQ(commands->channel_max_depth(), 3U);
  EXPECT_EQ(resize->channel_max_depth(), 1U);
  EXPECT_EQ(input->channel_max_depth(), std::nullopt);
  EXPECT_EQ(input->subscriber_count(), 1U);
  EXPECT_EQ(screen.pending_commands(), 0U);
  EXPECT_EQ(screen.dropped_commands(), 0U);
}

TEST(ScreenTest, ConsumesAndRetainsNormalizedTerminalEventsInOrder) {
  multithreading::JobQueue workers;
  Screen screen(-1, -1, workers);
  msg::TerminalInputEventCodec codec;

  const auto publish_text = [&](std::string text) {
    const msg::TerminalInputEvent message{
        .data = msg::TerminalTextEvent{.utf8 = std::move(text)}};
    std::vector<std::uint8_t> payload;
    if (!msg::is_ok(codec.serialize(message, payload))) {
      return false;
    }
    const ipc::TransferResult result = screen.ipc_directory().transmit(
        msg::kTerminalInputEventChannel, payload);
    return ipc::is_ok(result.status) && result.bytes == payload.size();
  };

  ASSERT_TRUE(publish_text("first"));
  ASSERT_TRUE(publish_text("second"));
  std::vector<terminal::Event> events;
  ASSERT_EQ(screen.drain_input_events(events), Status::OK);
  ASSERT_EQ(events.size(), 2U);
  ASSERT_TRUE(std::holds_alternative<terminal::TextEvent>(events[0]));
  ASSERT_TRUE(std::holds_alternative<terminal::TextEvent>(events[1]));
  EXPECT_EQ(std::get<terminal::TextEvent>(events[0]).utf8, "first");
  EXPECT_EQ(std::get<terminal::TextEvent>(events[1]).utf8, "second");

  ASSERT_EQ(screen.drain_input_events(events), Status::OK);
  EXPECT_TRUE(events.empty());
}

TEST(ScreenTest, ReportsMalformedTerminalInputAtTheDrainBoundary) {
  multithreading::JobQueue workers;
  Screen screen(-1, -1, workers);
  const std::vector<std::uint8_t> malformed{0xffU};
  const ipc::TransferResult result = screen.ipc_directory().transmit(
      msg::kTerminalInputEventChannel, malformed);
  ASSERT_TRUE(ipc::is_ok(result.status));
  ASSERT_EQ(result.bytes, malformed.size());

  std::vector<terminal::Event> events;
  EXPECT_EQ(screen.drain_input_events(events), Status::MESSAGE_DECODING_FAILED);
  EXPECT_TRUE(events.empty());
}

TEST(ScreenTest, BorrowsOneSharedApplicationWorkerBudget) {
  multithreading::JobQueue workers(4U);
  {
    Screen screen(-1, -1, workers);
    EXPECT_EQ(screen.ipc_directory().delivery_worker_count(), 4U);
  }
  EXPECT_TRUE(workers.active());
}

TEST(ScreenTest, GeometryIsUnavailableUntilAnObservedResizeEvent) {
  multithreading::JobQueue workers;
  Screen screen(-1, -1, workers);
  std::size_t width  = 1U;
  std::size_t height = 1U;
  CellDimensions dimensions{.width = 9U, .height = 9U};
  EXPECT_EQ(screen.get_dimensions(width, height, dimensions),
            Status::TERMINAL_QUERY_FAILED);
  EXPECT_EQ(width, 0U);
  EXPECT_EQ(height, 0U);
  EXPECT_EQ(dimensions, kDefaultCellDimensions);
  EXPECT_EQ(screen.draw(), Status::TERMINAL_NOT_AVAILABLE);
}

TEST(ScreenTest, CanvasAttachmentRejectsNullAndInvalidCanvases) {
  multithreading::JobQueue workers;
  Screen screen(-1, -1, workers);
  EXPECT_EQ(screen.set_canvas(nullptr), Status::CANVAS_NOT_SET);
  const auto invalid =
      std::make_shared<Canvas>(std::numeric_limits<std::size_t>::max(), 2U);
  EXPECT_EQ(screen.set_canvas(invalid), Status::DIMENSION_OVERFLOW);
  EXPECT_EQ(screen.set_canvas(std::make_shared<Canvas>(2U, 1U)), Status::OK);
}

TEST(ScreenTest, TakeAndReleaseAreAsynchronousAndRestoreTerminalState) {
  multithreading::JobQueue workers;
  PseudoTerminal terminal;
  ASSERT_TRUE(terminal.valid());
  ASSERT_TRUE(terminal.set_dimensions(80U, 24U));
  termios before{};
  ASSERT_EQ(::tcgetattr(terminal.slave_fd(), &before), 0);

  Screen screen(terminal.slave_fd(), terminal.slave_fd(), workers);
  ASSERT_EQ(screen.take(), Status::OK);
  EXPECT_TRUE(screen.is_taken());
  ASSERT_TRUE(wait_until(
      [&] { return raw_mode_enabled(terminal.slave_fd(), before); }));

  std::size_t width  = 0U;
  std::size_t height = 0U;
  ASSERT_TRUE(wait_until(
      [&] { return screen.get_dimensions(width, height) == Status::OK; }));
  EXPECT_EQ(width, 80U);
  EXPECT_EQ(height, 24U);
  EXPECT_NE(terminal.read_available().find("\x1b[2J\x1b[H"), std::string::npos);

  ASSERT_EQ(screen.release(), Status::OK);
  EXPECT_FALSE(screen.is_taken());
  ASSERT_TRUE(wait_until(
      [&] { return terminal_state_restored(terminal.slave_fd(), before); }));
  const std::string output = terminal.read_available();
  EXPECT_NE(output.find("\x1b[0m"), std::string::npos);
  EXPECT_NE(output.find("\x1b[?1049l"), std::string::npos);
}

TEST(ScreenTest, DefaultTakeRequestsTheCompleteInteractiveContract) {
  multithreading::JobQueue workers;
  PseudoTerminal terminal;
  ASSERT_TRUE(terminal.valid());
  ASSERT_TRUE(terminal.set_dimensions(80U, 24U));

  Screen screen(terminal.slave_fd(), terminal.slave_fd(), workers);
  ASSERT_EQ(screen.take(), Status::OK);

  std::string entered;
  ASSERT_TRUE(wait_until([&] {
    entered.append(terminal.read_available());
    return entered.contains("\x1b[?1049h") && entered.contains("\x1b[?25l") &&
           entered.contains("\x1b[?7l") && entered.contains("\x1b[?2004h") &&
           entered.contains("\x1b[?1004h") && entered.contains("\x1b[?1002h") &&
           entered.contains("\x1b[?1006h") && entered.contains("\x1b[>29u");
  }));

  ASSERT_EQ(screen.release(), Status::OK);
  std::string released;
  ASSERT_TRUE(wait_until([&] {
    released.append(terminal.read_available());
    return released.contains("\x1b[<u") && released.contains("\x1b[?1006l") &&
           released.contains("\x1b[?1002l") &&
           released.contains("\x1b[?1004l") &&
           released.contains("\x1b[?2004l") && released.contains("\x1b[?7h") &&
           released.contains("\x1b[?25h") && released.contains("\x1b[?1049l");
  }));
}

TEST(ScreenTest, ReadInputUsesTheOwnedTerminalSessionAndCallerDecoder) {
  multithreading::JobQueue workers;
  PseudoTerminal terminal;
  ASSERT_TRUE(terminal.valid());
  ASSERT_TRUE(terminal.set_dimensions(80U, 24U));

  std::error_code error;
  const std::filesystem::path root = std::filesystem::current_path(error);
  ASSERT_FALSE(error);
  puc::properties::Properties properties{root, root / "missing_user_overrides"};
  puc::terminal::Decoder decoder;
  ASSERT_EQ(decoder.setup(properties, "xterm-256color"),
            puc::terminal::Status::OK);

  Screen screen(terminal.slave_fd(), terminal.slave_fd(), workers);
  ASSERT_EQ(screen.take(), Status::OK);
  std::size_t width  = 0U;
  std::size_t height = 0U;
  ASSERT_TRUE(wait_until(
      [&] { return screen.get_dimensions(width, height) == Status::OK; }));
  ASSERT_TRUE(terminal.write_input("puc"));

  std::vector<puc::terminal::Event> events;
  std::size_t bytes_read = 0U;
  bool end_of_input      = false;
  ASSERT_TRUE(wait_until([&] {
    const puc::terminal::Status status =
        screen.read_input(decoder, events, bytes_read, end_of_input);
    return status == puc::terminal::Status::OK && !events.empty();
  }));
  ASSERT_EQ(events.size(), 1U);
  ASSERT_TRUE(std::holds_alternative<puc::terminal::TextEvent>(events.front()));
  EXPECT_EQ(std::get<puc::terminal::TextEvent>(events.front()).utf8, "puc");
  EXPECT_EQ(bytes_read, 3U);
  EXPECT_FALSE(end_of_input);
  EXPECT_EQ(screen.release(), Status::OK);
}

TEST(ScreenTest, DestructionSynchronouslyBackstopsQueuedRelease) {
  multithreading::JobQueue workers;
  PseudoTerminal terminal;
  ASSERT_TRUE(terminal.valid());
  ASSERT_TRUE(terminal.set_dimensions(80U, 24U));
  termios before{};
  ASSERT_EQ(::tcgetattr(terminal.slave_fd(), &before), 0);

  {
    Screen screen(terminal.slave_fd(), terminal.slave_fd(), workers);
    ASSERT_EQ(screen.take(), Status::OK);
    ASSERT_TRUE(wait_until(
        [&] { return raw_mode_enabled(terminal.slave_fd(), before); }));
  }
  EXPECT_TRUE(terminal_state_restored(terminal.slave_fd(), before));
  EXPECT_NE(terminal.read_available().find("\x1b[0m"), std::string::npos);
}

TEST(ScreenTest, ReportsRelativeCharacterCellDimensionsFromResizeState) {
  multithreading::JobQueue workers;
  PseudoTerminal terminal;
  ASSERT_TRUE(terminal.valid());
  ASSERT_TRUE(terminal.set_dimensions(80U, 24U, 960U, 480U));
  Screen screen(terminal.slave_fd(), terminal.slave_fd(), workers);
  ASSERT_EQ(screen.take(), Status::OK);

  std::size_t width  = 0U;
  std::size_t height = 0U;
  CellDimensions dimensions;
  ASSERT_TRUE(wait_until([&] {
    return screen.get_dimensions(width, height, dimensions) == Status::OK;
  }));
  EXPECT_EQ(width, 80U);
  EXPECT_EQ(height, 24U);
  EXPECT_EQ(dimensions, (CellDimensions{.width = 3U, .height = 5U}));

  ASSERT_EQ(screen.set_canvas(std::make_shared<Canvas>(1U, 1U)), Status::OK);
  ASSERT_TRUE(terminal.set_dimensions(80U, 24U));
  ASSERT_EQ(screen.draw(), Status::OK);
  ASSERT_TRUE(wait_until([&] {
    return screen.get_dimensions(width, height, dimensions) == Status::OK &&
           dimensions == kDefaultCellDimensions;
  }));
  EXPECT_EQ(screen.release(), Status::OK);
}

TEST(ScreenTest, DrawRequiresAnAttachedCanvas) {
  multithreading::JobQueue workers;
  PseudoTerminal terminal;
  ASSERT_TRUE(terminal.valid());
  ASSERT_TRUE(terminal.set_dimensions(10U, 5U));
  Screen screen(terminal.slave_fd(), terminal.slave_fd(), workers);
  ASSERT_EQ(screen.take(), Status::OK);
  EXPECT_EQ(screen.draw(), Status::CANVAS_NOT_SET);
  EXPECT_EQ(screen.release(), Status::OK);
}

TEST(ScreenTest, DrawAsynchronouslyEmitsTrueColorAndUtf8Cells) {
  multithreading::JobQueue workers;
  PseudoTerminal terminal;
  ASSERT_TRUE(terminal.valid());
  ASSERT_TRUE(terminal.set_dimensions(2U, 1U));
  Screen screen(terminal.slave_fd(), terminal.slave_fd(), workers);
  ASSERT_EQ(screen.take(), Status::OK);
  std::size_t width  = 0U;
  std::size_t height = 0U;
  ASSERT_TRUE(wait_until(
      [&] { return screen.get_dimensions(width, height) == Status::OK; }));
  static_cast<void>(terminal.read_available());

  auto canvas = std::make_shared<Canvas>(2U, 1U);
  std::vector<Canvas::Cell> row{
      cell(U'A', 0x112233U, 0x445566U),
      cell(U'λ', 0x112233U, 0x445566U),
  };
  std::vector<std::span<Canvas::Cell>> rows{std::span<Canvas::Cell>{row}};
  ASSERT_EQ(canvas->begin_frame(), Status::OK);
  ASSERT_EQ(canvas->write_cells(
                Canvas::Rect{.x = 0U, .y = 0U, .width = 2U, .height = 1U},
                std::span<std::span<Canvas::Cell>>{rows}),
            Status::OK);
  ASSERT_EQ(canvas->end_frame(), Status::OK);
  ASSERT_EQ(screen.set_canvas(canvas), Status::OK);
  ASSERT_EQ(screen.draw(), Status::OK);

  std::string output;
  ASSERT_TRUE(wait_until([&] {
    output.append(terminal.read_available());
    return output.find("A\xce\xbb") != std::string::npos &&
           output.find("\x1b[0m") != std::string::npos;
  }));
  EXPECT_NE(output.find("\x1b[H"), std::string::npos);
  EXPECT_NE(output.find("\x1b[38;2;17;34;51m"), std::string::npos);
  EXPECT_NE(output.find("\x1b[48;2;68;85;102m"), std::string::npos);
  EXPECT_EQ(screen.release(), Status::OK);
}

TEST(ScreenTest, SelectionDoesNotTouchClipboardUntilExplicitCopyCommand) {
  multithreading::JobQueue workers;
  PseudoTerminal terminal;
  ASSERT_TRUE(terminal.valid());
  ASSERT_TRUE(terminal.set_dimensions(20U, 5U));
  Screen screen(terminal.slave_fd(), terminal.slave_fd(), workers);
  ASSERT_EQ(screen.take(), Status::OK);
  std::size_t width  = 0U;
  std::size_t height = 0U;
  ASSERT_TRUE(wait_until(
      [&] { return screen.get_dimensions(width, height) == Status::OK; }));
  static_cast<void>(terminal.read_available());

  ZBuffer z_buffer;
  const auto frame = std::make_shared<ClipboardFrame>();
  ASSERT_EQ(z_buffer.add("clipboard", frame), Status::OK);
  const std::map<std::string, Canvas::Rect> layouts{
      {"clipboard", {.x = 0U, .y = 0U, .width = 20U, .height = 5U}},
  };
  const puc::terminal::MouseEvent press{
      .position = {.x = 2U, .y = 2U},
      .button   = puc::terminal::MouseButton::LEFT,
      .action   = puc::terminal::MouseAction::PRESS,
  };
  const puc::terminal::MouseEvent release{
      .position = press.position,
      .button   = puc::terminal::MouseButton::LEFT,
      .action   = puc::terminal::MouseAction::RELEASE,
  };
  ASSERT_EQ(screen.handle_mouse_event(press, z_buffer, layouts), Status::OK);
  ASSERT_EQ(screen.handle_mouse_event(release, z_buffer, layouts), Status::OK);
  ASSERT_EQ(screen.handle_mouse_event(press, z_buffer, layouts), Status::OK);
  ASSERT_EQ(screen.handle_mouse_event(release, z_buffer, layouts), Status::OK);
  ASSERT_EQ(screen.selection_phase(), SelectionPhase::COMPLETE);

  std::this_thread::sleep_for(10ms);
  EXPECT_EQ(terminal.read_available().find("\x1b]52;"), std::string::npos);

  ASSERT_EQ(screen.copy_selection(), Status::OK);
  std::string output;
  ASSERT_TRUE(wait_until([&] {
    output.append(terminal.read_available());
    return output.find("\x1b]52;c;UFVD\x1b\\") != std::string::npos;
  }));
  EXPECT_EQ(screen.release(), Status::OK);
}

TEST(ScreenTest, PublishesInitialAndChangedGeometryAsStateEvents) {
  multithreading::JobQueue workers;
  PseudoTerminal terminal;
  ASSERT_TRUE(terminal.valid());
  ASSERT_TRUE(terminal.set_dimensions(80U, 24U));
  Screen screen(terminal.slave_fd(), terminal.slave_fd(), workers);

  msg::ScreenResizeEventCodec codec;
  std::mutex events_mutex;
  std::vector<msg::ScreenResizeEvent> events;
  ipc::Subscription subscription;
  ASSERT_EQ(screen.ipc_directory().subscribe(
                msg::kScreenResizeEventChannel,
                [&](ipc::Channel::Bytes payload) noexcept {
                  msg::ScreenResizeEvent event;
                  if (msg::is_ok(codec.deserialize(payload, event))) {
                    const std::lock_guard lock(events_mutex);
                    events.push_back(event);
                  }
                },
                subscription),
            ipc::Status::OK);

  ASSERT_EQ(screen.take(), Status::OK);
  ASSERT_TRUE(wait_until([&] {
    const std::lock_guard lock(events_mutex);
    return events.size() == 1U;
  }));
  ASSERT_EQ(screen.set_canvas(std::make_shared<Canvas>(1U, 1U)), Status::OK);
  ASSERT_TRUE(terminal.set_dimensions(100U, 40U));
  ASSERT_EQ(screen.draw(), Status::OK);
  ASSERT_TRUE(wait_until([&] {
    const std::lock_guard lock(events_mutex);
    return events.size() == 2U;
  }));
  {
    const std::lock_guard lock(events_mutex);
    EXPECT_EQ(events.front().width, 80U);
    EXPECT_EQ(events.front().height, 24U);
    EXPECT_EQ(events.back().width, 100U);
    EXPECT_EQ(events.back().height, 40U);
  }
  EXPECT_EQ(screen.release(), Status::OK);
}

}  // namespace
}  // namespace puc::tui
