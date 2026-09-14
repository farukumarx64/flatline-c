#include "job.h"

#include <string.h>

int faultline_job_can_transition(enum faultline_job_state from,
                                enum faultline_job_state to)
{
    switch (from) {
    case FAULTLINE_JOB_QUEUED:
        return to == FAULTLINE_JOB_ASSIGNED;
    case FAULTLINE_JOB_ASSIGNED:
        return to == FAULTLINE_JOB_RUNNING || to == FAULTLINE_JOB_QUEUED ||
               to == FAULTLINE_JOB_FAILED;
    case FAULTLINE_JOB_RUNNING:
        return to == FAULTLINE_JOB_DONE || to == FAULTLINE_JOB_QUEUED ||
               to == FAULTLINE_JOB_FAILED;
    default:
        return 0;
    }
}

static enum faultline_job_result check_payload(const uint8_t *data, size_t size,
                                               size_t capacity)
{
    if (size > capacity) {
        return FAULTLINE_JOB_PAYLOAD_TOO_LARGE;
    }
    if (data == NULL && size != 0) {
        return FAULTLINE_JOB_INVALID_ARGUMENT;
    }
    return FAULTLINE_JOB_OK;
}

static enum faultline_job_result check_transition(
    const struct faultline_job *job, enum faultline_job_state next, int64_t now_ms)
{
    if (job == NULL || now_ms < 0) {
        return FAULTLINE_JOB_INVALID_ARGUMENT;
    }
    if (!faultline_job_can_transition(job->state, next)) {
        return FAULTLINE_JOB_INVALID_TRANSITION;
    }
    if (now_ms < job->updated_at_ms) {
        return FAULTLINE_JOB_TIME_REVERSED;
    }
    return FAULTLINE_JOB_OK;
}

static enum faultline_job_result check_attempt(
    const struct faultline_job *job, uint32_t worker_id, uint64_t attempt)
{
    if (worker_id == 0 || attempt == 0) {
        return FAULTLINE_JOB_INVALID_ARGUMENT;
    }
    if (worker_id != job->worker_id) {
        return FAULTLINE_JOB_WRONG_WORKER;
    }
    if (attempt != job->attempt) {
        return FAULTLINE_JOB_STALE_ATTEMPT;
    }
    return FAULTLINE_JOB_OK;
}

enum faultline_job_result faultline_job_init(
    struct faultline_job *job, uint64_t id, enum faultline_task_type task_type,
    const uint8_t *arguments, size_t argument_size, uint32_t max_retries,
    int64_t now_ms)
{
    enum faultline_job_result result;

    if (job == NULL || id == 0 || now_ms < 0 ||
        task_type < FAULTLINE_TASK_SLEEP || task_type > FAULTLINE_TASK_HASH) {
        return FAULTLINE_JOB_INVALID_ARGUMENT;
    }
    result = check_payload(arguments, argument_size, sizeof(job->arguments));
    if (result != FAULTLINE_JOB_OK) {
        return result;
    }
    *job = (struct faultline_job){
        .id = id, .task_type = task_type, .argument_size = argument_size,
        .state = FAULTLINE_JOB_QUEUED, .max_retries = max_retries,
        .created_at_ms = now_ms, .updated_at_ms = now_ms,
        .assigned_at_ms = FAULTLINE_JOB_TIME_UNSET,
        .started_at_ms = FAULTLINE_JOB_TIME_UNSET,
        .finished_at_ms = FAULTLINE_JOB_TIME_UNSET
    };
    if (argument_size != 0) {
        memcpy(job->arguments, arguments, argument_size);
    }
    return FAULTLINE_JOB_OK;
}

enum faultline_job_result faultline_job_assign(
    struct faultline_job *job, uint32_t worker_id, int64_t now_ms)
{
    enum faultline_job_result result = check_transition(job, FAULTLINE_JOB_ASSIGNED, now_ms);

    if (result != FAULTLINE_JOB_OK) {
        return result;
    }
    if (worker_id == 0) {
        return FAULTLINE_JOB_INVALID_ARGUMENT;
    }
    job->state = FAULTLINE_JOB_ASSIGNED;
    job->worker_id = worker_id;
    /* At most UINT32_MAX retries plus one initial attempt: the uint64_t fits. */
    ++job->attempt;
    job->assigned_at_ms = now_ms;
    job->updated_at_ms = now_ms;
    job->failure = FAULTLINE_JOB_FAILURE_NONE;
    return FAULTLINE_JOB_OK;
}

enum faultline_job_result faultline_job_start(
    struct faultline_job *job, uint32_t worker_id, uint64_t attempt, int64_t now_ms)
{
    enum faultline_job_result result = check_transition(job, FAULTLINE_JOB_RUNNING, now_ms);

    if (result != FAULTLINE_JOB_OK) {
        return result;
    }
    result = check_attempt(job, worker_id, attempt);
    if (result != FAULTLINE_JOB_OK) {
        return result;
    }
    job->state = FAULTLINE_JOB_RUNNING;
    job->started_at_ms = now_ms;
    job->updated_at_ms = now_ms;
    return FAULTLINE_JOB_OK;
}

enum faultline_job_result faultline_job_complete(
    struct faultline_job *job, uint32_t worker_id, uint64_t attempt,
    const uint8_t *data, size_t result_size, int64_t now_ms)
{
    enum faultline_job_result result = check_transition(job, FAULTLINE_JOB_DONE, now_ms);

    if (result != FAULTLINE_JOB_OK) {
        return result;
    }
    result = check_attempt(job, worker_id, attempt);
    if (result != FAULTLINE_JOB_OK) {
        return result;
    }
    result = check_payload(data, result_size, sizeof(job->result));
    if (result != FAULTLINE_JOB_OK) {
        return result;
    }
    if (result_size != 0) {
        memcpy(job->result, data, result_size);
    }
    job->result_size = result_size;
    job->state = FAULTLINE_JOB_DONE;
    job->finished_at_ms = now_ms;
    job->updated_at_ms = now_ms;
    return FAULTLINE_JOB_OK;
}

enum faultline_job_result faultline_job_fail(
    struct faultline_job *job, uint32_t worker_id, uint64_t attempt,
    enum faultline_job_failure failure, int64_t now_ms)
{
    enum faultline_job_state next;
    enum faultline_job_result result;

    if (job == NULL || (failure != FAULTLINE_JOB_FAILURE_TASK &&
                        failure != FAULTLINE_JOB_FAILURE_WORKER_LOST)) {
        return FAULTLINE_JOB_INVALID_ARGUMENT;
    }
    next = job->retry_count < job->max_retries ? FAULTLINE_JOB_QUEUED : FAULTLINE_JOB_FAILED;
    result = check_transition(job, next, now_ms);
    if (result != FAULTLINE_JOB_OK) {
        return result;
    }
    result = check_attempt(job, worker_id, attempt);
    if (result != FAULTLINE_JOB_OK) {
        return result;
    }
    job->state = next;
    job->updated_at_ms = now_ms;
    job->failure = failure;
    if (next == FAULTLINE_JOB_QUEUED) {
        ++job->retry_count;
        job->worker_id = 0;
        job->assigned_at_ms = FAULTLINE_JOB_TIME_UNSET;
        job->started_at_ms = FAULTLINE_JOB_TIME_UNSET;
        job->finished_at_ms = FAULTLINE_JOB_TIME_UNSET;
    } else {
        job->finished_at_ms = now_ms;
    }
    return FAULTLINE_JOB_OK;
}
