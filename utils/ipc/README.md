# PUC IPC

`utils/ipc` is PUC's named byte-event bus and its optional Unix transport
layer. It is intended for logical event paths such as:

- `//screen/resize_events`
- `//screen/mouse_events`
- `//screen/trie_errors`

The library deliberately separates three concerns:

1. `Directory` owns the mapping from canonical names to channels and stable
   numeric wire IDs.
2. `Channel` owns subscriptions; a concrete channel decides how bytes move.
3. `Message` provides a portable envelope when several logical channels need
   to cross a process boundary.

The event payload itself belongs to the layer that defines the event. IPC does
not reinterpret a resize event, mouse event, or terminal decoder error.

## Local event bus

`SmemChannel` is the ordinary in-process event mechanism. Without delivery
options, it invokes subscribers synchronously on the transmitting thread,
borrows the payload for the duration of each callback, and does not copy it.
Set `ChannelOptions::channel_max_depth` to make delivery asynchronous and
bounded after registration with a `Directory`. The channel then owns at most
the N newest pending messages, evicts the oldest pending message when full,
and delivers retained messages FIFO on the Directory's borrowed worker pool. A
message whose callback is already executing is no longer pending and is never
evicted. The pool owner must keep it active until the Directory is destroyed;
Directory drains its channels but never stops or joins the pool.

```cpp
#include "utils/ipc/ipc.hpp"
#include "utils/multithreading/job_queue.hpp"

puc::multithreading::JobQueue workers(4U);
puc::ipc::Directory events(workers);
auto resize = std::make_shared<puc::ipc::SmemChannel>(
    "//screen/resize_events", 64U,
    puc::ipc::ChannelOptions{.channel_max_depth = 1U});

puc::ipc::ChannelId resize_id = 0U;
if (events.open_channel(resize, resize_id) != puc::ipc::Status::OK) {
  // Log or propagate the status.
}

puc::ipc::Subscription subscription;
events.subscribe(
    "//screen/resize_events",
    [](puc::ipc::Channel::Bytes event) noexcept {
      // Decode and consume the screen-owned resize payload here.
    },
    subscription);

// Bounded delivery copies this payload before transmit() returns.
events.transmit("//screen/resize_events", encoded_resize_event);
```

A `Subscription` is the callback's ownership token. Moving it transfers that
ownership; resetting or destroying it disables the callback for later
activation checks. One concurrent delivery that already observed it as enabled
may still invoke or finish it. Callbacks must be `noexcept`, must not retain the
borrowed byte span, and should return quickly. `pending_messages()` exposes the
current bounded backlog, while `dropped_messages()` reports lifetime
oldest-pending evictions. An unset maximum preserves the synchronous path;
setting it to zero is invalid.

## Cross-process transports

`SocketChannel` provides a bidirectional Unix-domain stream socket. A server
binds and accepts one peer at a time; a client connects during construction.
The server removes only the socket path it successfully created.

`FileBufferChannel` provides the same framing over two existing POSIX named
pipes. Endpoint B swaps endpoint A's read and write paths. The caller owns both
FIFO entries; the channel never creates or removes them. Ordinary files are
rejected.

Both transports prefix every payload with a four-byte network-order length,
handle partial nonblocking I/O, serialize concurrent writers, and deliver
incoming frames from a private reader thread. They impose backpressure: a
synchronous `transmit()` can wait while the peer is not reading.

`MetaChannel` composes transports. It sends to every underlying channel in
order and relays messages received from any of them. Mixed fan-out success is
reported as `PARTIAL_TRANSFER`.

## Multiplexing logical channels

The version-zero `Message` codec is useful when one physical transport carries
many logical directory channels:

1. Look up the logical channel's `ChannelId` in its `Directory`.
2. Serialize that ID, a sender-selected message ID, optional metadata, and the
   payload with `serialize_message()`.
3. Send the serialized bytes through the physical channel.
4. In the physical channel's receive callback, call `deserialize_message()`.
5. Route the borrowed decoded payload with
   `directory.transmit(decoded.header.channel_id, decoded.payload)`.

The codec never copies C++ object layouts. Its fixed base header is:

| Offset | Bytes | Meaning |
|---:|---:|---|
| 0 | 4 | ASCII magic `PUCI` |
| 4 | 1 | wire version (`0`) |
| 5 | 1 | optional-section flags |
| 6 | 2 | complete header size, network byte order |
| 8 | 4 | nonzero channel ID, network byte order |
| 12 | 4 | sender-selected message ID, network byte order |
| 16 | 8 | payload length, network byte order |

Flag bits 0 through 4 mean session ID, timestamp, multipart metadata, checksum,
and extension bytes respectively; bits 5 through 7 must be zero. Present
sections follow in that same order: a one-byte session length and up to 16
session bytes, an eight-byte nanosecond Unix timestamp, two four-byte multipart
integers (`total_parts`, then zero-based `part_index`), and finally any opaque
extension bytes through the declared header size. The payload follows the
complete header. When requested, a 32-byte SHA-256 digest over all preceding
bytes follows the payload. Decoding borrows payload and extension spans from
the input buffer and reports the exact number of bytes consumed, allowing
concatenated messages.

## Error and lifetime model

Operational failures use `Status` and `TransferResult`; the library does not
use exception-based error control flow. Persistent construction failures are
available through `Channel::status()`.

Destroy transports only after application threads have stopped calling them.
Destroying a channel from inside its own callback is unsupported. A socket or
FIFO reader may invoke callbacks concurrently with unrelated application
threads. An unbounded `SmemChannel` invokes them synchronously on its caller;
a bounded channel serializes them through its Directory's delivery workers.
