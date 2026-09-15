#include "job_queue.h"

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

#define REJECT(queue, expression, expected)                                     \
    do {                                                                       \
        unsigned char snapshot[sizeof(queue)];                                  \
        memcpy(snapshot, &(queue), sizeof(queue));                              \
        CHECK((expression) == (expected));                                      \
        CHECK(memcmp(snapshot, &(queue), sizeof(queue)) == 0);                   \
    } while (0)

/* A queue copies IDs only. These unit fixtures need no persistent job store. */
static int push_id(struct faultline_job_queue *queue, uint64_t id)
{
    struct faultline_job job;

    CHECK(faultline_job_init(&job, id, FAULTLINE_TASK_SLEEP, NULL, 0, 0, 0) == FAULTLINE_JOB_OK);
    CHECK(faultline_job_queue_push(queue, &job) == FAULTLINE_JOB_QUEUE_OK);
    return EXIT_SUCCESS;
}

static int test_empty_queue_and_invalid_arguments(void)
{
    struct faultline_job_queue queue;
    struct faultline_job job;
    uint64_t output = 123;

    faultline_job_queue_init(&queue);
    CHECK(queue.count == 0 && queue.head == 0);
    for (size_t i = 0; i < FAULTLINE_JOB_QUEUE_CAPACITY; ++i) {
        CHECK(queue.ids[i] == 0);
    }
    REJECT(queue, faultline_job_queue_peek(&queue, &output), FAULTLINE_JOB_QUEUE_EMPTY);
    CHECK(output == 123);
    REJECT(queue, faultline_job_queue_pop(&queue, &output), FAULTLINE_JOB_QUEUE_EMPTY);
    CHECK(output == 123);
    CHECK(faultline_job_queue_peek(NULL, &output) == FAULTLINE_JOB_QUEUE_INVALID_ARGUMENT);
    CHECK(faultline_job_queue_pop(NULL, &output) == FAULTLINE_JOB_QUEUE_INVALID_ARGUMENT);
    CHECK(output == 123);
    REJECT(queue, faultline_job_queue_push(&queue, NULL), FAULTLINE_JOB_QUEUE_INVALID_ARGUMENT);
    CHECK(faultline_job_init(&job, 1, FAULTLINE_TASK_HASH, NULL, 0, 0, 0) == FAULTLINE_JOB_OK);
    CHECK(faultline_job_queue_push(NULL, &job) == FAULTLINE_JOB_QUEUE_INVALID_ARGUMENT);
    CHECK(faultline_job_queue_push(&queue, &job) == FAULTLINE_JOB_QUEUE_OK);
    REJECT(queue, faultline_job_queue_peek(&queue, NULL), FAULTLINE_JOB_QUEUE_INVALID_ARGUMENT);
    REJECT(queue, faultline_job_queue_pop(&queue, NULL), FAULTLINE_JOB_QUEUE_INVALID_ARGUMENT);
    CHECK(faultline_job_queue_pop(&queue, &output) == FAULTLINE_JOB_QUEUE_OK && output == 1);
    return EXIT_SUCCESS;
}

static int test_fifo_order_and_job_preservation(void)
{
    const uint64_t ids[] = {91, 7, UINT64_MAX, 3};
    const int64_t times[] = {500, 100, 300, 200};
    struct faultline_job jobs[4];
    unsigned char original[sizeof(jobs)];
    struct faultline_job_queue queue;
    uint64_t output;

    faultline_job_queue_init(&queue);
    for (size_t i = 0; i < 4; ++i) {
        const uint8_t input[] = {0x00, (uint8_t)i, 0xff};

        CHECK(faultline_job_init(&jobs[i], ids[i], FAULTLINE_TASK_HASH, input, sizeof(input),
                                2, times[i]) == FAULTLINE_JOB_OK);
    }
    memcpy(original, jobs, sizeof(jobs));
    for (size_t i = 0; i < 4; ++i) {
        CHECK(faultline_job_queue_push(&queue, &jobs[i]) == FAULTLINE_JOB_QUEUE_OK);
        CHECK(queue.count == i + 1);
    }
    for (size_t i = 0; i < 4; ++i) {
        CHECK(faultline_job_queue_peek(&queue, &output) == FAULTLINE_JOB_QUEUE_OK && output == ids[i]);
        CHECK(faultline_job_queue_pop(&queue, &output) == FAULTLINE_JOB_QUEUE_OK && output == ids[i]);
        CHECK(queue.count == 3 - i);
    }
    /* Neither insertion nor removal changes the authoritative job records. */
    CHECK(memcmp(original, jobs, sizeof(jobs)) == 0);
    CHECK(queue.count == 0);
    return EXIT_SUCCESS;
}

