#include "job_queue.h"

void faultline_job_queue_init(struct faultline_job_queue *queue)
{
    *queue = (struct faultline_job_queue){0};
}

enum faultline_job_queue_result faultline_job_queue_push(
    struct faultline_job_queue *queue, const struct faultline_job *job)
{
    size_t tail;

    if (queue == NULL || job == NULL || job->id == 0) {
        return FAULTLINE_JOB_QUEUE_INVALID_ARGUMENT;
    }
    if (job->state != FAULTLINE_JOB_QUEUED || job->worker_id != 0) {
        return FAULTLINE_JOB_QUEUE_INVALID_STATE;
    }
    for (size_t i = 0; i < queue->count; ++i) {
        size_t slot = (queue->head + i) % FAULTLINE_JOB_QUEUE_CAPACITY;

        if (queue->ids[slot] == job->id) {
            return FAULTLINE_JOB_QUEUE_DUPLICATE;
        }
    }
    if (queue->count == FAULTLINE_JOB_QUEUE_CAPACITY) {
        return FAULTLINE_JOB_QUEUE_FULL;
    }
    tail = (queue->head + queue->count) % FAULTLINE_JOB_QUEUE_CAPACITY;
    queue->ids[tail] = job->id;
    ++queue->count;
    return FAULTLINE_JOB_QUEUE_OK;
}

enum faultline_job_queue_result faultline_job_queue_peek(
    const struct faultline_job_queue *queue, uint64_t *job_id)
{
    if (queue == NULL || job_id == NULL) {
        return FAULTLINE_JOB_QUEUE_INVALID_ARGUMENT;
    }
    if (queue->count == 0) {
        return FAULTLINE_JOB_QUEUE_EMPTY;
    }
    *job_id = queue->ids[queue->head];
    return FAULTLINE_JOB_QUEUE_OK;
}

enum faultline_job_queue_result faultline_job_queue_pop(
    struct faultline_job_queue *queue, uint64_t *job_id)
{
    enum faultline_job_queue_result result = faultline_job_queue_peek(queue, job_id);

    if (result != FAULTLINE_JOB_QUEUE_OK) {
        return result;
    }
    queue->ids[queue->head] = 0;
    queue->head = (queue->head + 1) % FAULTLINE_JOB_QUEUE_CAPACITY;
    --queue->count;
    return FAULTLINE_JOB_QUEUE_OK;
}
