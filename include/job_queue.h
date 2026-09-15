#ifndef FAULTLINE_JOB_QUEUE_H
#define FAULTLINE_JOB_QUEUE_H

#include "job.h"

/* Pending IDs only; the job store owns the full records separately. */
#define FAULTLINE_JOB_QUEUE_CAPACITY 256u

struct faultline_job_queue {
    uint64_t ids[FAULTLINE_JOB_QUEUE_CAPACITY];
    size_t head;  /* Array position of the oldest entry. */
    size_t count;
};

enum faultline_job_queue_result {
    FAULTLINE_JOB_QUEUE_OK = 0,
    FAULTLINE_JOB_QUEUE_INVALID_ARGUMENT,
    FAULTLINE_JOB_QUEUE_INVALID_STATE,
    FAULTLINE_JOB_QUEUE_DUPLICATE,
    FAULTLINE_JOB_QUEUE_FULL,
    FAULTLINE_JOB_QUEUE_EMPTY
};

/* Initialize non-null storage once before use; no heap allocation or I/O. */
void faultline_job_queue_init(struct faultline_job_queue *queue);

/*
 * Append an initialized QUEUED job with a nonzero ID and no assigned worker.
 * Copies only the ID, never a pointer or the job record. The caller owns the
 * record and must retain it separately for later lookup, assignment, and status.
 * One pending entry per ID; full queues reject input without dropping old work.
 * FIFO means successful insertion order, not numeric ID or timestamp order.
 */
enum faultline_job_queue_result faultline_job_queue_push(
    struct faultline_job_queue *queue, const struct faultline_job *job);

/* Read the oldest ID without removing it. */
enum faultline_job_queue_result faultline_job_queue_peek(
    const struct faultline_job_queue *queue, uint64_t *job_id);

/*
 * Remove and return the oldest ID; does not modify or destroy its job record.
 * The future scheduler should peek, find/assign the job, then pop only when
 * assignment succeeds. With no available worker, leave the queue untouched.
 */
enum faultline_job_queue_result faultline_job_queue_pop(
    struct faultline_job_queue *queue, uint64_t *job_id);

/*
 * The coordinator's single event loop owns the queue; there is no locking.
 * Treat its fields as read-only outside this API. Non-null pointers must address
 * valid, non-overlapping storage. Every failed call leaves queue, job, and output
 * unchanged. Resetting a nonempty queue would discard pending IDs.
 */

#endif
