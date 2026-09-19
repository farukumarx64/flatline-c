#include "scheduler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
    return EXIT_FAILURE; } } while (0)

/* Large fixtures live outside the stack, matching the coordinator's ownership. */
static struct faultline_scheduler scheduler, before;
static const struct faultline_job_submit_payload sample = {
    .task_type = FAULTLINE_TASK_HASH, .max_retries = 1,
    .argument_size = 3, .arguments = {0xaa, 0, 0xbb}
};

static int test_acceptance(void)
{
    uint64_t first = 0, second = 0;
    struct faultline_job_submit_payload submit = sample;
    faultline_scheduler_init(&scheduler);
    CHECK(faultline_scheduler_submit(&scheduler, &submit, 10, &first) == FAULTLINE_SCHEDULER_OK);
    memset(submit.arguments, 0, sizeof(submit.arguments));
    CHECK(faultline_scheduler_submit(&scheduler, &sample, 11, &second) == FAULTLINE_SCHEDULER_OK);
    CHECK(first == 1 && second == 2 && scheduler.pending.count == 2 && scheduler.count == 2);
    const struct faultline_job *job = faultline_scheduler_find(&scheduler, first);
    CHECK(job != NULL && job->state == FAULTLINE_JOB_QUEUED && job->worker_id == 0);
    CHECK(job->argument_size == 3 && memcmp(job->arguments, sample.arguments, 3) == 0);
    CHECK(faultline_scheduler_find(&scheduler, 0) == NULL);
    CHECK(faultline_scheduler_find(&scheduler, 999) == NULL);
    CHECK(faultline_scheduler_find(NULL, 1) == NULL);
    CHECK(faultline_scheduler_active(&scheduler, 7) == NULL);
    return EXIT_SUCCESS;
}

static int test_fifo_reservation(void)
{
    uint64_t id;
    struct faultline_message assignment, untouched;
    faultline_scheduler_init(&scheduler);
    for (int i = 0; i < 3; ++i) {
        CHECK(faultline_scheduler_submit(&scheduler, &sample, 100, &id) == FAULTLINE_SCHEDULER_OK);
    }
    memset(&assignment, 0xa5, sizeof(assignment));
    memcpy(&untouched, &assignment, sizeof(assignment));
    memcpy(&before, &scheduler, sizeof(scheduler));
    CHECK(faultline_scheduler_assign(&scheduler, 7, 99, &assignment) == FAULTLINE_SCHEDULER_INVALID_JOB);
    CHECK(memcmp(&assignment, &untouched, sizeof(assignment)) == 0);
    CHECK(memcmp(&scheduler, &before, sizeof(scheduler)) == 0);
    CHECK(faultline_scheduler_assign(&scheduler, 7, 101, &assignment) == FAULTLINE_SCHEDULER_OK);
    CHECK(assignment.message_type == FAULTLINE_MSG_JOB_ASSIGN);
    CHECK(assignment.payload.job_assign.identity.job_id == 1);
    CHECK(assignment.payload.job_assign.identity.worker_id == 7);
    CHECK(assignment.payload.job_assign.identity.attempt == 1);
    CHECK(assignment.payload.job_assign.argument_size == 3);
    CHECK(memcmp(assignment.payload.job_assign.arguments, sample.arguments, 3) == 0);
    CHECK(faultline_scheduler_active(&scheduler, 7)->id == 1);
    memcpy(&before, &scheduler, sizeof(scheduler));
    CHECK(faultline_scheduler_assign(&scheduler, 7, 102, &assignment) == FAULTLINE_SCHEDULER_WORKER_BUSY);
    CHECK(memcmp(&scheduler, &before, sizeof(scheduler)) == 0);
    CHECK(faultline_scheduler_assign(&scheduler, 8, 102, &assignment) == FAULTLINE_SCHEDULER_OK);
    CHECK(assignment.payload.job_assign.identity.job_id == 2);
    CHECK(scheduler.pending.count == 1);
    return EXIT_SUCCESS;
}

