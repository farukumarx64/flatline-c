# Worker failure recovery

Four fault-tolerance acceptance checks interrupt a real busy worker and verify
that another already-connected worker completes its job. SIGKILL exercises
connection-loss recovery; SIGSTOP exercises heartbeat expiry while the worker
process remains paused and keeps its socket open until the coordinator closes it.
Two further scenarios resume that expired worker while its retry is running or
after its retry has completed, checking that the current job remains unchanged.
The coordinator, both workers, and the submission CLI are the actual executables.

## Run the checks

```sh
make test-recovery
make SANITIZE=1 test-recovery
```

The target builds the executables and runs `tests/integration/test_recovery.py`.
It selects an available loopback port, starts its own coordinator and workers,
captures their logs, and cleans up the processes afterward. Each scenario uses
a fresh coordinator. Signals target only the test's own worker child processes.

All four checks are also included in `make test-integration`, `make test`, and
`make test-sanitize`. The existing `INTEGRATION_ARGS='--port 9000'` option works
when that port is available.

## Hard-crash scenario

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

## Heartbeat-expiry scenario

The second test registers two workers with 100 ms heartbeat intervals and uses
the default 6000 ms coordinator timeout. It submits another three-second sleep
with one retry, identifies its owner from the first RUNNING report, and waits
for another heartbeat from that owner before sending SIGSTOP.

SIGSTOP suspends the entire process, including its networking and task threads.
The process has not exited, and its socket is not closed by the pause. No
application signal handler needs to run. The coordinator receives no special
message saying that the worker was paused; it must detect heartbeat silence.

The test verifies these stages:

1. **The worker is actually stopped.** `os.waitpid()` with `WUNTRACED` observes
   the operating system's stopped status and confirms the stop signal was SIGSTOP.
   `WNOHANG` and a deadline keep the status check from blocking indefinitely.
2. **No immediate reassignment.** For an observation window before expiry, the
   original job remains RUNNING on attempt 1, with no worker-death, timeout,
   requeue, or second assignment event. The paused process is still present.
3. **The rest of the system remains responsive.** The idle worker keeps sending
   heartbeats, and the CLI can receive PONG while the owner is suspended.
4. **Heartbeat expiry causes worker loss.** Exactly one timeout and one death
   are recorded with reason `heartbeat_timeout`. The observed silence must be
   at least 6000 ms and below 7500 ms, allowing local scheduling slack. The test
   checks `silence_ms = detected_at_ms - last_heartbeat_ms`, the matching socket
   descriptor, and the timestamp of the last accepted heartbeat.
5. **The survivor completes the retry.** The same job ID is requeued once and
   assigned to the connected idle worker with attempt 2 and retry count 1. Its
   result is `slept_ms=3000`. It then completes a new Fibonacci job with result 55.
6. **The original worker stays paused.** It sends no terminal report and remains
   present through the recovered completion. The healthy worker retains its
   original registration and continues heartbeating. The complete expected job
   transition sequence is checked, just as in the hard-crash scenario.

The timeout is measured from the last heartbeat the coordinator accepted,
not from the harness's call to SIGSTOP. Bytes already sent before suspension may
still be delivered. The coordinator's own timestamps establish the silence
interval without comparing clocks from different processes.

At expiry the coordinator marks the registration DEAD, handles the job as
WORKER_LOST, and closes that connection. DEAD describes its scheduling/liveness
decision; the operating-system process can still be alive and stopped. The
connection stays open before expiry, and the coordinator closes it as part of
expiry cleanup.

The paused worker is deliberately not resumed in this scenario. A `finally`
block kills and reaps that test-owned process after verification, or after a
test failure, because a stopped process cannot handle SIGTERM. On a passing run,
this cleanup happens after recovery and completion; it cannot be the event that
caused reassignment. The separate scenarios below exercise resuming the old attempt.

## Resuming an expired attempt

`OldAttemptRecoveryTests` adds two scenarios using the same real coordinator,
workers, CLI, three-second sleep, and default six-second timeout. Both register
two workers, wait for attempt 1 to start, pause its owner with SIGSTOP, and wait
for heartbeat expiry and reassignment to the connected survivor.

