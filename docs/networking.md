# The first TCP exchange

Faultline can now start a coordinator on `127.0.0.1:9000`, accept a CLI connection,
receive a binary PING header, and send a binary PONG header. The coordinator also
accepts registration and heartbeat frames and owns a [worker registry](workers.md).
The worker executable's networking and job scheduling are still to come.

## Run it

Build with `make`, then use two terminals:

```sh
# Terminal 1
./build/debug/faultline-coordinator --port 9000

# Terminal 2
./build/debug/faultline ping --coordinator 127.0.0.1:9000
```

The CLI prints `PONG` and exits successfully only after validating the complete
response. Errors go to stderr and produce a nonzero exit status. Both programs
use `127.0.0.1:9000` by default; the address options above are optional.

The coordinator logs listening, connection, PING/PONG, registration, heartbeat,
and worker-death events. The `fd`
field is the operating system's connection descriptor, which can be reused
after closing a connection. It is not a worker ID. Ctrl+C or SIGTERM requests
shutdown, closes all client sockets and the listener, and exits the event loop.

## Files and responsibilities

| File | Responsibility |
| --- | --- |
| `include/net.h` | Networking API, defaults, and receive result codes. |
| `src/common/net.c` | Socket setup, port parsing, readiness waits, deadlines, and complete-transfer helpers. |
| `src/coordinator/main.c` | Listener, client slots, poll loop, frame handling, queued replies, and registry integration. |
| `include/worker_registry.h`, `src/coordinator/worker_registry.c` | Coordinator-owned IDs, connections, liveness states, and heartbeat timestamps. |
| `src/cli/main.c` | CLI arguments, one connection, request transmission, and response validation. |

`protocol.h` and `protocol.c` own the 12-byte header and complete-message formats.
Networking uses their encoders and decoders; it does not send raw C structs.

## Connection establishment

The coordinator creates an IPv4 TCP socket, enables `SO_REUSEADDR`, marks the
socket nonblocking, binds it to loopback and the selected port, and calls
`listen()`. Reuse-address helps with restarting after old TCP connections; it
does not permit a second live coordinator to claim the same listening address.

`accept()` returns a different socket for each established client connection.
The listening socket remains responsible for accepting new connections. Each
accepted socket is explicitly made nonblocking and placed in a client slot.

The CLI creates its own nonblocking socket and calls `connect()`. Connection
establishment may still be in progress, reported as `EINPROGRESS`. It waits for
writability with `poll()` and checks `SO_ERROR` before treating the connection
as successful. Writability alone does not establish that connection succeeded.

The socket address's port uses `htons()`. That conversion belongs to the POSIX
socket-address API; the protocol header encoder separately handles the header's
byte order. Numeric IPv4 addresses are parsed with `inet_pton()`.

## The coordinator event loop

The single-threaded coordinator uses `poll()` to wait for activity on the
listener and up to 64 clients. A client has two phases:

```text
READING_MESSAGE -> WRITING_REPLY -> READING_MESSAGE -> ...
READING_MESSAGE -> record heartbeat -> READING_MESSAGE -> ...
```

Each client owns 16-byte input and output buffers, received/sent byte counters,
the expected input size, the actual output size, its phase, and a monotonic
timestamp of its last byte-transfer progress. It also stores its assigned
worker ID, or zero if the connection has not registered.

In the read phase, the loop requests `POLLIN`. A `recv()` call asks for only the
bytes still needed for the header. Positive results advance the receive counter.
Once 12 bytes are present, the complete-message decoder either returns an empty
message, rejects the frame, or reports that a valid payload is incomplete. For
a payload-bearing frame the coordinator raises the expected size to 16 and
reads only the remaining four bytes before decoding again.

PING queues PONG. WORKER_REGISTER adds an ALIVE registry entry with a fresh ID
and queues an ACK carrying that ID. A valid HEARTBEAT updates only the sending
worker's registry timestamp and resets the input state without queuing a reply.
Duplicate registration and incorrect heartbeat IDs close the offending connection.

