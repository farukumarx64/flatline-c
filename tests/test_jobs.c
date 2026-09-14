#include "job.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,     \
                    #condition);                                               \
            return EXIT_FAILURE;                                               \
        }                                                                      \
    } while (0)

/* Check the entire record, including payload storage, after a rejected operation. */
#define REJECT(job, expression, expected)                                       \
    do {                                                                       \
        unsigned char snapshot[sizeof(job)];                                    \
        memcpy(snapshot, &(job), sizeof(job));                                  \
        CHECK((expression) == (expected));                                      \
        CHECK(memcmp(snapshot, &(job), sizeof(job)) == 0);                       \
    } while (0)

enum operation { ASSIGN, START, COMPLETE, FAIL_TASK, LOSE_WORKER };

static enum faultline_job_result apply(struct faultline_job *job, enum operation operation,
                                       uint32_t worker_id, uint64_t attempt, int64_t now)
{
    const uint8_t result[] = {0x41, 0x00, 0xff};

    switch (operation) {
    case ASSIGN:
        return faultline_job_assign(job, worker_id, now);
    case START:
        return faultline_job_start(job, worker_id, attempt, now);
    case COMPLETE:
        return faultline_job_complete(job, worker_id, attempt, result, sizeof(result), now);
    case FAIL_TASK:
        return faultline_job_fail(job, worker_id, attempt, FAULTLINE_JOB_FAILURE_TASK, now);
    case LOSE_WORKER:
        return faultline_job_fail(job, worker_id, attempt, FAULTLINE_JOB_FAILURE_WORKER_LOST, now);
    }
    return FAULTLINE_JOB_INVALID_ARGUMENT;
}

static int make_job(struct faultline_job *job, enum faultline_job_state state,
                     uint32_t max_retries)
{
    const uint8_t argument[] = {0x00, 0x00, 0x03, 0xe8};

    CHECK(faultline_job_init(job, 42, FAULTLINE_TASK_SLEEP, argument, sizeof(argument),
                            max_retries, 100) == FAULTLINE_JOB_OK);
    if (state == FAULTLINE_JOB_QUEUED) {
        return EXIT_SUCCESS;
    }
    CHECK(faultline_job_assign(job, 7, 110) == FAULTLINE_JOB_OK);
    if (state == FAULTLINE_JOB_ASSIGNED) {
        return EXIT_SUCCESS;
    }
    CHECK(faultline_job_start(job, 7, 1, 120) == FAULTLINE_JOB_OK);
    if (state == FAULTLINE_JOB_RUNNING) {
        return EXIT_SUCCESS;
    }
    if (state == FAULTLINE_JOB_DONE) {
        CHECK(faultline_job_complete(job, 7, 1, NULL, 0, 130) == FAULTLINE_JOB_OK);
        return EXIT_SUCCESS;
    }
    CHECK(state == FAULTLINE_JOB_FAILED);
    for (;;) {
        CHECK(faultline_job_fail(job, 7, job->attempt, FAULTLINE_JOB_FAILURE_TASK, 130) ==
              FAULTLINE_JOB_OK);
        if (job->state == FAULTLINE_JOB_FAILED) {
            return EXIT_SUCCESS;
        }
        CHECK(faultline_job_assign(job, 7, 130) == FAULTLINE_JOB_OK);
        CHECK(faultline_job_start(job, 7, job->attempt, 130) == FAULTLINE_JOB_OK);
    }
}

static int test_initialization_and_owned_arguments(void)
{
    const enum faultline_task_type tasks[] = {
        FAULTLINE_TASK_SLEEP, FAULTLINE_TASK_PRIME_COUNT,
        FAULTLINE_TASK_FIBONACCI, FAULTLINE_TASK_HASH
    };
    uint8_t arguments[FAULTLINE_JOB_MAX_ARGUMENT_SIZE];
    struct faultline_job job;

    for (size_t i = 0; i < sizeof(arguments); ++i) {
        arguments[i] = (uint8_t)(i % 251u);
    }
    for (size_t i = 0; i < sizeof(tasks) / sizeof(tasks[0]); ++i) {
        CHECK(faultline_job_init(&job, i + UINT64_C(1), tasks[i], arguments,
                                sizeof(arguments), 2, 0) == FAULTLINE_JOB_OK);
        CHECK(job.id == i + UINT64_C(1) && job.task_type == tasks[i]);
        CHECK(job.state == FAULTLINE_JOB_QUEUED && job.worker_id == 0 && job.attempt == 0);
        CHECK(job.retry_count == 0 && job.max_retries == 2);
        CHECK(job.created_at_ms == 0 && job.updated_at_ms == 0);
        CHECK(job.assigned_at_ms == -1 && job.started_at_ms == -1 && job.finished_at_ms == -1);
        CHECK(job.argument_size == sizeof(arguments));
        CHECK(memcmp(job.arguments, arguments, sizeof(arguments)) == 0);
        CHECK(job.result_size == 0 && job.failure == FAULTLINE_JOB_FAILURE_NONE);
    }
    memset(arguments, 0xff, sizeof(arguments));
    CHECK(job.arguments[0] == 0 && job.arguments[1] == 1);  /* Caller buffer is independent. */
    CHECK(faultline_job_init(&job, 1, FAULTLINE_TASK_HASH, NULL, 0, 0, 0) == FAULTLINE_JOB_OK);
    CHECK(job.argument_size == 0 && job.arguments[0] == 0);
    return EXIT_SUCCESS;
}