static int test_peek_waiting_and_assignment_failure(void)
{
    struct faultline_job_queue queue;
    struct faultline_job job;
    unsigned char snapshot[sizeof(queue)];
    uint64_t output = 0;

    faultline_job_queue_init(&queue);
    CHECK(faultline_job_init(&job, 42, FAULTLINE_TASK_SLEEP, NULL, 0, 0, 100) == FAULTLINE_JOB_OK);
    CHECK(faultline_job_queue_push(&queue, &job) == FAULTLINE_JOB_QUEUE_OK);
    CHECK(push_id(&queue, 43) == EXIT_SUCCESS);
    memcpy(snapshot, &queue, sizeof(queue));
    /* While no worker is available, inspection does not consume pending work. */
    for (size_t i = 0; i < 10; ++i) {
        CHECK(faultline_job_queue_peek(&queue, &output) == FAULTLINE_JOB_QUEUE_OK && output == 42);
        CHECK(queue.count == 2 && job.state == FAULTLINE_JOB_QUEUED);
        CHECK(memcmp(snapshot, &queue, sizeof(queue)) == 0);
    }
    CHECK(faultline_job_assign(&job, 0, 110) == FAULTLINE_JOB_INVALID_ARGUMENT);
    CHECK(faultline_job_assign(&job, 7, 99) == FAULTLINE_JOB_TIME_REVERSED);
    CHECK(memcmp(snapshot, &queue, sizeof(queue)) == 0);
    CHECK(faultline_job_queue_peek(&queue, &output) == FAULTLINE_JOB_QUEUE_OK && output == 42);
    /* Future scheduler ordering: assign successfully before committing the pop. */
    CHECK(faultline_job_assign(&job, 7, 110) == FAULTLINE_JOB_OK);
    CHECK(faultline_job_queue_pop(&queue, &output) == FAULTLINE_JOB_QUEUE_OK && output == 42);
    CHECK(job.state == FAULTLINE_JOB_ASSIGNED && job.worker_id == 7 && job.attempt == 1);
    CHECK(faultline_job_queue_peek(&queue, &output) == FAULTLINE_JOB_QUEUE_OK && output == 43);
    return EXIT_SUCCESS;
}

