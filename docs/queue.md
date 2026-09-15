# Coordinator FIFO job queue

The FIFO module is defined in `include/job_queue.h` and implemented in
`src/coordinator/job_queue.c`. It stores up to 256 pending job IDs in successful
insertion order. It is linked into the coordinator and tested with the
[job model](jobs.md). The [job message formats](job-protocol.md) are now defined.
CLI submission and transport handlers, the authoritative job store, and the
scheduler are still needed to use the queue in the running system.

## What FIFO means

FIFO means first in, first out. New entries join the back; removal takes the front:

| Operation | Returned ID | Waiting order, front to back |
| --- | --- | --- |
| Push job 42 | — | 42 |
| Push job 7 | — | 42, 7 |
| Push job 91 | — | 42, 7, 91 |
| Peek | 42 | 42, 7, 91 |
| Pop | 42 | 7, 91 |
| Push job 13 | — | 7, 91, 13 |

The order is not sorted by job ID, task type, or creation timestamp. A retried job
that is pushed again joins the back, behind work already waiting. FIFO controls
selection order; workers may finish different jobs in a different order.

## Queue entries and job records

The queue copies only the job's 64-bit ID. It never retains the supplied pointer
or copies the argument/result arrays. The future job store must keep full records
separately and support lookup by ID, including jobs that have left the queue.

For example, popping ID 42 removes its waiting-list entry. It does not destroy
job 42, its arguments, assignment, or eventual result. This separation avoids
keeping two mutable copies of the same job and allows status queries after
assignment and completion.

`push()` accepts an initialized job record so it can check that the ID is nonzero,
the state is QUEUED, and there is no assigned worker. It rejects another pending
entry with the same ID, even if it comes from a different C struct. Global ID
uniqueness remains the caller's responsibility; duplicate checking covers only
IDs currently in this queue. The queue is not a job registry or persistence layer.

## API

| Function | Operation |
| --- | --- |
| `faultline_job_queue_init()` | Initialize fresh non-null storage as empty. |
| `faultline_job_queue_push()` | Append a queued job's ID. |
| `faultline_job_queue_peek()` | Return the oldest ID without changing the queue. |
| `faultline_job_queue_pop()` | Remove and return the oldest ID. |

`queue.count` reports how many IDs are waiting. Treat the fields as read-only
outside the module. The coordinator's single event loop will own the queue, so
the API does not add locks or background threads. There is no heap allocation,
socket I/O, clock reading, or job-state mutation in these operations.

Result codes distinguish success, invalid arguments, invalid job state,
duplicate ID, full queue, and empty queue. Errors leave the queue and any output
ID unchanged. Caller-provided pointers must address valid, non-overlapping storage.
Initialization is for fresh or intentionally reset storage; reinitializing a
nonempty queue discards its entries.

The 256-entry limit applies to pending IDs, not the total number of jobs the
future store can retain. A full push reports FULL and preserves all existing
entries. It also leaves the proposed job record untouched. Future submission
handlers must handle this result before acknowledging acceptance. Retry handling
must retain or defer a job whose re-enqueue cannot succeed rather than drop it.

## Circular-array implementation

The queue contains:

```c
uint64_t ids[FAULTLINE_JOB_QUEUE_CAPACITY];
size_t head;
size_t count;
```

`head` identifies the oldest occupied slot. `count` distinguishes an empty queue
from a full one and tracks how many IDs are active. The next insertion slot is:

```c
tail = (head + count) % FAULTLINE_JOB_QUEUE_CAPACITY;
```

`%` is the remainder operator. At the end of the array it brings the position
back to zero, letting the queue reuse freed slots. Popping clears the old slot,
advances `head` with the same wraparound rule, and decrements `count`.

No remaining entries are shifted during removal. Peek and pop take constant
time. Push scans the active IDs to reject duplicates, so it takes linear time
in the number of waiting jobs, bounded here by 256 entries. The ID array itself
uses 2048 bytes, plus the two position/count fields and any struct padding.

## Waiting and assignment

When no worker is available, the scheduler should leave the queue unchanged.
Repeated peeks do not consume work. Once a worker is available, the intended
single-event-loop sequence is:

1. Peek the oldest ID.
2. Look up its full record in the job store.
3. Check that the selected worker is alive and idle.
4. Call `faultline_job_assign()` with that worker ID and the current time.
5. If assignment succeeds, pop the same ID.

If assignment fails, do not pop or push the front job again; doing that would
remove work or move it behind newer jobs. Complete this sequence in one event-loop
operation with no intervening queue mutations. Pop itself does not verify worker
availability or change the job state. The scheduler and send-failure/retry paths
are not implemented by this module.

Likewise, `faultline_job_fail()` changing a record back to QUEUED does not insert
it into this queue. A successful explicit push adds it to the back. A state value
describes a job; queue membership describes whether its ID is waiting here.

## Tests

`tests/test_job_queue.c` has seven groups covering empty/error handling, FIFO
ordering and unchanged job records, non-destructive peeking and failed assignment,
duplicate IDs and state validation, full-queue rejection, circular wraparound,
and a retried job joining the back.

The wraparound test repeatedly removes half the entries and refills the array,
checks duplicates in wrapped storage, and then drains every remaining ID in order.
Tests use unsorted IDs/timestamps, include UINT64_MAX, reuse empty queues, and
compare snapshots to verify rejected operations leave the queue unchanged.
They use model calls only; they do not claim a working CLI submission or scheduler.

Run `make test-unit` and `make SANITIZE=1 test-unit`. The queue tests also run
through `make test` and `make test-sanitize`.