The old process is then resumed with SIGCONT at one of two points:

| Resume point | What must be protected |
| --- | --- |
| Attempt 2 is RUNNING | Current ownership, attempt, retry count, and running state |
| Attempt 2 is DONE | The accepted result and terminal state, as well as identity |

The harness snapshots the original job's complete event history immediately
before SIGCONT and checks that it is unchanged when the old worker exits. The
RUNNING case explicitly requires the retry to still be RUNNING at this point;
the DONE case includes the stored `slept_ms=3000` result. After attempt 2 completes,
a follow-up Fibonacci job must return `55` from the survivor, with no further
events for the original job.

The resumed process keeps its old socket, worker ID, and attempt-1 assignment.
Expiry has already removed that connection from the coordinator's event loop.
On resuming, the worker encounters the closed connection and exits with status 1.
The test requires a connection-related error, not termination by a cleanup signal.
Its previously accepted heartbeat history must remain unchanged, with exactly
one old-worker registration, one timeout, and one death. The survivor retains its
own registration, continues heartbeating, and the CLI still receives PONG.

Thread scheduling determines whether the old task finishes before the networking
thread detects the closed connection. A local socket send can also succeed without
the coordinator accepting the report. The test prints `old_local_completion_send`
as diagnostic information, but neither requires nor forbids a local completion
log. Its assertions use coordinator events. It does not claim that every run
delivers an outdated completion into the coordinator's report handler.

To exercise that handler's underlying validation independently of this race,
`tests/test_scheduler.c` also adds a direct C test group. It passes old STARTED,
COMPLETED, and FAILED reports to the scheduler in each of these states:

- QUEUED after the first attempt failed or its worker was lost.
- ASSIGNED to attempt 2.
- RUNNING on attempt 2.
- DONE with attempt 2's accepted result.

It runs this matrix for a task-error retry on the same worker ID and for a
worker-loss retry on a different worker ID: 24 rejected reports in total. The
same-worker case checks why the attempt number is necessary even when worker
identity matches. The old completion carries `OLD`, while the accepted completion
carries `NEW`, so an overwrite would be visible. Every rejected call must leave
the entire scheduler snapshot unchanged, including records, timestamps, retry
counters, result bytes, ID allocation state, and the FIFO.

Together these checks cover the closed-connection boundary and state/identity
validation. The current wire format already carries job ID, worker ID, and attempt;
no new message, field, or production C change was required.

Each real-worker scenario has a finally block that kills and reaps the old child
if it is still present after a failure, including if it is still stopped. On a
passing run the resumed worker has already exited by itself. SIGCONT resumes the
old process; it does not restore its expired registration or ownership.

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

These acceptance checks required no changes to the C recovery implementation:

- `src/coordinator/main.c`: connection cleanup finds the worker's active job,
  invokes worker-loss handling, marks the registration dead, and closes the socket.
  Its poll loop checks heartbeat expiry before reading socket events and calls
  that same cleanup path with reason `heartbeat_timeout`.
- `src/coordinator/scheduler.c`: `faultline_scheduler_worker_lost()` applies
  WORKER_LOST to the active job and publishes the resulting queue/state update.
- `src/coordinator/job.c`: the model enforces the retry allowance, clears queued
  ownership, and increments the attempt on the next assignment.
- The scheduler selects the oldest queued job for an eligible idle worker.
- The survivor executes the task and sends its normal STARTED/COMPLETED reports.

The existing [scheduling guide](scheduling.md) explains validation and queue
ownership; [task execution](tasks.md) explains the worker's computation and reporting.

## Scope of this step

These checks verify crash, heartbeat-based recovery, and old-attempt protection
while the coordinator remains alive. Jobs and results are still in memory; coordinator restart recovery
needs the future WAL. The system retains at-least-once semantics: each test asserts
one accepted result for its scenario, not a guarantee that every task executes
only once.

The next separate check is repeated worker losses exhausting the retry allowance.
Existing tests cover retry limits at the model/scheduler level and task-error
exhaustion with real workers; repeated worker-loss exhaustion remains the next
fault-tolerance acceptance scenario.
