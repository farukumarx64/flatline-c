#include "task.h"
#include "net.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
    return EXIT_FAILURE; } } while (0)

static int expect_result(uint16_t type, const char *input, const char *expected)
{
    atomic_bool cancel = ATOMIC_VAR_INIT(false);
    struct faultline_task_result result;
    CHECK(faultline_task_execute(type, (const uint8_t *)input, strlen(input), &cancel, &result) ==
          FAULTLINE_TASK_OK);
    CHECK(result.size == strlen(expected));
    CHECK(memcmp(result.bytes, expected, result.size) == 0);
    return EXIT_SUCCESS;
}

static int test_sleep(void)
{
    CHECK(expect_result(FAULTLINE_TASK_SLEEP, "000", "slept_ms=0") == 0);
    int64_t start = faultline_monotonic_ms();
    CHECK(start >= 0);
    CHECK(expect_result(FAULTLINE_TASK_SLEEP, "30", "slept_ms=30") == 0);
    CHECK(faultline_monotonic_ms() - start >= 30);
    return EXIT_SUCCESS;
}

static int test_primes(void)
{
    const char *inputs[] = {"0", "1", "2", "3", "4", "25", "100", "1000000"};
    const char *answers[] = {"0", "0", "1", "2", "2", "9", "25", "78498"};
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        CHECK(expect_result(FAULTLINE_TASK_PRIME_COUNT, inputs[i], answers[i]) == 0);
    }
    return EXIT_SUCCESS;
}

static int test_fibonacci(void)
{
    const char *inputs[] = {"0", "1", "2", "10", "92", "93"};
    const char *answers[] = {"0", "1", "1", "55", "7540113804746346429", "12200160415121876738"};
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        CHECK(expect_result(FAULTLINE_TASK_FIBONACCI, inputs[i], answers[i]) == 0);
    }
    return EXIT_SUCCESS;
}

static int test_hash(void)
{
    CHECK(expect_result(FAULTLINE_TASK_HASH, "", "cbf29ce484222325") == 0);
    CHECK(expect_result(FAULTLINE_TASK_HASH, "a", "af63dc4c8601ec8c") == 0);
    CHECK(expect_result(FAULTLINE_TASK_HASH, "hello", "a430d84680aabd0b") == 0);
    CHECK(expect_result(FAULTLINE_TASK_HASH, "foobar", "85944171f73967e8") == 0);
    atomic_bool cancel = ATOMIC_VAR_INIT(false);
    struct faultline_task_result result;
    const uint8_t zero = 0;
    CHECK(faultline_task_execute(FAULTLINE_TASK_HASH, &zero, 1, &cancel, &result) == FAULTLINE_TASK_OK);
    CHECK(result.size == 16 && memcmp(result.bytes, "af63bd4c8601b7df", 16) == 0);
    CHECK(faultline_task_execute(FAULTLINE_TASK_HASH, NULL, 0, &cancel, &result) == FAULTLINE_TASK_OK);
    CHECK(result.size == 16 && memcmp(result.bytes, "cbf29ce484222325", 16) == 0);
    return EXIT_SUCCESS;
}