static int test_successful_lifecycle_and_result_ownership(void)
{
    struct faultline_job job;
    uint8_t result[FAULTLINE_JOB_MAX_RESULT_SIZE];

    memset(result, 0xa5, sizeof(result));
    result[1] = 0;  /* Opaque data supports embedded zero bytes. */
    CHECK(make_job(&job, FAULTLINE_JOB_QUEUED, 2) == EXIT_SUCCESS);
    CHECK(faultline_job_assign(&job, 7, 110) == FAULTLINE_JOB_OK);
    CHECK(job.state == FAULTLINE_JOB_ASSIGNED && job.worker_id == 7 && job.attempt == 1);
    CHECK(job.assigned_at_ms == 110 && job.updated_at_ms == 110 && job.started_at_ms == -1);
    CHECK(faultline_job_start(&job, 7, 1, 120) == FAULTLINE_JOB_OK);
    CHECK(job.state == FAULTLINE_JOB_RUNNING && job.started_at_ms == 120);
    CHECK(job.updated_at_ms == 120 && job.finished_at_ms == -1);
    CHECK(faultline_job_complete(&job, 7, 1, result, sizeof(result), 140) == FAULTLINE_JOB_OK);
    CHECK(job.state == FAULTLINE_JOB_DONE && job.finished_at_ms == 140 && job.updated_at_ms == 140);
    CHECK(job.created_at_ms == 100 && job.assigned_at_ms == 110 && job.started_at_ms == 120);
    CHECK(job.worker_id == 7 && job.attempt == 1 && job.retry_count == 0);
    CHECK(job.result_size == sizeof(result) && memcmp(job.result, result, sizeof(result)) == 0);
    memset(result, 0, sizeof(result));
    CHECK(job.result[0] == 0xa5 && job.result[1] == 0);
    return EXIT_SUCCESS;
}

static int test_transition_graph(void)
{
    const enum faultline_job_state states[] = {
        FAULTLINE_JOB_QUEUED, FAULTLINE_JOB_ASSIGNED, FAULTLINE_JOB_RUNNING,
        FAULTLINE_JOB_DONE, FAULTLINE_JOB_FAILED
    };
    /* Columns and rows: QUEUED, ASSIGNED, RUNNING, DONE, FAILED. */
    const int allowed[5][5] = {
        {0, 1, 0, 0, 0},
        {1, 0, 1, 0, 1},
        {1, 0, 0, 1, 1},
        {0, 0, 0, 0, 0},
        {0, 0, 0, 0, 0}
    };
    const enum faultline_job_state invalid[] = {0, (enum faultline_job_state)-1, 99};

    for (size_t from = 0; from < 5; ++from) {
        for (size_t to = 0; to < 5; ++to) {
            CHECK(faultline_job_can_transition(states[from], states[to]) == allowed[from][to]);
        }
        for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
            CHECK(!faultline_job_can_transition(states[from], invalid[i]));
            CHECK(!faultline_job_can_transition(invalid[i], states[from]));
        }
    }
    return EXIT_SUCCESS;
}

