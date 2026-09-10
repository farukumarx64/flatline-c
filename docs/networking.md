# The first TCP exchange

Faultline can now start a coordinator on `127.0.0.1:9000`, accept a CLI connection,
receive a binary PING header, and send a binary PONG header. This milestone
establishes working transport before worker registration and job scheduling.

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

The coordinator logs listening, connection, PING, and PONG events. The `fd`
field is the operating system's connection descriptor, which can be reused
after closing a connection. It is not a worker ID. Ctrl+C or SIGTERM requests
shutdown, closes all client sockets and the listener, and exits the event loop.

## Files and responsibilities

| File | Responsibility |
| --- | --- |
| `include/net.h` | Networking API, defaults, and receive result codes. |
| `src/common/net.c` | Socket setup, port parsing, readiness waits, deadlines, and complete-transfer helpers. |
| `src/coordinator/main.c` | Listener, client slots, poll loop, PING validation, and queued PONG writes. |
| `src/cli/main.c` | CLI arguments, one connection, request transmission, and response validation. |

`protocol.h` and `protocol.c` still own the 12-byte wire format. Networking uses
their encoder and decoder; it does not send raw C structs.

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
READING_PING -> WRITING_PONG -> READING_PING -> ...
```

Each client owns a 12-byte input buffer, a 12-byte output buffer, received/sent
byte counters, its phase, and a monotonic timestamp of its last progress.

In the read phase, the loop requests `POLLIN`. A `recv()` call asks for only the
bytes still needed for that header. Positive results advance the receive
counter. A complete header is decoded, checked for type PING and zero payload,
and used to encode the outgoing PONG. The phase then changes to writing.

In the write phase, the loop requests `POLLOUT`. A `send()` call uses the remaining
part of the PONG buffer and advances the sent counter by the actual result.
After all 12 bytes have been accepted by the local socket, the client returns
to reading. The `pong_sent` log records that local send completion, not proof
that the remote application has received the response.

The loop processes one read or write per ready client per iteration. It accepts
at most 64 connections per iteration and closes excess connections when all
slots are occupied. A client with a partial header therefore cannot hold the
loop waiting for its next byte. The coordinator does not call the CLI's
complete-transfer helpers inside this shared event loop because those helpers
wait for one connection's whole operation.

If several PINGs arrive together, the coordinator reads exactly one header,
responds, and then reads the next. Later bytes remain in the kernel's receive
buffer. This works for the current fixed-size, empty-payload messages; job
payloads will need more parser states.

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

The coordinator applies a five-second inactivity limit per connection, measured
since acceptance or the last successful read/write. The poll loop checks this
at least approximately once per second when idle, so an inactive connection
usually closes within five to six seconds. Successful byte transfers refresh
this inactivity timer. It is not yet worker heartbeat detection.

All elapsed-time calculations use `CLOCK_MONOTONIC`, which avoids wall-clock
adjustments affecting timeouts.

## Current boundary and tests

PING and PONG require empty payloads. Incorrect types, malformed headers, and
nonzero payload declarations close the affected connection without an error
frame. The shared header codec still supports bounded length values for future
message types.

The coordinator currently listens only on IPv4 loopback. The CLI accepts a
numeric IPv4 address. There is no hostname resolution, IPv6, worker registry,
heartbeat protocol, job execution, or persistence yet. Logs provide basic event
visibility; full timestamped structured logging remains future work.

`make test` runs protocol unit tests, socket unit tests, and TCP integration
tests. `make test-sanitize` instruments the C binaries for the same checks. The
test guide in `tests/README.md` explains the cases and the optional port 9000 run.