static int test_invalid_inputs(void)
{
    atomic_bool cancel = ATOMIC_VAR_INIT(false);
    struct faultline_task_result result, before;
    memset(&result, 0xa5, sizeof(result));
    memcpy(&before, &result, sizeof(result));
    const char *invalid[] = {"", "-1", "+1", " 1", "1 ", "1.0", "1ms", "1\n",
                             "18446744073709551616", "999999999999999999999999999999"};
    for (uint16_t type = FAULTLINE_TASK_SLEEP; type <= FAULTLINE_TASK_FIBONACCI; ++type) {
        for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
            CHECK(faultline_task_execute(type, (const uint8_t *)invalid[i], strlen(invalid[i]),
                                        &cancel, &result) == FAULTLINE_TASK_INVALID_ARGUMENT);
            CHECK(memcmp(&result, &before, sizeof(result)) == 0);
        }
        const uint8_t binary[] = {'1', 0, '2'};
        CHECK(faultline_task_execute(type, binary, sizeof(binary), &cancel, &result) == FAULTLINE_TASK_INVALID_ARGUMENT);
        CHECK(memcmp(&result, &before, sizeof(result)) == 0);
    }
    const char *over_limit[] = {"86400001", "100000001", "94"};
    for (uint16_t type = 1; type <= 3; ++type) {
        CHECK(faultline_task_execute(type, (const uint8_t *)over_limit[type - 1],
              strlen(over_limit[type - 1]), &cancel, &result) == FAULTLINE_TASK_INVALID_ARGUMENT);
        CHECK(memcmp(&result, &before, sizeof(result)) == 0);
    }
    CHECK(faultline_task_execute(0, NULL, 0, &cancel, &result) == FAULTLINE_TASK_INVALID_ARGUMENT);
    CHECK(faultline_task_execute(5, NULL, 0, &cancel, &result) == FAULTLINE_TASK_INVALID_ARGUMENT);
    CHECK(faultline_task_execute(4, NULL, 1, &cancel, &result) == FAULTLINE_TASK_INVALID_ARGUMENT);
    CHECK(faultline_task_execute(4, NULL, 0, NULL, &result) == FAULTLINE_TASK_INVALID_ARGUMENT);
    CHECK(faultline_task_execute(4, NULL, 0, &cancel, NULL) == FAULTLINE_TASK_INVALID_ARGUMENT);
    uint8_t too_large[FAULTLINE_JOB_MAX_ARGUMENT_SIZE + 1] = {0};
    CHECK(faultline_task_execute(4, too_large, sizeof(too_large), &cancel, &result) == FAULTLINE_TASK_INVALID_ARGUMENT);
    CHECK(memcmp(&result, &before, sizeof(result)) == 0);
    return EXIT_SUCCESS;
}

static int test_precancelled(void)
{
    atomic_bool cancel = ATOMIC_VAR_INIT(true);
    struct faultline_task_result result, before;
    memset(&result, 0xa5, sizeof(result));
    memcpy(&before, &result, sizeof(result));
    for (uint16_t type = 1; type <= 4; ++type) {
        CHECK(faultline_task_execute(type, (const uint8_t *)"1", 1, &cancel, &result) == FAULTLINE_TASK_CANCELLED);
        CHECK(memcmp(&result, &before, sizeof(result)) == 0);
    }
    return EXIT_SUCCESS;
}

struct cancellation_case {
    uint16_t type;
    const char *input;
    atomic_bool cancel;
    enum faultline_task_status status;
    struct faultline_task_result result;
};

static void *cancelled_task(void *argument)
{
    struct cancellation_case *test = argument;
    test->status = faultline_task_execute(test->type, (const uint8_t *)test->input,
                                         strlen(test->input), &test->cancel, &test->result);
    return NULL;
}

static int test_cancel_running(void)
{
    const char *inputs[] = {"86400000", "100000000"};
    for (uint16_t type = 1; type <= 2; ++type) {
        struct cancellation_case test = {.type = type, .input = inputs[type - 1]};
        atomic_init(&test.cancel, false);
        pthread_t thread;
        CHECK(pthread_create(&thread, NULL, cancelled_task, &test) == 0);
        const struct timespec pause = {.tv_sec = 0, .tv_nsec = 30000000};
        (void)nanosleep(&pause, NULL);
        atomic_store(&test.cancel, true);
        CHECK(pthread_join(thread, NULL) == 0);
        CHECK(test.status == FAULTLINE_TASK_CANCELLED);
        CHECK(test.result.size == 0);
    }
    return EXIT_SUCCESS;
}

int main(void)
{
    int (*tests[])(void) = {test_sleep, test_primes, test_fibonacci, test_hash,
                           test_invalid_inputs, test_precancelled, test_cancel_running};
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        if (tests[i]() != EXIT_SUCCESS) { return EXIT_FAILURE; }
    }
    puts("All 7 task test groups passed.");
    return EXIT_SUCCESS;
}
