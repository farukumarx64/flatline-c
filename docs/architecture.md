# Faultline MVP architecture

Faultline will distribute independent jobs across worker processes and recover
unfinished work when a worker fails. The v0.1 goal is a small C11 system whose
networking, scheduling, failure handling, and recovery behavior can be explained
and demonstrated with reproducible experiments.

This note describes the intended MVP. The current implementation supports a
tested TCP PING/PONG exchange between the CLI and a coordinator using `poll()`.
The coordinator also registers workers, returns assigned IDs, validates heartbeat
ownership, and records connection state and heartbeat times in a worker registry.
Disconnected or timed-out workers are marked dead. The worker executable connects,
registers, validates its assigned ID, and sends periodic heartbeats. Worker interval
and coordinator timeout are configurable, defaulting to two and six seconds.
The [job model](jobs.md) defines records and validated state transitions, including
attempt identity and bounded requeue/failure rules. The [FIFO queue](queue.md)
holds pending IDs in insertion order independently of full job records. The
[job message codec](job-protocol.md) defines submission, acknowledgment, assignment,
started, completed, and failed payloads. Runtime job transport/handlers, the job
store, scheduling, execution, automatic retries, and recovery remain future work.

## Components and ownership

```text
CLI client ── submit and query over TCP ──> Coordinator
                                              │
                                     assign jobs over TCP
                                              │
                                      ┌───────┴───────┐
                                   Worker A        Worker B
                                      │               │
                                      └── heartbeats ─┘
                                          and results
                                      to coordinator
```

The **CLI** submits a supported task and its arguments, receives a job ID, and
queries jobs, workers, and statistics. It does not decide which worker runs a job.

The **coordinator** owns the authoritative job state, FIFO queue, worker registry,
and current assignments. It gives the oldest queued job to an idle worker,
monitors worker liveness, applies retry limits, and records durable job state
transitions in an append-only write-ahead log (WAL).

Each **worker** connects to the coordinator, registers for an ID, sends periodic
heartbeats, and executes one assigned job at a time. It reports job start,
completion, or failure. Workers may run on the same computer or across a LAN.
Built-in tasks are `sleep`, `prime_count`, `fibonacci`, and `hash`.

## Job lifecycle and recovery

The normal lifecycle is `QUEUED → ASSIGNED → RUNNING → DONE`. An assignment is a
lease: the coordinator may revoke it when the worker disconnects or its
heartbeat times out. Unfinished jobs then return to `QUEUED` while retry allowance
remains; exhausted jobs enter the terminal `FAILED` state. Reported task errors
also follow the retry policy. Retrying starts the task again from the beginning.

The WAL records job creation and meaningful state transitions. After a
coordinator restart, replay reconstructs job state, preserves terminal outcomes,
and makes queued work available again. Previously assigned or running jobs have
uncertain outcomes and must be handled by the recovery and retry policy.

## Guarantees and limits

- Multiple workers can execute independent jobs concurrently.
- Disconnects and missed heartbeats make workers unavailable for scheduling;
  unfinished work is automatically retried within a configured limit.
- Execution uses at-least-once semantics. If a completion message is lost or a
  worker is suspected dead while still executing, a task may execute more than
  once. Tasks should be safe to repeat. Retry limits can still produce terminal
  failure; successful completion is not unconditional.
- Durable job state must survive coordinator process restart. The exact point
  at which submission is acknowledged as durable must be defined with the WAL
  write and flush policy before this guarantee is claimed by the implementation.
- There is one coordinator and no automatic failover. Scheduling and queries
  are unavailable while it is down. Existing worker computations may continue,
  and restart recovery must account for their uncertain outcomes.

A heartbeat timeout is evidence of unavailability, not proof that a process has
stopped. An expired assignment can overlap with a later retry. The implementation
must distinguish attempts so a late result from an older attempt cannot overwrite
the authoritative state of a newer one.

The MVP excludes coordinator consensus, exactly-once execution, task
checkpointing, arbitrary shell execution, a web dashboard, Kubernetes
integration, and production authentication or TLS.

## Implementation direction

Use C11, POSIX TCP sockets, and `poll()` for portable event multiplexing on macOS
and Linux, with pthreads where necessary. TCP messages require an explicit wire
format and buffering for partial reads and writes; raw C structs must not be sent
as the protocol. The implemented 12-byte header and its encoding are documented
in [the protocol specification](protocol.md). PING/PONG, registration, and
heartbeat handlers support partial headers and the fixed worker ID payload; see
[the networking walkthrough](networking.md). The coordinator's
[worker registry](workers.md) owns IDs independently of reusable socket descriptors.
The shared message representation supports bounded variable job payloads.
Runtime job handling will require extending the current 16-byte transport buffers
and dispatch logic; recognized job headers are safely rejected for now.

Before job execution and persistence are implemented, resolve how workers keep
sending heartbeats during long computations, how job/attempt IDs and retry
counters are preserved across restarts, and how WAL writes,
acknowledgments, and incomplete trailing records are handled.

## Evidence required for v0.1

Demonstrate multiple workers completing independent jobs, heartbeat-based failure
detection, reassignment after killing a busy worker, and state recovery after
restarting the coordinator. Unit tests, integration tests, controlled failure
experiments, and worker churn tests should show that no accepted durable job
silently disappears. Benchmark 1, 2, 4, and 8 workers and report the workload,
machine specifications, throughput, latency, and recovery costs.