static int test_reports_and_idle_worker(void)
{
    uint64_t id;
    struct faultline_message assignment;
    faultline_scheduler_init(&scheduler);
    CHECK(faultline_scheduler_submit(&scheduler, &sample, 10, &id) == FAULTLINE_SCHEDULER_OK);
    CHECK(faultline_scheduler_assign(&scheduler, 7, 11, &assignment) == FAULTLINE_SCHEDULER_OK);
    struct faultline_message started = {.message_type = FAULTLINE_MSG_JOB_STARTED,
                                        .payload.job_started = {id, 7, 1}};
    struct faultline_message completed = {.message_type = FAULTLINE_MSG_JOB_COMPLETED,
        .payload.job_completed = {.identity = {id, 7, 1}, .result_size = 3, .result = {1, 0, 2}}};
    memcpy(&before, &scheduler, sizeof(scheduler));
    CHECK(faultline_scheduler_report(&scheduler, 8, &started, 12) == FAULTLINE_SCHEDULER_INVALID_JOB);
    CHECK(faultline_scheduler_report(&scheduler, 7, &completed, 12) == FAULTLINE_SCHEDULER_INVALID_JOB);
    CHECK(memcmp(&scheduler, &before, sizeof(scheduler)) == 0);
    CHECK(faultline_scheduler_report(&scheduler, 7, &started, 12) == FAULTLINE_SCHEDULER_OK);
    CHECK(faultline_scheduler_active(&scheduler, 7)->state == FAULTLINE_JOB_RUNNING);
    CHECK(faultline_scheduler_report(&scheduler, 7, &completed, 13) == FAULTLINE_SCHEDULER_OK);
    CHECK(faultline_scheduler_active(&scheduler, 7) == NULL);
    const struct faultline_job *job = faultline_scheduler_find(&scheduler, id);
    CHECK(job->state == FAULTLINE_JOB_DONE && job->result_size == 3);
    memset(completed.payload.job_completed.result, 0, 3);
    CHECK(job->result[0] == 1 && job->result[1] == 0 && job->result[2] == 2);
    CHECK(scheduler.count == 1); /* Terminal records are retained. */
    CHECK(faultline_scheduler_submit(&scheduler, &sample, 14, &id) == FAULTLINE_SCHEDULER_OK);
    CHECK(faultline_scheduler_assign(&scheduler, 7, 15, &assignment) == FAULTLINE_SCHEDULER_OK);
    CHECK(assignment.payload.job_assign.identity.job_id == 2);
    memcpy(&before, &scheduler, sizeof(scheduler));
    CHECK(faultline_scheduler_report(&scheduler, 7, &completed, 16) == FAULTLINE_SCHEDULER_INVALID_JOB);
    CHECK(memcmp(&scheduler, &before, sizeof(scheduler)) == 0);
    return EXIT_SUCCESS;
}