static int test_operations_in_every_state(void)
{
    const enum faultline_job_state states[] = {
        FAULTLINE_JOB_QUEUED, FAULTLINE_JOB_ASSIGNED, FAULTLINE_JOB_RUNNING,
        FAULTLINE_JOB_DONE, FAULTLINE_JOB_FAILED
    };
    const enum operation operations[] = {ASSIGN, START, COMPLETE, FAIL_TASK, LOSE_WORKER};
    /* Expected target state for no retries; zero means reject the operation. */
    const enum faultline_job_state targets[5][5] = {
        {FAULTLINE_JOB_ASSIGNED, 0, 0, 0, 0},
        {0, FAULTLINE_JOB_RUNNING, 0, FAULTLINE_JOB_FAILED, FAULTLINE_JOB_FAILED},
        {0, 0, FAULTLINE_JOB_DONE, FAULTLINE_JOB_FAILED, FAULTLINE_JOB_FAILED},
        {0, 0, 0, 0, 0},
        {0, 0, 0, 0, 0}
    };

    for (uint32_t retries = 0; retries <= 2; retries += 2) {
        for (size_t state = 0; state < 5; ++state) {
            for (size_t operation = 0; operation < 5; ++operation) {
                struct faultline_job job;
                enum faultline_job_state target = targets[state][operation];

                CHECK(make_job(&job, states[state], retries) == EXIT_SUCCESS);
                if (target == 0) {
                    REJECT(job, apply(&job, operations[operation], 7, job.attempt, 200),
                           FAULTLINE_JOB_INVALID_TRANSITION);
                } else {
                    if (target == FAULTLINE_JOB_FAILED && retries != 0) {
                        target = FAULTLINE_JOB_QUEUED;
                    }
                    CHECK(apply(&job, operations[operation], 7, job.attempt, 200) == FAULTLINE_JOB_OK);
                    CHECK(job.state == target && job.updated_at_ms == 200);
                }
            }
        }
    }
    return EXIT_SUCCESS;
}

static int test_retry_lifecycle_and_stale_reports(void)
{
    struct faultline_job job;

    CHECK(make_job(&job, FAULTLINE_JOB_RUNNING, 2) == EXIT_SUCCESS);
    CHECK(faultline_job_fail(&job, 7, 1, FAULTLINE_JOB_FAILURE_TASK, 130) == FAULTLINE_JOB_OK);
    CHECK(job.state == FAULTLINE_JOB_QUEUED && job.worker_id == 0 && job.retry_count == 1);
    CHECK(job.attempt == 1 && job.failure == FAULTLINE_JOB_FAILURE_TASK);
    CHECK(job.assigned_at_ms == -1 && job.started_at_ms == -1 && job.finished_at_ms == -1);
    CHECK(job.created_at_ms == 100 && job.updated_at_ms == 130 && job.result_size == 0);
    CHECK(job.id == 42 && job.argument_size == 4 && job.arguments[3] == 0xe8);

    CHECK(faultline_job_assign(&job, 7, 140) == FAULTLINE_JOB_OK);  /* Same worker, new attempt. */
    CHECK(job.attempt == 2 && job.failure == FAULTLINE_JOB_FAILURE_NONE);
    REJECT(job, faultline_job_start(&job, 7, 1, 150), FAULTLINE_JOB_STALE_ATTEMPT);
    CHECK(faultline_job_start(&job, 7, 2, 150) == FAULTLINE_JOB_OK);
    REJECT(job, faultline_job_complete(&job, 7, 1, NULL, 0, 160), FAULTLINE_JOB_STALE_ATTEMPT);
    REJECT(job, faultline_job_fail(&job, 7, 1, FAULTLINE_JOB_FAILURE_WORKER_LOST, 160),
           FAULTLINE_JOB_STALE_ATTEMPT);
    CHECK(faultline_job_fail(&job, 7, 2, FAULTLINE_JOB_FAILURE_WORKER_LOST, 160) == FAULTLINE_JOB_OK);
    CHECK(job.retry_count == 2 && job.attempt == 2 && job.state == FAULTLINE_JOB_QUEUED);
    CHECK(faultline_job_assign(&job, 9, 170) == FAULTLINE_JOB_OK);
    CHECK(job.attempt == 3);
    REJECT(job, faultline_job_start(&job, 7, 3, 180), FAULTLINE_JOB_WRONG_WORKER);
    CHECK(faultline_job_start(&job, 9, 3, 180) == FAULTLINE_JOB_OK);
    CHECK(faultline_job_complete(&job, 9, 3, NULL, 0, 190) == FAULTLINE_JOB_OK);
    CHECK(job.state == FAULTLINE_JOB_DONE && job.worker_id == 9 && job.retry_count == 2);
    return EXIT_SUCCESS;
}

