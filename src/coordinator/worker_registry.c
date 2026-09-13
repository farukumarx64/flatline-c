#include "worker_registry.h"
#include "protocol.h"

#include <stddef.h>

void faultline_worker_registry_init(struct faultline_worker_registry *registry)
{
    *registry = (struct faultline_worker_registry){.next_worker_id = 1};
    for (size_t i = 0; i < FAULTLINE_MAX_WORKERS; ++i) {
        registry->workers[i].fd = -1;
    }
}

static size_t find_slot(const struct faultline_worker_registry *registry,
                        uint32_t worker_id)
{
    for (size_t i = 0; i < FAULTLINE_MAX_WORKERS; ++i) {
        if (registry->workers[i].state != FAULTLINE_WORKER_UNUSED &&
            registry->workers[i].id == worker_id) {
            return i;
        }
    }
    return FAULTLINE_MAX_WORKERS;
}

const struct faultline_worker *faultline_worker_find(
    const struct faultline_worker_registry *registry, uint32_t worker_id)
{
    size_t slot;

    if (registry == NULL || worker_id == FAULTLINE_WORKER_ID_UNASSIGNED) {
        return NULL;
    }
    slot = find_slot(registry, worker_id);
    return slot == FAULTLINE_MAX_WORKERS ? NULL : &registry->workers[slot];
}

enum faultline_registry_result faultline_worker_register(
    struct faultline_worker_registry *registry, int fd, int64_t now_ms,
    uint32_t *worker_id)
{
    size_t slot = FAULTLINE_MAX_WORKERS;

    if (registry == NULL || worker_id == NULL || fd < 0 || now_ms < 0) {
        return FAULTLINE_REGISTRY_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < FAULTLINE_MAX_WORKERS; ++i) {
        const struct faultline_worker *worker = &registry->workers[i];

        if (worker->state == FAULTLINE_WORKER_ALIVE && worker->fd == fd) {
            return FAULTLINE_REGISTRY_ALREADY_REGISTERED;
        }
        if (worker->state != FAULTLINE_WORKER_ALIVE && slot == FAULTLINE_MAX_WORKERS) {
            slot = i;
        }
    }
    if (slot == FAULTLINE_MAX_WORKERS) {
        return FAULTLINE_REGISTRY_FULL;
    }
    if (registry->next_worker_id == FAULTLINE_WORKER_ID_UNASSIGNED) {
        return FAULTLINE_REGISTRY_ID_EXHAUSTED;
    }

    *worker_id = registry->next_worker_id;
    registry->workers[slot] = (struct faultline_worker){
        .id = *worker_id, .fd = fd, .state = FAULTLINE_WORKER_ALIVE,
        .last_heartbeat_ms = now_ms
    };
    registry->next_worker_id = *worker_id == UINT32_MAX ?
        FAULTLINE_WORKER_ID_UNASSIGNED : *worker_id + UINT32_C(1);
    return FAULTLINE_REGISTRY_OK;
}

static enum faultline_registry_result find_live_connection(
    struct faultline_worker_registry *registry, uint32_t worker_id, int fd,
    struct faultline_worker **worker)
{
    size_t slot;

    if (registry == NULL || fd < 0) {
        return FAULTLINE_REGISTRY_INVALID_ARGUMENT;
    }
    slot = find_slot(registry, worker_id);
    if (slot == FAULTLINE_MAX_WORKERS) {
        return FAULTLINE_REGISTRY_NOT_FOUND;
    }
    if (registry->workers[slot].state != FAULTLINE_WORKER_ALIVE) {
        return FAULTLINE_REGISTRY_NOT_ALIVE;
    }
    if (registry->workers[slot].fd != fd) {
        return FAULTLINE_REGISTRY_CONNECTION_MISMATCH;
    }
    *worker = &registry->workers[slot];
    return FAULTLINE_REGISTRY_OK;
}

enum faultline_registry_result faultline_worker_heartbeat(
    struct faultline_worker_registry *registry, uint32_t worker_id, int fd,
    int64_t now_ms)
{
    struct faultline_worker *worker;
    enum faultline_registry_result result;

    if (now_ms < 0) {
        return FAULTLINE_REGISTRY_INVALID_ARGUMENT;
    }
    result = find_live_connection(registry, worker_id, fd, &worker);
    if (result != FAULTLINE_REGISTRY_OK) {
        return result;
    }
    if (now_ms < worker->last_heartbeat_ms) {
        return FAULTLINE_REGISTRY_INVALID_ARGUMENT;
    }
    worker->last_heartbeat_ms = now_ms;
    return FAULTLINE_REGISTRY_OK;
}

enum faultline_registry_result faultline_worker_mark_dead(
    struct faultline_worker_registry *registry, uint32_t worker_id, int fd)
{
    struct faultline_worker *worker;
    enum faultline_registry_result result =
        find_live_connection(registry, worker_id, fd, &worker);

    if (result != FAULTLINE_REGISTRY_OK) {
        return result;
    }
    worker->state = FAULTLINE_WORKER_DEAD;
    worker->fd = -1;
    return FAULTLINE_REGISTRY_OK;
}
