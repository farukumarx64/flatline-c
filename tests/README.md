# Tests

Run all tests from the project root:

```sh
make test
make test-sanitize
```

These run protocol, registry, job, queue, and socket unit tests written in C plus process integration
tests using Python 3's standard library. A loopback-capable environment is
required. You can select the Python interpreter with `PYTHON=/path/to/python3`.
Use `make test-unit` or `make test-integration` to run one layer separately.
Use `make test-failures` for the dedicated failure-detection suite, or
`make SANITIZE=1 test-failures` for the same checks against instrumented binaries.

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

`test_messages.c` adds seven complete-message test groups:

- Literal complete frames for PING, PONG, WORKER_REGISTER, WORKER_REGISTER_ACK,
  and HEARTBEAT, including exact payload lengths, unaligned buffers, and guards.
- Worker IDs 1, 12, `0x01020304`, and `UINT32_MAX` in ACK and HEARTBEAT payloads.
- Every incomplete prefix of each frame and every insufficient output capacity,
  including 12 through 15 bytes of messages carrying a 4-byte ID. Truncated inputs
  use exact-sized allocations for ASan; failed calls preserve outputs and counts.
- Wrong declared payload lengths, oversized payloads, zero IDs, and invalid
  headers. Wrong lengths fail before waiting for any payload bytes.
- Invalid encoder message types and IDs without partial writes.
- Null required pointers with unchanged remaining outputs.
- Consecutive registration and ACK frames followed by a partial heartbeat,
  checking byte-consumption counts and decoding again as the remaining ID bytes
  arrive. This is a buffer-level test, not a live worker registration exchange.

`test_job_messages.c` adds eight job codec test groups:

- Literal complete frames for all six job types, including unaligned 64-bit
  fields, exact lengths, and untouched surrounding bytes.
- Every incomplete prefix and insufficient capacity for each sample frame and
  the maximum 1062-byte assignment; exact allocations expose overreads to ASan.
- Empty, 1/255/256/1024-byte binary data, embedded zeroes, decoded ownership after
  receive-buffer reuse, and rejection of 1025-byte/`SIZE_MAX` host lengths.
- All task IDs, retry limits zero/`UINT32_MAX`, job/worker ID boundaries, and
  64-bit attempts above `UINT32_MAX` through `UINT64_MAX`.
- Zero identity fields, unknown tasks, and invalid failure codes, including a
  worker trying to report the coordinator-only WORKER_LOST reason.
- Too-short/too-large outer sizes and inconsistent or oversized inner lengths,
  rejected from the available prefix without waiting for data.
- Coalesced job messages and a heartbeat followed by a fragmented final report.
- A decoded STARTED report accepted by the model, then rejected as stale after
  reassignment to the same worker under a newer attempt number.

Rejected codec calls preserve all output bytes and written/consumed counts.
These are buffer/model tests; live job execution is not implemented. The new
binary also links the job model for the final identity check.

`test_net.c` has six socket test groups and two parsing groups. Socket
tests use local stream socket pairs and child processes to verify fragmented
receives, clean EOF versus truncation,
one total receive deadline despite progress, a 256 KiB send through a constrained
send buffer, a stalled-send deadline, and handling a disconnected peer without
SIGPIPE terminating the process. The bulk-send receiver checks every byte
independently using raw `recv()` calls.

Endpoint tests cover numeric IPv4 addresses, port boundaries, malformed inputs,
unsupported hostname/IPv6 inputs, null arguments, and insufficient host buffers.
Failed parsing leaves the host and port outputs unchanged. Both the CLI and
worker use this shared parser.

Duration tests check positive decimal milliseconds, leading zeros, `INT_MAX`,
overflow, malformed input, null pointers, and unchanged output on failure.

`test_worker_registry.c` has seven groups covering registration and lookup,
heartbeat ownership and monotonic time, disconnect and descriptor reuse,
capacity/duplicate registration/churn, ID exhaustion, invalid arguments, and
heartbeat expiration. Expiry checks cover just before, exactly at, and after a
deadline, heartbeat renewal, dead/unused records, invalid inputs, a one-millisecond
timeout, and timestamps near `INT64_MAX` without overflowing an added deadline.
These tests supply descriptor numbers and timestamps directly; no real sockets
or sleeps are needed. Reusing descriptor 7 is deliberate and deterministic.
Stale IDs cannot update or kill its replacement worker. Churn exceeds the
64-slot capacity without replacing live records; exhaustion never wraps to ID 1.