static int test_retry_order_and_stale_attempt(void)
{
    uint64_t id;
    struct faultline_message assignment;
    faultline_scheduler_init(&scheduler);
    CHECK(faultline_scheduler_submit(&scheduler, &sample, 10, &id) == FAULTLINE_SCHEDULER_OK);
    CHECK(faultline_scheduler_submit(&scheduler, &sample, 10, &id) == FAULTLINE_SCHEDULER_OK);
    CHECK(faultline_scheduler_assign(&scheduler, 7, 11, &assignment) == FAULTLINE_SCHEDULER_OK);
    struct faultline_message failed = {.message_type = FAULTLINE_MSG_JOB_FAILED,
        .payload.job_failed = {.identity = {1, 7, 1}, .failure = FAULTLINE_JOB_FAILURE_TASK}};
    CHECK(faultline_scheduler_report(&scheduler, 7, &failed, 12) == FAULTLINE_SCHEDULER_OK);
    CHECK(faultline_scheduler_find(&scheduler, 1)->retry_count == 1);
    CHECK(faultline_scheduler_assign(&scheduler, 8, 13, &assignment) == FAULTLINE_SCHEDULER_OK);
    CHECK(assignment.payload.job_assign.identity.job_id == 2);
    CHECK(faultline_scheduler_assign(&scheduler, 7, 14, &assignment) == FAULTLINE_SCHEDULER_OK);
    CHECK(assignment.payload.job_assign.identity.job_id == 1 && assignment.payload.job_assign.identity.attempt == 2);
    memcpy(&before, &scheduler, sizeof(scheduler));
    CHECK(faultline_scheduler_report(&scheduler, 7, &failed, 15) == FAULTLINE_SCHEDULER_INVALID_JOB);
    CHECK(memcmp(&scheduler, &before, sizeof(scheduler)) == 0);
    CHECK(faultline_scheduler_worker_lost(&scheduler, 7, 15) == FAULTLINE_SCHEDULER_OK);
    CHECK(faultline_scheduler_find(&scheduler, 1)->state == FAULTLINE_JOB_FAILED);
    CHECK(faultline_scheduler_find(&scheduler, 1)->failure == FAULTLINE_JOB_FAILURE_WORKER_LOST);
    CHECK(faultline_scheduler_worker_lost(&scheduler, 7, 16) == FAULTLINE_SCHEDULER_EMPTY);
    CHECK(faultline_scheduler_active(&scheduler, 7) == NULL && scheduler.pending.count == 0);
    return EXIT_SUCCESS;
}

static int reject_old_reports(uint64_t job_id, int64_t now_ms)
{
    const struct faultline_message reports[] = {
        {.message_type = FAULTLINE_MSG_JOB_STARTED, .payload.job_started = {job_id, 7, 1}},
        {.message_type = FAULTLINE_MSG_JOB_COMPLETED,
         .payload.job_completed = {.identity = {job_id, 7, 1},
                                  .result_size = 3, .result = {'O', 'L', 'D'}}},
        {.message_type = FAULTLINE_MSG_JOB_FAILED,
         .payload.job_failed = {.identity = {job_id, 7, 1}, .failure = FAULTLINE_JOB_FAILURE_TASK}}
    };
    memcpy(&before, &scheduler, sizeof(scheduler));
    for (size_t i = 0; i < sizeof(reports) / sizeof(reports[0]); ++i) {
        CHECK(faultline_scheduler_report(&scheduler, 7, &reports[i], now_ms) ==
              FAULTLINE_SCHEDULER_INVALID_JOB);
        /* Preserve every record, result, timestamp, retry counter, and queue entry. */
        CHECK(memcmp(&scheduler, &before, sizeof(scheduler)) == 0);
    }
    return EXIT_SUCCESS;
}