static int test_duplicates_and_job_state_validation(void)
{
    struct faultline_job_queue queue;
    struct faultline_job job;
    struct faultline_job same_id;
    uint64_t output;

    faultline_job_queue_init(&queue);
    CHECK(faultline_job_init(&job, 42, FAULTLINE_TASK_SLEEP, NULL, 0, 0, 100) == FAULTLINE_JOB_OK);
    CHECK(faultline_job_init(&same_id, 42, FAULTLINE_TASK_HASH, NULL, 0, 0, 200) == FAULTLINE_JOB_OK);
    CHECK(faultline_job_queue_push(&queue, &job) == FAULTLINE_JOB_QUEUE_OK);
    REJECT(queue, faultline_job_queue_push(&queue, &job), FAULTLINE_JOB_QUEUE_DUPLICATE);
    REJECT(queue, faultline_job_queue_push(&queue, &same_id), FAULTLINE_JOB_QUEUE_DUPLICATE);
    CHECK(faultline_job_queue_pop(&queue, &output) == FAULTLINE_JOB_QUEUE_OK);
    CHECK(faultline_job_assign(&job, 7, 110) == FAULTLINE_JOB_OK);
    REJECT(queue, faultline_job_queue_push(&queue, &job), FAULTLINE_JOB_QUEUE_INVALID_STATE);
    CHECK(faultline_job_start(&job, 7, 1, 120) == FAULTLINE_JOB_OK);
    REJECT(queue, faultline_job_queue_push(&queue, &job), FAULTLINE_JOB_QUEUE_INVALID_STATE);
    CHECK(faultline_job_complete(&job, 7, 1, NULL, 0, 130) == FAULTLINE_JOB_OK);
    REJECT(queue, faultline_job_queue_push(&queue, &job), FAULTLINE_JOB_QUEUE_INVALID_STATE);
    CHECK(faultline_job_assign(&same_id, 7, 200) == FAULTLINE_JOB_OK);
    CHECK(faultline_job_fail(&same_id, 7, 1, FAULTLINE_JOB_FAILURE_TASK, 200) == FAULTLINE_JOB_OK);
    REJECT(queue, faultline_job_queue_push(&queue, &same_id), FAULTLINE_JOB_QUEUE_INVALID_STATE);

    CHECK(faultline_job_init(&job, 43, FAULTLINE_TASK_SLEEP, NULL, 0, 0, 100) == FAULTLINE_JOB_OK);
    /* Deliberate malformed records to check the queue's defensive input guards. */
    job.worker_id = 7;
    REJECT(queue, faultline_job_queue_push(&queue, &job), FAULTLINE_JOB_QUEUE_INVALID_STATE);
    job.worker_id = 0;
    job.state = 0;
    REJECT(queue, faultline_job_queue_push(&queue, &job), FAULTLINE_JOB_QUEUE_INVALID_STATE);
    job.state = FAULTLINE_JOB_QUEUED;
    job.id = 0;
    REJECT(queue, faultline_job_queue_push(&queue, &job), FAULTLINE_JOB_QUEUE_INVALID_ARGUMENT);
    return EXIT_SUCCESS;
}

static int test_full_queue_preserves_existing_work(void)
{
    struct faultline_job_queue queue;
    struct faultline_job extra;
    uint64_t output;

    faultline_job_queue_init(&queue);
    for (uint64_t id = 1; id <= FAULTLINE_JOB_QUEUE_CAPACITY; ++id) {
        CHECK(push_id(&queue, id) == EXIT_SUCCESS);
    }
    CHECK(queue.count == FAULTLINE_JOB_QUEUE_CAPACITY);
    CHECK(faultline_job_init(&extra, FAULTLINE_JOB_QUEUE_CAPACITY + UINT64_C(1),
                            FAULTLINE_TASK_SLEEP, NULL, 0, 0, 0) == FAULTLINE_JOB_OK);
    REJECT(queue, faultline_job_queue_push(&queue, &extra), FAULTLINE_JOB_QUEUE_FULL);
    CHECK(extra.state == FAULTLINE_JOB_QUEUED && extra.attempt == 0);
    CHECK(faultline_job_init(&extra, 1, FAULTLINE_TASK_SLEEP, NULL, 0, 0, 0) == FAULTLINE_JOB_OK);
    REJECT(queue, faultline_job_queue_push(&queue, &extra), FAULTLINE_JOB_QUEUE_DUPLICATE);
    for (uint64_t id = 1; id <= FAULTLINE_JOB_QUEUE_CAPACITY; ++id) {
        CHECK(faultline_job_queue_pop(&queue, &output) == FAULTLINE_JOB_QUEUE_OK && output == id);
    }
    CHECK(queue.count == 0);
    return EXIT_SUCCESS;
}

