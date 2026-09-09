# Tests

Run the protocol unit tests from the project root:

```sh
make test
make test-sanitize
```

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

These tests exercise the header layer only. TCP fragmentation, multiple frames
in one receive buffer, and connection closure during a frame require the future
transport implementation. Later integration tests will exercise job execution,
worker failure, and coordinator recovery.