static int test_retry_exhaustion(void)
{
    struct faultline_job job;

    CHECK(make_job(&job, FAULTLINE_JOB_ASSIGNED, 0) == EXIT_SUCCESS);
    CHECK(faultline_job_fail(&job, 7, 1, FAULTLINE_JOB_FAILURE_WORKER_LOST, 120) == FAULTLINE_JOB_OK);
    CHECK(job.state == FAULTLINE_JOB_FAILED && job.retry_count == 0 && job.attempt == 1);
    CHECK(job.worker_id == 7 && job.assigned_at_ms == 110 && job.started_at_ms == -1);
    CHECK(job.finished_at_ms == 120 && job.failure == FAULTLINE_JOB_FAILURE_WORKER_LOST);
    CHECK(make_job(&job, FAULTLINE_JOB_FAILED, 2) == EXIT_SUCCESS);
    CHECK(job.retry_count == 2 && job.attempt == 3 && job.worker_id == 7);
    CHECK(job.finished_at_ms == 130 && job.failure == FAULTLINE_JOB_FAILURE_TASK);
    REJECT(job, faultline_job_assign(&job, 9, 200), FAULTLINE_JOB_INVALID_TRANSITION);
    return EXIT_SUCCESS;
}

static int test_invalid_arguments_and_payload_bounds(void)
{
    struct faultline_job job;
    const uint8_t byte = 0x42;
    const enum operation operations[] = {ASSIGN, START, COMPLETE, FAIL_TASK, LOSE_WORKER};

    memset(&job, 0xa5, sizeof(job));
    CHECK(faultline_job_init(NULL, 1, FAULTLINE_TASK_SLEEP, NULL, 0, 0, 0) ==
          FAULTLINE_JOB_INVALID_ARGUMENT);
    REJECT(job, faultline_job_init(&job, 0, FAULTLINE_TASK_SLEEP, NULL, 0, 0, 0),
           FAULTLINE_JOB_INVALID_ARGUMENT);
    REJECT(job, faultline_job_init(&job, 1, 0, NULL, 0, 0, 0), FAULTLINE_JOB_INVALID_ARGUMENT);
    REJECT(job, faultline_job_init(&job, 1, (enum faultline_task_type)-1, NULL, 0, 0, 0),
           FAULTLINE_JOB_INVALID_ARGUMENT);
    REJECT(job, faultline_job_init(&job, 1, 99, NULL, 0, 0, 0), FAULTLINE_JOB_INVALID_ARGUMENT);
    REJECT(job, faultline_job_init(&job, 1, FAULTLINE_TASK_SLEEP, NULL, 1, 0, 0),
           FAULTLINE_JOB_INVALID_ARGUMENT);
    REJECT(job, faultline_job_init(&job, 1, FAULTLINE_TASK_SLEEP, &byte,
                                  FAULTLINE_JOB_MAX_ARGUMENT_SIZE + 1, 0, 0),
           FAULTLINE_JOB_PAYLOAD_TOO_LARGE);
    REJECT(job, faultline_job_init(&job, 1, FAULTLINE_TASK_SLEEP, &byte, SIZE_MAX, 0, 0),
           FAULTLINE_JOB_PAYLOAD_TOO_LARGE);
    REJECT(job, faultline_job_init(&job, 1, FAULTLINE_TASK_SLEEP, NULL, 0, 0, -1),
           FAULTLINE_JOB_INVALID_ARGUMENT);
    CHECK(make_job(&job, FAULTLINE_JOB_RUNNING, 0) == EXIT_SUCCESS);
    REJECT(job, faultline_job_complete(&job, 7, 1, NULL, 1, 130), FAULTLINE_JOB_INVALID_ARGUMENT);
    REJECT(job, faultline_job_complete(&job, 7, 1, &byte, FAULTLINE_JOB_MAX_RESULT_SIZE + 1, 130),
           FAULTLINE_JOB_PAYLOAD_TOO_LARGE);
    REJECT(job, faultline_job_complete(&job, 7, 1, &byte, SIZE_MAX, 130),
           FAULTLINE_JOB_PAYLOAD_TOO_LARGE);
    REJECT(job, faultline_job_fail(&job, 7, 1, FAULTLINE_JOB_FAILURE_NONE, 130),
           FAULTLINE_JOB_INVALID_ARGUMENT);
    REJECT(job, faultline_job_fail(&job, 7, 1, 99, 130), FAULTLINE_JOB_INVALID_ARGUMENT);
    for (size_t i = 0; i < sizeof(operations) / sizeof(operations[0]); ++i) {
        CHECK(apply(NULL, operations[i], 7, 1, 200) == FAULTLINE_JOB_INVALID_ARGUMENT);
    }
    return EXIT_SUCCESS;
}

