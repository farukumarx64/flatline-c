#include "task.h"
#include "net.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <time.h>

static int parse_number(const uint8_t *bytes, size_t size, uint64_t limit,
                        uint64_t *number)
{
    uint64_t value = 0;
    if (size == 0) { return -1; }
    for (size_t i = 0; i < size; ++i) {
        if (bytes[i] < '0' || bytes[i] > '9') { return -1; }
        uint64_t digit = (uint64_t)(bytes[i] - '0');
        if (value > limit / 10 || (value == limit / 10 && digit > limit % 10)) {
            return -1;
        }
        value = value * 10 + digit;
    }
    *number = value;
    return 0;
}

static enum faultline_task_status run_sleep(uint64_t duration, const atomic_bool *cancel)
{
    int64_t start = faultline_monotonic_ms();
    if (start < 0) { return FAULTLINE_TASK_SYSTEM_ERROR; }
    int64_t deadline = start + (int64_t)duration;
    for (;;) {
        if (atomic_load(cancel)) { return FAULTLINE_TASK_CANCELLED; }
        int64_t now = faultline_monotonic_ms();
        if (now < 0) { return FAULTLINE_TASK_SYSTEM_ERROR; }
        if (now >= deadline) { return FAULTLINE_TASK_OK; }
        int64_t remaining = deadline - now;
        /* Short naps allow shutdown to cancel even a day-long task promptly. */
        long slice_ms = remaining > 20 ? 20 : (long)remaining;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = slice_ms * 1000000L};
        if (nanosleep(&pause, NULL) < 0 && errno != EINTR) {
            return FAULTLINE_TASK_SYSTEM_ERROR;
        }
        /* Recheck the monotonic deadline after interruptions and scheduling delays. */
    }
}

enum faultline_task_status faultline_task_execute(
    uint16_t task_type, const uint8_t *arguments, size_t argument_size,
    const atomic_bool *cancel, struct faultline_task_result *result)
{
    if (cancel == NULL || result == NULL ||
        (arguments == NULL && argument_size != 0) ||
        argument_size > FAULTLINE_JOB_MAX_ARGUMENT_SIZE) {
        return FAULTLINE_TASK_INVALID_ARGUMENT;
    }
    if (atomic_load(cancel)) { return FAULTLINE_TASK_CANCELLED; }
    uint64_t number = 0;
    uint64_t limit;
    switch (task_type) {
    case FAULTLINE_TASK_SLEEP: limit = FAULTLINE_SLEEP_MAX_MS; break;
    case FAULTLINE_TASK_PRIME_COUNT: limit = FAULTLINE_PRIME_COUNT_MAX; break;
    case FAULTLINE_TASK_FIBONACCI: limit = FAULTLINE_FIBONACCI_MAX; break;
    case FAULTLINE_TASK_HASH: limit = 0; break;
    default: return FAULTLINE_TASK_INVALID_ARGUMENT;
    }
    if (task_type != FAULTLINE_TASK_HASH &&
        parse_number(arguments, argument_size, limit, &number) < 0) {
        return FAULTLINE_TASK_INVALID_ARGUMENT;
    }
    struct faultline_task_result completed = {0};
    int length;
    if (task_type == FAULTLINE_TASK_SLEEP) {
        enum faultline_task_status status = run_sleep(number, cancel);
        if (status != FAULTLINE_TASK_OK) { return status; }
        length = snprintf((char *)completed.bytes, sizeof(completed.bytes),
                          "slept_ms=%" PRIu64, number);
    } else if (task_type == FAULTLINE_TASK_PRIME_COUNT) {
        uint64_t count = number >= 2 ? 1 : 0;
        for (uint64_t candidate = 3; candidate <= number; candidate += 2) {
            if (atomic_load(cancel)) { return FAULTLINE_TASK_CANCELLED; }
            int prime = 1;
            /* Division expresses divisor^2 <= candidate without multiplication overflow. */
            for (uint64_t divisor = 3; divisor <= candidate / divisor; divisor += 2) {
                if (atomic_load(cancel)) { return FAULTLINE_TASK_CANCELLED; }
                if (candidate % divisor == 0) { prime = 0; break; }
            }
            if (prime) { ++count; }
        }
        length = snprintf((char *)completed.bytes, sizeof(completed.bytes), "%" PRIu64, count);
    } else if (task_type == FAULTLINE_TASK_FIBONACCI) {
        uint64_t previous = 0, current = 1;
        for (uint64_t i = 2; i <= number; ++i) {
            uint64_t next = previous + current;
            previous = current;
            current = next;
        }
        length = snprintf((char *)completed.bytes, sizeof(completed.bytes),
                          "%" PRIu64, number == 0 ? UINT64_C(0) : current);
    } else {
        uint64_t hash = UINT64_C(14695981039346656037);
        for (size_t i = 0; i < argument_size; ++i) {
            hash ^= arguments[i];
            hash *= UINT64_C(1099511628211); /* Unsigned wraparound is part of FNV-1a. */
        }
        length = snprintf((char *)completed.bytes, sizeof(completed.bytes), "%016" PRIx64, hash);
    }
    if (length < 0 || (size_t)length >= sizeof(completed.bytes)) {
        return FAULTLINE_TASK_SYSTEM_ERROR;
    }
    if (atomic_load(cancel)) { return FAULTLINE_TASK_CANCELLED; }
    completed.size = (size_t)length;
    *result = completed;
    return FAULTLINE_TASK_OK;
}
