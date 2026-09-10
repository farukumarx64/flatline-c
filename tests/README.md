# Tests

Run all tests from the project root:

```sh
make test
make test-sanitize
```

These run protocol and socket unit tests written in C plus process integration
tests using Python 3's standard library. A loopback-capable environment is
required. You can select the Python interpreter with `PYTHON=/path/to/python3`.
Use `make test-unit` or `make test-integration` to run one layer separately.

`test_protocol.c` has seven test groups:

- Exact encoded and decoded bytes for PING and PONG, using literal reference
  headers so matching bugs in both functions cannot hide behind a round trip.
- Payload-length round trips at byte boundaries and the exact 1 MiB maximum.
- Every incomplete header size from 0 through 11 bytes, checking that output
  stays unchanged. Truncated decode inputs use exact-sized allocations so ASan
  can detect out-of-bounds reads.
- Malformed magic, unsupported versions, unknown message types, excessive
  payload lengths, and a header encoded in the wrong byte order.
- Invalid encoder field values, including maximum integer values, without
  modifying the destination buffer.
- Null argument handling.
- Header reads and writes at an unaligned address, preservation of surrounding
  bytes, and acceptance of buffers with trailing data.

The test program exits unsuccessfully at the first failed check and reports the
test, source line, and expression. Its checks remain active with `NDEBUG` set.

`test_net.c` has six socket test groups. They use local stream socket pairs and
child processes to verify fragmented receives, clean EOF versus truncation,
one total receive deadline despite progress, a 256 KiB send through a constrained
send buffer, a stalled-send deadline, and handling a disconnected peer without
SIGPIPE terminating the process. The bulk-send receiver checks every byte
independently using raw `recv()` calls.

`integration/test_ping.py` starts the real coordinator and invokes the real CLI.
Its 15 scenarios cover a successful exchange and sequential clients, default
port behavior, fragmented PING and PONG, repeated/coalesced frames, concurrent
clients alongside idle/partial peers, half-close handling, truncated requests,
invalid requests and replies, resets, receive timeout, connection refusal,
port conflicts, invalid arguments, and a complete frame followed by a partial
frame. Cleanup checks coordinator exit status
after SIGTERM and captures its logs for failure diagnostics.

Fragmentation checks cover 13 delivery patterns in each direction: one byte at
a time, an uneven `3 + 5 + 4` split, and all 11 possible two-piece splits of a
12-byte header. After every non-final fragment, the test withholds the suffix
and checks that the receiver neither replies nor closes the connection. This
tests pending input without assuming that separate sends map to separate TCP
packets or receive calls. Coordinator cases reuse the connection to check that
receive state resets between frames.

Disconnect cases cover every incomplete header length from 0 through 11 bytes
for both requests and responses. Another case sends a complete PING plus five
bytes of the next PING together, expects exactly one PONG, and completes the
second header in two more pieces before checking a third exchange.

By default, the integration harness selects an available port and skips the
specific default-endpoint check. To run all 15 scenarios, stop any existing
coordinator on port 9000 and run:

```sh
make test-integration INTEGRATION_ARGS='--port 9000'
# Or run the complete sanitizer suite, including the default endpoints:
make test-sanitize INTEGRATION_ARGS='--port 9000'
```

When 9000 is selected, the harness starts the coordinator without `--port` and
also invokes `faultline ping` without `--coordinator` to test both defaults.
Processes started by the harness are stopped afterward. The coordinator and CLI
are not yet tested for job execution, worker failure, or persistent recovery;
those features will add their own integration scenarios.