static int test_old_reports_preserve_retry_and_result(void)
{
    for (uint32_t next_worker = 7; next_worker <= 8; ++next_worker) {
        uint64_t id;
        struct faultline_message assignment;
        faultline_scheduler_init(&scheduler);
        CHECK(faultline_scheduler_submit(&scheduler, &sample, 10, &id) == FAULTLINE_SCHEDULER_OK);
        CHECK(faultline_scheduler_assign(&scheduler, 7, 11, &assignment) == FAULTLINE_SCHEDULER_OK);
        struct faultline_message started = {.message_type = FAULTLINE_MSG_JOB_STARTED,
                                            .payload.job_started = {id, 7, 1}};
        CHECK(faultline_scheduler_report(&scheduler, 7, &started, 12) == FAULTLINE_SCHEDULER_OK);
        if (next_worker == 7) {
            /* A task error can retry on the same live worker: only the attempt changes. */
            const struct faultline_message failed = {.message_type = FAULTLINE_MSG_JOB_FAILED,
                .payload.job_failed = {.identity = {id, 7, 1}, .failure = FAULTLINE_JOB_FAILURE_TASK}};
            CHECK(faultline_scheduler_report(&scheduler, 7, &failed, 13) == FAULTLINE_SCHEDULER_OK);
        } else {
            CHECK(faultline_scheduler_worker_lost(&scheduler, 7, 13) == FAULTLINE_SCHEDULER_OK);
        }
        CHECK(faultline_scheduler_find(&scheduler, id)->state == FAULTLINE_JOB_QUEUED);
        CHECK(scheduler.pending.count == 1);
        CHECK(reject_old_reports(id, 14) == EXIT_SUCCESS);

        CHECK(faultline_scheduler_assign(&scheduler, next_worker, 15, &assignment) == FAULTLINE_SCHEDULER_OK);
        CHECK(faultline_scheduler_find(&scheduler, id)->state == FAULTLINE_JOB_ASSIGNED);
        CHECK(assignment.payload.job_assign.identity.attempt == 2);
        CHECK(reject_old_reports(id, 16) == EXIT_SUCCESS);
        started.payload.job_started = assignment.payload.job_assign.identity;
        CHECK(faultline_scheduler_report(&scheduler, next_worker, &started, 17) == FAULTLINE_SCHEDULER_OK);
        CHECK(faultline_scheduler_find(&scheduler, id)->state == FAULTLINE_JOB_RUNNING);
        CHECK(reject_old_reports(id, 18) == EXIT_SUCCESS);

        const struct faultline_message completed = {.message_type = FAULTLINE_MSG_JOB_COMPLETED,
            .payload.job_completed = {.identity = {id, next_worker, 2},
                                     .result_size = 3, .result = {'N', 'E', 'W'}}};
        CHECK(faultline_scheduler_report(&scheduler, next_worker, &completed, 19) == FAULTLINE_SCHEDULER_OK);
        CHECK(reject_old_reports(id, 20) == EXIT_SUCCESS);
        const struct faultline_job *job = faultline_scheduler_find(&scheduler, id);
        CHECK(job->state == FAULTLINE_JOB_DONE && job->worker_id == next_worker);
        CHECK(job->attempt == 2 && job->retry_count == 1 && scheduler.pending.count == 0);
        CHECK(job->result_size == 3 && memcmp(job->result, "NEW", 3) == 0);
    }
    return EXIT_SUCCESS;
}

static int test_capacity_and_retry_room(void)
{
    uint64_t id = 0;
    struct faultline_message assignment;
    faultline_scheduler_init(&scheduler);
    for (size_t i = 0; i < FAULTLINE_JOB_STORE_CAPACITY; ++i) {
        CHECK(faultline_scheduler_submit(&scheduler, &sample, 10, &id) == FAULTLINE_SCHEDULER_OK);
        CHECK(id == i + 1);
    }
    memcpy(&before, &scheduler, sizeof(scheduler));
    CHECK(faultline_scheduler_submit(&scheduler, &sample, 11, &id) == FAULTLINE_SCHEDULER_FULL);
    CHECK(id == FAULTLINE_JOB_STORE_CAPACITY && memcmp(&before, &scheduler, sizeof(scheduler)) == 0);
    CHECK(faultline_scheduler_assign(&scheduler, 7, 12, &assignment) == FAULTLINE_SCHEDULER_OK);
    CHECK(faultline_scheduler_worker_lost(&scheduler, 7, 13) == FAULTLINE_SCHEDULER_OK);
    CHECK(scheduler.pending.count == FAULTLINE_JOB_STORE_CAPACITY);
    CHECK(faultline_job_queue_peek(&scheduler.pending, &id) == FAULTLINE_JOB_QUEUE_OK && id == 2);
    size_t tail = (scheduler.pending.head + scheduler.pending.count - 1) % FAULTLINE_JOB_QUEUE_CAPACITY;
    CHECK(scheduler.pending.ids[tail] == 1);
    return EXIT_SUCCESS;
}