static int test_identity_and_monotonic_time(void)
{
    const enum operation operations[] = {ASSIGN, START, COMPLETE, FAIL_TASK, LOSE_WORKER};

    for (size_t i = 0; i < sizeof(operations) / sizeof(operations[0]); ++i) {
        struct faultline_job job;
        enum faultline_job_state state = i == 0 ? FAULTLINE_JOB_QUEUED :
            (i == 1 ? FAULTLINE_JOB_ASSIGNED : FAULTLINE_JOB_RUNNING);

        CHECK(make_job(&job, state, 2) == EXIT_SUCCESS);
        REJECT(job, apply(&job, operations[i], 7, job.attempt, -1), FAULTLINE_JOB_INVALID_ARGUMENT);
        REJECT(job, apply(&job, operations[i], 7, job.attempt, job.updated_at_ms - 1),
               FAULTLINE_JOB_TIME_REVERSED);
        REJECT(job, apply(&job, operations[i], 0, job.attempt, 200), FAULTLINE_JOB_INVALID_ARGUMENT);
        if (i != 0) {
            REJECT(job, apply(&job, operations[i], 8, job.attempt, 200), FAULTLINE_JOB_WRONG_WORKER);
            REJECT(job, apply(&job, operations[i], 7, 0, 200), FAULTLINE_JOB_INVALID_ARGUMENT);
            REJECT(job, apply(&job, operations[i], 7, job.attempt + 1, 200), FAULTLINE_JOB_STALE_ATTEMPT);
        }
        CHECK(apply(&job, operations[i], 7, job.attempt, job.updated_at_ms) == FAULTLINE_JOB_OK);
    }
    return EXIT_SUCCESS;
}

static int test_integer_boundaries(void)
{
    struct faultline_job job;

    CHECK(faultline_job_init(&job, UINT64_MAX, FAULTLINE_TASK_FIBONACCI, NULL, 0,
                            UINT32_MAX, INT64_MAX) == FAULTLINE_JOB_OK);
    CHECK(faultline_job_assign(&job, UINT32_MAX, INT64_MAX) == FAULTLINE_JOB_OK);
    CHECK(faultline_job_start(&job, UINT32_MAX, 1, INT64_MAX) == FAULTLINE_JOB_OK);
    CHECK(faultline_job_complete(&job, UINT32_MAX, 1, NULL, 0, INT64_MAX) == FAULTLINE_JOB_OK);
    CHECK(job.id == UINT64_MAX && job.worker_id == UINT32_MAX && job.finished_at_ms == INT64_MAX);

    CHECK(make_job(&job, FAULTLINE_JOB_RUNNING, UINT32_MAX) == EXIT_SUCCESS);
    /* Simulate the last two attempts without running billions of iterations. */
    job.retry_count = UINT32_MAX - UINT32_C(1);
    job.attempt = UINT32_MAX;
    CHECK(faultline_job_fail(&job, 7, job.attempt, FAULTLINE_JOB_FAILURE_TASK, 200) == FAULTLINE_JOB_OK);
    CHECK(job.state == FAULTLINE_JOB_QUEUED && job.retry_count == UINT32_MAX);
    CHECK(faultline_job_assign(&job, 7, 200) == FAULTLINE_JOB_OK);
    CHECK(job.attempt == (uint64_t)UINT32_MAX + UINT64_C(1));
    CHECK(faultline_job_fail(&job, 7, job.attempt, FAULTLINE_JOB_FAILURE_WORKER_LOST, 200) ==
          FAULTLINE_JOB_OK);
    CHECK(job.state == FAULTLINE_JOB_FAILED && job.retry_count == UINT32_MAX);
    return EXIT_SUCCESS;
}

int main(void)
{
    const struct {
        const char *name;
        int (*run)(void);
    } tests[] = {
        {"job initialization and owned arguments", test_initialization_and_owned_arguments},
        {"successful job lifecycle and owned results", test_successful_lifecycle_and_result_ownership},
        {"complete job transition graph", test_transition_graph},
        {"job operations in every state", test_operations_in_every_state},
        {"retry lifecycle and stale attempt reports", test_retry_lifecycle_and_stale_reports},
        {"retry limits and terminal failure", test_retry_exhaustion},
        {"job invalid arguments and payload bounds", test_invalid_arguments_and_payload_bounds},
        {"job identity checks and monotonic time", test_identity_and_monotonic_time},
        {"job integer boundaries", test_integer_boundaries}
    };

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        if (tests[i].run() != EXIT_SUCCESS) {
            fprintf(stderr, "FAIL: %s\n", tests[i].name);
            return EXIT_FAILURE;
        }
        printf("PASS: %s\n", tests[i].name);
    }
    return EXIT_SUCCESS;
}