In the write phase, the loop requests `POLLOUT`. A `send()` call uses the remaining
part of the reply buffer and advances the sent counter by the actual result.
After the actual reply length (12 for PONG, 16 for registration ACK) has been
accepted by the local socket, the client returns to reading a new header.
The `pong_sent` and `worker_register_ack_sent` logs record local send completion,
not proof that the remote application received the response.

The loop processes one read or write per ready client per iteration. It accepts
at most 64 connections per iteration and closes excess connections when all
slots are occupied. A client with a partial header therefore cannot hold the
loop waiting for its next byte. The coordinator does not call the CLI's
complete-transfer helpers inside this shared event loop because those helpers
wait for one connection's whole operation.

If several PINGs arrive together, the coordinator reads exactly one header,
responds, and then reads the next. Later bytes remain in the kernel's receive
buffer. The same rule preserves frame boundaries for registration and heartbeat
payloads. Larger job payloads will require extending the bounded input storage.

## Partial I/O and errors

TCP delivers an ordered byte stream. A `recv()` may return fewer bytes than
requested, and a `send()` may accept only part of the supplied buffer. Neither
operation defines application-message boundaries.

The CLI uses `faultline_send_all()` and `faultline_recv_exact()` to repeat those
operations while retaining the offset already transferred. Both require
nonblocking sockets and use `poll()` between attempts. `EINTR` means a signal
interrupted an operation; `EAGAIN` or `EWOULDBLOCK` means it would currently block.
These conditions are retried without losing progress.

Receive results distinguish a complete read, clean EOF before any bytes,
truncation after some bytes, and a system error. Receiving zero bytes from a
nonzero-length `recv()` request means EOF. It does not mean "try again later."

A `POLLHUP` event may accompany buffered input. The coordinator still attempts
the relevant I/O, so a client that sends PING and then shuts down its write half
can receive PONG. A reset or broken connection closes that client alone.

Both executables ignore SIGPIPE so a send to a closed connection produces an
error that code can handle, rather than terminating the process. This does not
hide the send error. Socket setup failures close any newly created descriptor,
and the CLI closes its connection on both success and failure.

## Deadlines

The CLI allows five seconds each for connecting, sending the whole header, and
receiving the whole response. Each operation keeps one deadline across retries
and partial progress. These are separate operation budgets, not a five-second
budget for the entire invocation.

The coordinator applies a five-second inactivity limit to unregistered clients,
partial incoming messages, and queued replies, measured since acceptance or the
last successful read/write. The poll loop checks this approximately once per
second when idle, so a stalled operation usually closes within five to six
seconds. Successful byte transfers refresh this I/O timer.

Registered workers waiting between complete messages are exempt from this I/O
timer. Their separate registry timestamp changes only at registration or after
a valid heartbeat. A future configurable heartbeat timeout will use that
timestamp; it is not enforced yet. PINGs and partial messages cannot count as
heartbeats. A connected worker that stops sending all data currently stays ALIVE
until its connection closes; silence-based failure detection is still required.

All elapsed-time calculations use `CLOCK_MONOTONIC`, which avoids wall-clock
adjustments affecting timeouts.

## Current boundary and tests

PING, PONG, and WORKER_REGISTER require empty payloads. Registration ACK and
HEARTBEAT require exactly four ID bytes. Incorrect types, malformed frames,
wrong lengths, and invalid registration/heartbeat state close the affected
connection without an error frame. Every registered-client close path marks its
worker DEAD and detaches the descriptor before calling `close()`, including
EOF, I/O errors, protocol rejection, stalled transfers, and coordinator shutdown.

The coordinator currently listens only on IPv4 loopback. The CLI accepts a
numeric IPv4 address. There is no hostname resolution, IPv6, automatic worker
heartbeat sending, missed-heartbeat detection, job execution, or persistence yet.
Logs provide basic event
visibility; full timestamped structured logging remains future work.

`make test` runs protocol unit tests, socket unit tests, and TCP integration
tests. `make test-sanitize` instruments the C binaries for the same checks. The
test guide in `tests/README.md` explains the cases and the optional port 9000 run.
