#ifndef FAULTLINE_TASK_H
#define FAULTLINE_TASK_H

#include "job.h"

#include <stdatomic.h>

#define FAULTLINE_SLEEP_MAX_MS UINT64_C(86400000)
#define FAULTLINE_PRIME_COUNT_MAX UINT64_C(100000000)
#define FAULTLINE_FIBONACCI_MAX UINT64_C(93)

enum faultline_task_status {
    FAULTLINE_TASK_OK = 0,
    FAULTLINE_TASK_INVALID_ARGUMENT,
    FAULTLINE_TASK_CANCELLED,
    FAULTLINE_TASK_SYSTEM_ERROR
};

struct faultline_task_result {
    uint8_t bytes[FAULTLINE_JOB_MAX_RESULT_SIZE];
    size_t size;
};

/*
 * Blocking executor: call outside the networking thread. Owns no socket or job
 * state. Numeric inputs are nonempty ASCII decimal digits (leading zeros OK).
 * sleep: milliseconds, 0..86400000, result "slept_ms=N".
 * prime_count: count primes <= N, 0..100000000, decimal result.
 * fibonacci: F(0)=0, F(1)=1, N in 0..93, decimal uint64 result.
 * hash: raw bytes, including empty/NUL, FNV-1a 64-bit, 16 lowercase hex digits.
 * Results contain no trailing NUL. Hash is a noncryptographic checksum.
 * Arguments must not overlap result. NULL arguments are allowed only for size 0.
 * cancel must point to an initialized atomic flag; the caller can set it while
 * execution runs. All non-OK returns leave *result unchanged.
 */
enum faultline_task_status faultline_task_execute(
    uint16_t task_type, const uint8_t *arguments, size_t argument_size,
    const atomic_bool *cancel, struct faultline_task_result *result);

#endif