static int test_id_exhaustion_and_invalid_inputs(void)
{
    uint64_t id = 99;
    struct faultline_message message = {0};
    faultline_scheduler_init(&scheduler);
    memcpy(&before, &scheduler, sizeof(scheduler));
    CHECK(faultline_scheduler_submit(NULL, &sample, 0, &id) == FAULTLINE_SCHEDULER_INVALID_ARGUMENT);
    CHECK(faultline_scheduler_submit(&scheduler, NULL, 0, &id) == FAULTLINE_SCHEDULER_INVALID_ARGUMENT);
    CHECK(faultline_scheduler_submit(&scheduler, &sample, 0, NULL) == FAULTLINE_SCHEDULER_INVALID_ARGUMENT);
    CHECK(faultline_scheduler_submit(&scheduler, &sample, -1, &id) == FAULTLINE_SCHEDULER_INVALID_ARGUMENT);
    struct faultline_job_submit_payload invalid = sample;
    invalid.argument_size = SIZE_MAX;
    CHECK(faultline_scheduler_submit(&scheduler, &invalid, 0, &id) == FAULTLINE_SCHEDULER_INVALID_JOB);
    CHECK(id == 99 && memcmp(&before, &scheduler, sizeof(scheduler)) == 0);
    CHECK(faultline_scheduler_assign(&scheduler, 7, 0, &message) == FAULTLINE_SCHEDULER_EMPTY);
    CHECK(faultline_scheduler_assign(&scheduler, 0, 0, &message) == FAULTLINE_SCHEDULER_INVALID_ARGUMENT);
    CHECK(faultline_scheduler_assign(&scheduler, 7, 0, NULL) == FAULTLINE_SCHEDULER_INVALID_ARGUMENT);
    CHECK(faultline_scheduler_report(&scheduler, 7, NULL, 0) == FAULTLINE_SCHEDULER_INVALID_ARGUMENT);
    CHECK(faultline_scheduler_worker_lost(&scheduler, 0, 0) == FAULTLINE_SCHEDULER_INVALID_ARGUMENT);
    CHECK(faultline_scheduler_active(NULL, 7) == NULL);
    CHECK(memcmp(&before, &scheduler, sizeof(scheduler)) == 0);
    scheduler.next_job_id = UINT64_MAX; /* Simulate exhaustion without billions of submissions. */
    CHECK(faultline_scheduler_submit(&scheduler, &sample, 0, &id) == FAULTLINE_SCHEDULER_OK);
    CHECK(id == UINT64_MAX && scheduler.next_job_id == 0);
    memcpy(&before, &scheduler, sizeof(scheduler));
    CHECK(faultline_scheduler_submit(&scheduler, &sample, 0, &id) == FAULTLINE_SCHEDULER_ID_EXHAUSTED);
    CHECK(id == UINT64_MAX && memcmp(&before, &scheduler, sizeof(scheduler)) == 0);
    return EXIT_SUCCESS;
}

int main(void)
{
    const struct { const char *name; int (*run)(void); } tests[] = {
        {"atomic submission, owned records, and unique IDs", test_acceptance},
        {"FIFO assignment and immediate worker reservation", test_fifo_reservation},
        {"validated reports, retained results, and worker reuse", test_reports_and_idle_worker},
        {"retry ordering, stale attempts, and worker loss", test_retry_order_and_stale_attempt},
        {"old reports preserve retry state and accepted results", test_old_reports_preserve_retry_and_result},
        {"bounded store and guaranteed retry queue room", test_capacity_and_retry_room},
        {"job ID exhaustion and rejected scheduler inputs", test_id_exhaustion_and_invalid_inputs}
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        if (tests[i].run() != EXIT_SUCCESS) { fprintf(stderr, "FAIL: %s\n", tests[i].name); return EXIT_FAILURE; }
        printf("PASS: %s\n", tests[i].name);
    }
    return EXIT_SUCCESS;
}
