# PUC message payload codecs

`//msgs:msgs` is the typed payload layer above `//utils/ipc:ipc`. IPC owns
channels, transport, framing, checksums, and the portable envelope. This package
maps the envelope's message id to a C++ struct and a portable payload schema.

Each message struct is a regular C++ value with a `std::formatter<T, char>`
specialization. Its default format must be complete, valid JSON. Consequently,
every typed codec gets JSON conversion from the generic implementation:

```cpp
struct CursorMoved {
  std::uint16_t row = 0;
  std::uint16_t column = 0;
  bool operator==(const CursorMoved&) const = default;
};

template <>
struct std::formatter<CursorMoved, char> {
  constexpr auto parse(std::format_parse_context& context) {
    return context.begin();
  }

  template <typename FormatContext>
  auto format(const CursorMoved& event, FormatContext& context) const {
    return std::format_to(
        context.out(), "{{\"row\":{},\"column\":{}}}", event.row,
        event.column);
  }
};
```

Formatters that include text fields are responsible for JSON escaping those
strings. Changing a payload's byte schema incompatibly requires a new
`MessageId`; payload codecs must never serialize native object layout.

Derive `Codec<T>`, implement the two payload hooks, then register an owning
codec during setup. `MessageCodecCollection` can serialize and deserialize by
type, or inspect an IPC envelope and dispatch its payload directly to JSON.
IPC itself treats its message-id field as opaque; this package deliberately
uses that field as a globally unique payload schema id. Failed stream decoding
leaves the input span untouched, and an incomplete envelope has a distinct
status so callers can wait for more bytes without losing the partial message.

`screen_msgs.hpp` is the contract between `puc::tui::Screen` and
`puc::terminal::TerminalSession`. `ScreenCommand` is a one-way variant for
taking, presenting to, and releasing a terminal; `ScreenResizeEvent` publishes
observed geometry as state. The contract intentionally has no command result
or error reply. Its stable channel names are `//screen/commands` and
`//screen/resize_events`.