`test_jobs.c` adds nine in-memory model test groups:

- All four task identifiers, initial fields, argument ownership, zero-length
  arguments, and exact maximum-length payloads.
- A successful assignment/start/completion lifecycle, timestamp updates, and
  result ownership including embedded zero bytes and maximum-sized results.
- Every pair of the five states, plus unknown states, against an explicit graph.
- Every named operation from every state with zero and nonzero retry allowance;
  includes rejected self-transitions and terminal-state changes.
- Requeue/reset semantics, retry followed by success, assignment back to the same
  worker, and rejection of old STARTED, COMPLETED, and failure reports.
- Zero retries, exhausted retries, and failure before the worker reports starting.
- Null pointers, zero IDs, unknown tasks/failure reasons, negative time, oversized
  arguments/results (including SIZE_MAX), and unchanged records on errors.
- Wrong worker IDs, wrong attempt numbers, reversed timestamps, and equal-time events.
- Maximum job/worker IDs and timestamps, and the final retry/assignment boundary
  beyond UINT32_MAX attempts, without billions of iterations or counter wrap.

These tests need no sockets or sleeps. They verify model-level transitions only;
there is no job submission, executor, or automatic retry in the running
programs yet. The job model links into the coordinator and the job/queue/job-message test
binaries; the CLI and worker do not link its transition implementation.

`test_job_queue.c` adds seven FIFO test groups:

- Empty initialization, empty peek/pop, null arguments, and unchanged output IDs.
- Insertion order using unsorted IDs/timestamps and UINT64_MAX; push/pop leave
  the complete job records and their payloads unchanged.
- Repeated peeks while waiting and rejected assignments leave the front intact;
  successful model assignment followed by pop removes the intended ID.
- Duplicate IDs from the same or different records; rejection of assigned,
  running, done, failed, and malformed job records.
- Exactly 256 pending IDs; overflow preserves every existing entry and the
  proposed job; duplicate checking also works when full.
- Repeated half-drain/refill cycles, array wraparound, duplicate detection across
  the boundary, complete FIFO draining, and reuse after becoming empty.
- A model-level retry retains its ID and joins the back after explicit enqueue.

These tests use no sockets, sleeps, or scheduler. Most FIFO-only fixtures create
temporary job records to verify that the queue copies IDs and retains no pointers.
Application code must keep authoritative records in its own store. Queue code
links into the coordinator and its test binary; the queue tests also link the
job model to exercise assignment and retry ordering.

`integration/test_ping.py` starts the real coordinator and invokes the real CLI.
Its original 15 scenarios cover a successful exchange and sequential clients, default
port behavior, fragmented PING and PONG, repeated/coalesced frames, concurrent
clients alongside idle/partial peers, half-close handling, truncated requests,
invalid requests and replies, resets, receive timeout, connection refusal,
port conflicts, invalid arguments, and a complete frame followed by a partial
frame. Cleanup checks coordinator exit status
after SIGTERM and captures its logs for failure diagnostics.

Nine additional worker scenarios exercise the real coordinator using independent
Python TCP peers: simultaneous registered connections, fragmented registration,
all 15 two-part heartbeat splits, registration plus a coalesced PING and half-close,
70 successive registrations/disconnects, heartbeat ownership and stale IDs,
duplicate registration, truncated/invalid worker frames, and an idle registered
worker alongside a stalled payload. The stalled payload times out and marks its
worker dead; the idle worker expires under the default six-second heartbeat timeout.

One additional scenario sends all six valid job header types and the largest
variable declarations while keeping each peer open. The current 16-byte runtime
receiver must reject them before reading payload data; another registered worker
must continue heartbeating and PING/PONG must remain usable.
Logs verify state transitions and timestamp updates, including that PING
and incomplete heartbeat bytes do not count as heartbeats. Positional log reads
avoid moving the file offset shared with the coordinator's output stream.

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

`integration/test_worker.py` adds eight scenarios for the real worker executable:

- Two workers running simultaneously with distinct coordinator-issued IDs;
  stopping one leaves the other registered and the CLI usable.
