# Worker crash recovery

The first fault-tolerance acceptance check kills a real busy worker with
SIGKILL and verifies that another already-connected worker completes its job.
The coordinator, both workers, and the submission CLI are the actual executables.

## Run the check

```sh
make test-recovery
make SANITIZE=1 test-recovery
```

The target builds the executables and runs `tests/integration/test_recovery.py`.
It selects an available loopback port, starts its own coordinator and workers,
captures their logs, and cleans up the processes afterward. It sends SIGKILL
only to the child process identified as the running job's owner.

The check is also included in `make test-integration`, `make test`, and
`make test-sanitize`. The existing `INTEGRATION_ARGS='--port 9000'` option works
when that port is available.

## What the test does

1. Start two workers with 100 ms heartbeat intervals. Wait for both registrations
   and a heartbeat from each before submitting any work.
2. Submit `sleep --args 3000 --max-retries 1` through the CLI.
3. Wait until the coordinator records RUNNING for attempt 1. Read its worker ID
   to identify the owner; the test does not assume launch order determines ownership.
4. Verify the other worker is still idle, then observe another heartbeat from
   the owner after STARTED. Check that the job has not completed.
5. Send SIGKILL to the owner and verify its process exits because of that signal.
6. Verify the coordinator releases and requeues the unfinished job exactly once.
7. Wait for the already-connected worker to start the same job under attempt 2.
8. Verify DONE with `result="slept_ms=3000"`, the survivor's worker ID, and one retry.
9. Submit `fibonacci --args 10`. Verify that the survivor also completes this new
   job with result `55`, continues heartbeating, and the CLI still receives PONG.

The test waits for specific observed transitions with bounded deadlines, rather
than using a fixed pause to guess when it is safe to kill the worker. Only complete
log lines are parsed. Attempt-aware waits ensure a previous STARTED event cannot
be mistaken for the start of the retry.

## Why SIGKILL matters

SIGTERM and Ctrl+C allow the worker's signal handler to request shutdown. Its
ordinary cleanup then cancels and joins the task thread and closes the socket.
The execution suite already covers that cooperative path.

SIGKILL terminates the process without running that cleanup. The operating system
closes its sockets, and the coordinator observes EOF or a transport error. The
failed worker cannot send a task-failure report or put its own job back in the
queue. Recovery must come from the coordinator's existing record of ownership.

The coordinator retains its default six-second heartbeat timeout in this check.
The test requires the survivor's attempt-2 STARTED report within 2.5 seconds of
signalling the crash, verifies a transport-related death reason, and rejects a
heartbeat-timeout event for that worker. This establishes prompt disconnect-based
recovery. The bound allows local scheduling slack; it is not a network latency SLA.

## Expected state changes

Let J be the submitted job, A its original worker, and B the connected survivor:

| Event | Job state | Owner | Attempt | Retry count |
| --- | --- | --- | --- | --- |
| Submission | QUEUED | None | 0 | 0 |
| First assignment | ASSIGNED | A | 1 | 0 |
| First start | RUNNING | A | 1 | 0 |
| A is lost | QUEUED | None | 1 | 1 |
| Reassignment | ASSIGNED | B | 2 | 1 |
| Retry starts | RUNNING | B | 2 | 1 |
| Result accepted | DONE | B | 2 | 1 |

The job ID stays J throughout. The retry counter increases when the job is
requeued, and the attempt number increases when it is assigned again. A retry
runs the task from the beginning: B performs the full three-second sleep.

The test compares this complete transition sequence after the follow-up job
finishes. It also checks one recorded death for A, no death for B before cleanup,
and one registration for B. B keeps its original process and identity; no
replacement worker is launched after the crash. Missing or duplicate transitions
for J fail the check.

## Existing implementation exercised

The acceptance check required no changes to the C recovery implementation:

- `src/coordinator/main.c`: connection cleanup finds the worker's active job,
  invokes worker-loss handling, marks the registration dead, and closes the socket.
- `src/coordinator/scheduler.c`: `faultline_scheduler_worker_lost()` applies
  WORKER_LOST to the active job and publishes the resulting queue/state update.
- `src/coordinator/job.c`: the model enforces the retry allowance, clears queued
  ownership, and increments the attempt on the next assignment.
- The scheduler selects the oldest queued job for an eligible idle worker.
- The survivor executes the task and sends its normal STARTED/COMPLETED reports.

The existing [scheduling guide](scheduling.md) explains validation and queue
ownership; [task execution](tasks.md) explains the worker's computation and reporting.

## Scope of this step

This verifies hard-crash recovery while the coordinator remains alive. Jobs and
results are still in memory; coordinator restart recovery needs the future WAL.
The system retains at-least-once semantics: this test asserts one accepted result
for its scenario, not a guarantee that every task executes only once.

The next separate checks are heartbeat expiry during a real running job,
protection against an old attempt after a paused worker resumes, and repeated
worker losses exhausting the retry allowance. Existing tests cover parts of
those mechanisms; their combined fault-tolerance scenarios remain later steps.
