# Faultline

Faultline is a distributed job execution engine being built in C11. Its planned
MVP distributes independent jobs across workers, detects worker failures,
retries interrupted work, and recovers coordinator state after a restart.

The coordinator and CLI now exchange framed PING/PONG messages over TCP. The
coordinator handles multiple clients with nonblocking sockets and `poll()`, and
the shared networking code handles partial transfers and deadlines. Protocol
unit tests, socket tests, and process integration tests cover the exchange.
The coordinator also accepts worker registration, assigns IDs, and tracks each
worker's connection, liveness state, and last heartbeat time in a bounded registry.
It checks incoming heartbeat IDs against their connections and marks workers
dead on disconnect or heartbeat expiry. The worker executable connects, registers,
prints its assigned ID, and sends a heartbeat every two seconds. The coordinator
expires a worker after six seconds without a valid heartbeat. Both durations are
configurable. Job execution and persistence are future work.

Dedicated failure tests distinguish worker exit and TCP reset from missed
heartbeats on an open connection. A healthy worker and the CLI must remain usable
in every case; timeout logs report the measured silence duration.

## Build and run

Requirements: Make and a C11 compiler such as Clang or GCC. The project targets
macOS and Linux. Sanitizer builds also require the compiler's AddressSanitizer
and UndefinedBehaviorSanitizer runtimes. The integration tests require Python 3
(standard library only).

```sh
make
```

Start the coordinator in one terminal:

```sh
./build/debug/faultline-coordinator --port 9000
```

Then send a PING from a second terminal:

```sh
./build/debug/faultline ping --coordinator 127.0.0.1:9000
# PONG
```

All three programs default to `127.0.0.1:9000`, so `faultline-coordinator`,
`faultline ping`, and `faultline-worker` work without address options. Use the executable paths
above unless you have added their directory to PATH. Stop the coordinator with
Ctrl+C. It closes active connections and its listening socket before exiting.

With the coordinator running, start a worker in each of two additional terminals:

```sh
./build/debug/faultline-worker
```

Each prints its assigned ID and stays running. On a fresh coordinator, the first
two registrations get IDs 1 and 2 (process scheduling determines which gets 1):

```text
[INFO] worker registered worker_id=1 coordinator=127.0.0.1:9000 heartbeat_interval_ms=2000
[INFO] worker registered worker_id=2 coordinator=127.0.0.1:9000 heartbeat_interval_ms=2000
```

Use `./build/debug/faultline-worker --coordinator 127.0.0.1:9000` to specify an
endpoint. Ctrl+C or SIGTERM stops the worker and closes its socket. Unexpected
coordinator disconnection makes the worker report an error and exit; automatic
reconnection is not implemented yet.

To change the heartbeat timings, start the programs with these options:

```sh
# Coordinator terminal: expire after 3 seconds without a valid heartbeat
./build/debug/faultline-coordinator --port 9000 --heartbeat-timeout-ms 3000

# Worker terminal: send every 1 second
./build/debug/faultline-worker --coordinator 127.0.0.1:9000 --heartbeat-interval-ms 1000
```

Durations are positive decimal milliseconds. Configure the timeout comfortably
above every worker's interval; the separate processes do not negotiate these
values. The [worker guide](docs/workers.md) explains timing, timeout logs, and a
pause/resume experiment that demonstrates failure detection with an open socket.

The coordinator currently binds only to IPv4 loopback. The CLI and worker accept numeric
IPv4 addresses and ports from 1 through 65535. Hostname resolution, IPv6, and a
coordinator bind-address option are not implemented yet.

Warnings are enabled for common defects, conversions, shadowed variables,
function prototypes, and format strings. Dependency files ensure changes to
included headers trigger recompilation.

Build with AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
make sanitize
```

Run the same examples using `build/sanitize/` in place of
`build/debug/`.

Debug symbols and frame pointers make sanitizer reports easier to investigate.
Sanitizer builds stop on detected undefined behavior. Normal and sanitizer
outputs live in separate directories. Use `make clean` to remove both; also
clean before changing compilers or flags within the same build configuration.

## Test the protocol and TCP exchange

```sh
make test
make test-sanitize
```

Both commands build and run C unit tests and Python integration tests against
the real executables. `test-sanitize` instruments all C programs under test.
Use `make test-unit` or `make test-integration` to run either layer separately.
Use `make test-failures` to run only the five failure-detection scenarios, or
`make SANITIZE=1 test-failures` to run them with AddressSanitizer/UBSan.

Integration tests normally choose an available loopback port, leaving the
default-endpoint checks skipped. To also exercise all three programs' port 9000
defaults, first stop any existing coordinator and run:

```sh
make test-integration INTEGRATION_ARGS='--port 9000'
```

See [the test guide](tests/README.md) for coverage and failure diagnostics.

## Project layout

```text
faultline/
├── Makefile
├── docs/
│   ├── architecture.md
│   ├── protocol.md
│   ├── networking.md
│   └── workers.md
├── include/             Shared C headers
├── src/
│   ├── common/          Shared protocol, networking, and logging code
│   ├── coordinator/     Event loop and worker registry
│   ├── worker/          Worker registration and periodic heartbeats
│   └── cli/             Client entry point and future implementation
└── tests/               Protocol/registry/socket unit tests and TCP integration tests
```

Read [the architecture note](docs/architecture.md) for component responsibilities,
MVP guarantees, and design decisions still to be resolved. Read
[the protocol specification](docs/protocol.md) for byte offsets, network byte
order, validation rules, and the C API. The [networking walkthrough](docs/networking.md)
explains the PING/PONG exchange, connection state, partial I/O, and deadlines.
The [worker guide](docs/workers.md) describes the registration exchange, worker
IDs, connection ownership, and heartbeat timing. The next phase introduces job
messages, queued work, and assignment to available workers.