- The default worker endpoint on port 9000.
- Seventeen ACK fragment patterns: one byte at a time, `3 + 5 + 4 + 4`, and all
  15 two-part splits. The worker must not report an ID before the full ACK arrives.
- All 16 incomplete ACK prefixes, malformed headers, wrong types/lengths, and
  ID zero. Invalid headers are tested while the peer remains open, proving early rejection.
- A single five-second ACK deadline across delayed header and payload fragments.
- SIGINT/SIGTERM during an incomplete ACK.
- Coordinator EOF/reset and unexpected bytes coalesced after a valid ACK.
- Help, invalid arguments, and an unavailable endpoint (which may consume the
  existing five-second connect budget instead of refusing immediately).

The worker suite also verifies decoding/printing `UINT32_MAX`. Integration
suites share coordinator setup, cleanup, and protocol helpers through the
`CoordinatorTestCase` base class; worker process helpers live in
`WorkerProcessTestCase`. Test methods remain in their own subclasses and run only once.
Worker subprocesses are always cleaned up, and their stderr is checked for
AddressSanitizer/UBSan reports.

`integration/test_heartbeat.py` contains seven timing scenarios:

- Default two-second heartbeat cadence keeps a real worker alive past six
  seconds while an open, silent registration expires after the default timeout.
- A controlled peer independently checks repeated exact 16-byte heartbeat frames
  at a 120 ms interval; a partial ACK must not start heartbeat sending.
- A valid heartbeat renews the deadline beyond the original registration deadline.
- PING traffic and trickled heartbeat header/payload bytes cannot renew liveness.
- A worker configured to send more slowly than the timeout expires before its
  first heartbeat.
- A heartbeat queued while the coordinator is paused is rejected after expiry
  when the coordinator resumes; it cannot revive the old registration.
- Invalid/missing/duplicate duration arguments, overflow, help output, and worker
  options in reverse order.

Timing assertions allow scheduling slack; exact deadline boundaries are covered
by deterministic C tests. Every paused process is resumed in a `finally` block
before normal cleanup. The heartbeat suite uses both default timings and a
shorter coordinator timeout for focused scenarios; each fixture owns its processes.

`integration/test_failure_detection.py` contains five failure scenarios:

- A registered real worker exits successfully on SIGTERM; EOF marks it dead.
- A registered real worker is terminated by SIGKILL; its exit status confirms
  forced termination and EOF marks it dead without waiting for heartbeats.
- A registered Python peer closes with zero SO_LINGER, causing a TCP reset;
  the coordinator marks its registration dead with `reason=recv_error`.
- A registered Python peer sends two valid heartbeats, then remains open without
  sending more data. The coordinator must close it after six seconds of silence.
- The real-worker SIGSTOP case, moved from the heartbeat suite, keeps two workers
  alive for multiple timeout periods, pauses one, and requires only that ID to
  expire. The paused process must still be present when detection occurs.

Transport failures must be detected within 2.5 seconds of fault injection with
no `heartbeat_timeout` event. Timeout cases check the coordinator's monotonic
`detected_at_ms` and `silence_ms` diagnostics against its last recorded heartbeat
and configured timeout. Each case verifies one death event for the failed ID,
continuing heartbeat progress by a healthy worker, and a usable CLI. Real-worker
exit/pause cases check that freed slots accept fresh IDs. The peer silence case
does not call `close()` or `shutdown()` before coordinator-initiated closure.
SIGSTOP cleanup always resumes the process before trying to stop it.

The focused command prints each detected reason and elapsed observation time or
heartbeat silence duration. The full integration target includes this suite once.

By default, suites select available ports. The original coordinator and worker
suites each skip one default-endpoint check. To run all 44 scenarios, stop any existing
coordinator on port 9000 and run:

```sh
make test-integration INTEGRATION_ARGS='--port 9000'
# Or run the complete sanitizer suite, including the default endpoints:
make test-sanitize INTEGRATION_ARGS='--port 9000'
```

When 9000 is selected, the harness starts the coordinator without `--port` and
also invokes `faultline ping` and `faultline-worker` without `--coordinator`
to test all defaults. Processes started by the harness are stopped afterward.
The coordinator, CLI, and worker
are not yet tested for job execution or persistent recovery;
those features will add their own integration scenarios.
