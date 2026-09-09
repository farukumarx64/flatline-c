# Faultline

Faultline is a distributed job execution engine being built in C11. Its planned
MVP distributes independent jobs across workers, detects worker failures,
retries interrupted work, and recovers coordinator state after a restart.

The current foundation contains an architecture note, a build system, and three
executable entry points. Each executable only prints a scaffold message.
Networking, job execution, and persistence are not implemented yet.

## Build and run

Requirements: Make and a C11 compiler such as Clang or GCC. The project targets
macOS and Linux. Sanitizer builds also require the compiler's AddressSanitizer
and UndefinedBehaviorSanitizer runtimes.

```sh
make
./build/debug/faultline-coordinator
./build/debug/faultline-worker
./build/debug/faultline
```

Warnings are enabled for common defects, conversions, shadowed variables,
function prototypes, and format strings. Dependency files ensure changes to
included headers trigger recompilation.

Build and run with AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
make sanitize
./build/sanitize/faultline-coordinator
./build/sanitize/faultline-worker
./build/sanitize/faultline
```

Debug symbols and frame pointers make sanitizer reports easier to investigate.
Sanitizer builds stop on detected undefined behavior. Normal and sanitizer
outputs live in separate directories. Use `make clean` to remove both; also
clean before changing compilers or flags within the same build configuration.

## Project layout

```text
faultline/
├── Makefile
├── docs/
│   └── architecture.md
├── include/             Shared C headers
├── src/
│   ├── common/          Shared protocol, networking, and logging code
│   ├── coordinator/     Coordinator entry point and future implementation
│   ├── worker/          Worker entry point and future implementation
│   └── cli/             Client entry point and future implementation
└── tests/               Future unit and integration tests
```

Read [the architecture note](docs/architecture.md) for component responsibilities,
MVP guarantees, and design decisions still to be resolved. The next milestone is
a coordinator and client exchanging framed `PING`/`PONG` messages over TCP,
including correct handling of partial reads and writes.