static int test_wraparound_and_reuse(void)
{
    struct faultline_job_queue queue;
    struct faultline_job duplicate;
    uint64_t output;
    uint64_t next_id = 1;
    uint64_t expected_id = 1;

    faultline_job_queue_init(&queue);
    for (size_t i = 0; i < FAULTLINE_JOB_QUEUE_CAPACITY; ++i) {
        CHECK(push_id(&queue, next_id++) == EXIT_SUCCESS);
    }
    for (size_t round = 0; round < 10; ++round) {
        for (size_t i = 0; i < FAULTLINE_JOB_QUEUE_CAPACITY / 2; ++i) {
            CHECK(faultline_job_queue_pop(&queue, &output) == FAULTLINE_JOB_QUEUE_OK);
            CHECK(output == expected_id++);
        }
        for (size_t i = 0; i < FAULTLINE_JOB_QUEUE_CAPACITY / 2; ++i) {
            CHECK(push_id(&queue, next_id++) == EXIT_SUCCESS);
        }
        CHECK(queue.count == FAULTLINE_JOB_QUEUE_CAPACITY);
        CHECK(faultline_job_init(&duplicate, next_id - 1, FAULTLINE_TASK_SLEEP,
                                NULL, 0, 0, 0) == FAULTLINE_JOB_OK);
        REJECT(queue, faultline_job_queue_push(&queue, &duplicate), FAULTLINE_JOB_QUEUE_DUPLICATE);
        CHECK(faultline_job_queue_peek(&queue, &output) == FAULTLINE_JOB_QUEUE_OK);
        CHECK(output == expected_id);
    }
    while (queue.count != 0) {
        CHECK(faultline_job_queue_pop(&queue, &output) == FAULTLINE_JOB_QUEUE_OK);
        CHECK(output == expected_id++);
    }
    CHECK(expected_id == next_id);
    /* Reuse empty storage after wraparound with a previously removed ID. */
    CHECK(push_id(&queue, 1) == EXIT_SUCCESS);
    CHECK(faultline_job_queue_pop(&queue, &output) == FAULTLINE_JOB_QUEUE_OK && output == 1);
    return EXIT_SUCCESS;
}

static int test_retry_joins_back_of_queue(void)
{
    struct faultline_job_queue queue;
    struct faultline_job job;
    uint64_t output;
    const uint64_t expected[] = {2, 3, 1};

    faultline_job_queue_init(&queue);
    CHECK(faultline_job_init(&job, 1, FAULTLINE_TASK_SLEEP, NULL, 0, 1, 0) == FAULTLINE_JOB_OK);
    CHECK(faultline_job_queue_push(&queue, &job) == FAULTLINE_JOB_QUEUE_OK);
    CHECK(push_id(&queue, 2) == EXIT_SUCCESS);
    CHECK(push_id(&queue, 3) == EXIT_SUCCESS);
    CHECK(faultline_job_queue_peek(&queue, &output) == FAULTLINE_JOB_QUEUE_OK && output == 1);
    CHECK(faultline_job_assign(&job, 7, 10) == FAULTLINE_JOB_OK);
    CHECK(faultline_job_queue_pop(&queue, &output) == FAULTLINE_JOB_QUEUE_OK && output == 1);
    CHECK(faultline_job_fail(&job, 7, 1, FAULTLINE_JOB_FAILURE_WORKER_LOST, 20) == FAULTLINE_JOB_OK);
    CHECK(job.state == FAULTLINE_JOB_QUEUED && job.retry_count == 1);
    CHECK(faultline_job_queue_push(&queue, &job) == FAULTLINE_JOB_QUEUE_OK);
    REJECT(queue, faultline_job_queue_push(&queue, &job), FAULTLINE_JOB_QUEUE_DUPLICATE);
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        CHECK(faultline_job_queue_pop(&queue, &output) == FAULTLINE_JOB_QUEUE_OK && output == expected[i]);
    }
    CHECK(job.state == FAULTLINE_JOB_QUEUED && job.worker_id == 0 && job.attempt == 1);
    return EXIT_SUCCESS;
}

int main(void)
{
    const struct {
        const char *name;
        int (*run)(void);
    } tests[] = {
        {"empty job queue and invalid arguments", test_empty_queue_and_invalid_arguments},
        {"FIFO insertion order and job preservation", test_fifo_order_and_job_preservation},
        {"waiting, peeking, and failed assignment", test_peek_waiting_and_assignment_failure},
        {"duplicate job IDs and state validation", test_duplicates_and_job_state_validation},
        {"full queue preserves pending jobs", test_full_queue_preserves_existing_work},
        {"circular queue wraparound and reuse", test_wraparound_and_reuse},
        {"retried job joins the back of the queue", test_retry_joins_back_of_queue}
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
