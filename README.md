# Faultline

Faultline is a distributed job execution engine being built in C11. Its planned
MVP distributes independent jobs across workers, detects worker failures,
retries interrupted work, and recovers coordinator state after a restart.

The coordinator and CLI now exchange framed PING/PONG messages over TCP. The
coordinator handles multiple clients with nonblocking sockets and `poll()`, and
the shared networking code handles partial transfers and deadlines. Protocol
unit tests, socket tests, and process integration tests cover the exchange.
The worker remains a scaffold; job execution and persistence are future work.

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

Both programs default to `127.0.0.1:9000`, so `faultline-coordinator` and
`faultline ping` also work without address options. Use the executable paths
above unless you have added their directory to PATH. Stop the coordinator with
Ctrl+C. It closes active connections and its listening socket before exiting.

The coordinator currently binds only to IPv4 loopback. The CLI accepts numeric
IPv4 addresses and ports from 1 through 65535. Hostname resolution, IPv6, and a
coordinator bind-address option are not implemented yet.

Warnings are enabled for common defects, conversions, shadowed variables,
function prototypes, and format strings. Dependency files ensure changes to
included headers trigger recompilation.

Build with AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
make sanitize
```

Run the same two-terminal example using `build/sanitize/` in place of
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

Integration tests normally choose an available loopback port, leaving the
default-endpoint check skipped. To also exercise both programs' port 9000
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
│   └── networking.md
├── include/             Shared C headers
├── src/
│   ├── common/          Shared protocol, networking, and logging code
│   ├── coordinator/     Coordinator entry point and future implementation
│   ├── worker/          Worker entry point and future implementation
│   └── cli/             Client entry point and future implementation
└── tests/               Protocol/socket unit tests and TCP integration tests
```

Read [the architecture note](docs/architecture.md) for component responsibilities,
MVP guarantees, and design decisions still to be resolved. Read
[the protocol specification](docs/protocol.md) for byte offsets, network byte
order, validation rules, and the C API. The [networking walkthrough](docs/networking.md)
explains the PING/PONG exchange, connection state, partial I/O, and deadlines.
Worker registration and heartbeats are the next major milestone.
