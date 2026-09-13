#include "worker_registry.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,   \
                    #condition);                                             \
            return EXIT_FAILURE;                                             \
        }                                                                    \
    } while (0)

static int test_registration_and_lookup(void)
{
    struct faultline_worker_registry registry;
    uint32_t first = 0;
    uint32_t second = 0;

    faultline_worker_registry_init(&registry);
    for (size_t i = 0; i < FAULTLINE_MAX_WORKERS; ++i) {
        CHECK(registry.workers[i].state == FAULTLINE_WORKER_UNUSED);
        CHECK(registry.workers[i].id == 0);
        CHECK(registry.workers[i].fd == -1);
    }
    CHECK(faultline_worker_find(&registry, 0) == NULL);
    CHECK(faultline_worker_find(&registry, 1) == NULL);
    CHECK(faultline_worker_register(&registry, 7, 1000, &first) == FAULTLINE_REGISTRY_OK);
    CHECK(faultline_worker_register(&registry, 11, 2000, &second) == FAULTLINE_REGISTRY_OK);
    CHECK(first == 1 && second == 2);
    const struct faultline_worker *worker = faultline_worker_find(&registry, first);
    CHECK(worker != NULL);
    CHECK(worker->id == first && worker->fd == 7);
    CHECK(worker->state == FAULTLINE_WORKER_ALIVE && worker->last_heartbeat_ms == 1000);
    CHECK(faultline_worker_find(&registry, second)->fd == 11);
    return EXIT_SUCCESS;
}

static int test_heartbeat_ownership_and_time(void)
{
    struct faultline_worker_registry registry;
    uint32_t id = 0;

    faultline_worker_registry_init(&registry);
    CHECK(faultline_worker_register(&registry, 7, 1000, &id) == FAULTLINE_REGISTRY_OK);
    CHECK(faultline_worker_heartbeat(&registry, id, 7, 1200) == FAULTLINE_REGISTRY_OK);
    CHECK(faultline_worker_heartbeat(&registry, id, 7, 1200) == FAULTLINE_REGISTRY_OK);
    CHECK(faultline_worker_heartbeat(&registry, id, 8, 1400) ==
          FAULTLINE_REGISTRY_CONNECTION_MISMATCH);
    CHECK(faultline_worker_heartbeat(&registry, id + 1, 7, 1400) ==
          FAULTLINE_REGISTRY_NOT_FOUND);
    CHECK(faultline_worker_heartbeat(&registry, 0, 7, 1400) == FAULTLINE_REGISTRY_NOT_FOUND);
    CHECK(faultline_worker_heartbeat(&registry, id, 7, 1199) ==
          FAULTLINE_REGISTRY_INVALID_ARGUMENT);
    const struct faultline_worker *worker = faultline_worker_find(&registry, id);
    CHECK(worker->last_heartbeat_ms == 1200);
    CHECK(worker->fd == 7 && worker->state == FAULTLINE_WORKER_ALIVE);
    return EXIT_SUCCESS;
}

static int test_disconnect_and_descriptor_reuse(void)
{
    struct faultline_worker_registry registry;
    uint32_t old_id = 0;
    uint32_t new_id = 0;

    faultline_worker_registry_init(&registry);
    CHECK(faultline_worker_register(&registry, 7, 1000, &old_id) == FAULTLINE_REGISTRY_OK);
    CHECK(faultline_worker_mark_dead(&registry, old_id, 8) ==
          FAULTLINE_REGISTRY_CONNECTION_MISMATCH);
    CHECK(faultline_worker_find(&registry, old_id)->state == FAULTLINE_WORKER_ALIVE);
    CHECK(faultline_worker_mark_dead(&registry, old_id, 7) == FAULTLINE_REGISTRY_OK);
    const struct faultline_worker *dead = faultline_worker_find(&registry, old_id);
    CHECK(dead != NULL && dead->state == FAULTLINE_WORKER_DEAD);
    CHECK(dead->fd == -1 && dead->last_heartbeat_ms == 1000);
    CHECK(faultline_worker_heartbeat(&registry, old_id, 7, 2000) == FAULTLINE_REGISTRY_NOT_ALIVE);
    CHECK(faultline_worker_mark_dead(&registry, old_id, 7) == FAULTLINE_REGISTRY_NOT_ALIVE);

    /* Deliberately reuse descriptor 7; no dependency on the OS's allocation order. */
    CHECK(faultline_worker_register(&registry, 7, 3000, &new_id) == FAULTLINE_REGISTRY_OK);
    CHECK(new_id == old_id + 1);
    CHECK(faultline_worker_find(&registry, old_id) == NULL);
    CHECK(faultline_worker_heartbeat(&registry, old_id, 7, 4000) == FAULTLINE_REGISTRY_NOT_FOUND);
    CHECK(faultline_worker_mark_dead(&registry, old_id, 7) == FAULTLINE_REGISTRY_NOT_FOUND);
    const struct faultline_worker *current = faultline_worker_find(&registry, new_id);
    CHECK(current != NULL && current->state == FAULTLINE_WORKER_ALIVE);
    CHECK(current->fd == 7 && current->last_heartbeat_ms == 3000);
    return EXIT_SUCCESS;
}

static int test_capacity_duplicates_and_churn(void)
{
    struct faultline_worker_registry registry;
    uint32_t ids[FAULTLINE_MAX_WORKERS];
    uint32_t output = 999;

    faultline_worker_registry_init(&registry);
    for (size_t i = 0; i < FAULTLINE_MAX_WORKERS; ++i) {
        CHECK(faultline_worker_register(&registry, 100 + (int)i, 1000, &ids[i]) ==
              FAULTLINE_REGISTRY_OK);
        CHECK(ids[i] == i + 1);
    }
    CHECK(faultline_worker_register(&registry, 100, 2000, &output) ==
          FAULTLINE_REGISTRY_ALREADY_REGISTERED);
    CHECK(faultline_worker_register(&registry, 900, 2000, &output) == FAULTLINE_REGISTRY_FULL);
    CHECK(output == 999 && registry.next_worker_id == FAULTLINE_MAX_WORKERS + 1);
    CHECK(faultline_worker_find(&registry, ids[0])->last_heartbeat_ms == 1000);

    uint32_t current_id = ids[0];
    for (size_t i = 0; i < FAULTLINE_MAX_WORKERS * 2; ++i) {
        CHECK(faultline_worker_mark_dead(&registry, current_id, 100) == FAULTLINE_REGISTRY_OK);
        CHECK(faultline_worker_register(&registry, 100, 3000, &output) == FAULTLINE_REGISTRY_OK);
        CHECK(output == FAULTLINE_MAX_WORKERS + 1 + i);
        CHECK(output != current_id);
        current_id = output;
    }
    for (size_t i = 1; i < FAULTLINE_MAX_WORKERS; ++i) {
        CHECK(faultline_worker_find(&registry, ids[i])->state == FAULTLINE_WORKER_ALIVE);
        CHECK(faultline_worker_find(&registry, ids[i])->fd == 100 + (int)i);
    }
    return EXIT_SUCCESS;
}

static int test_id_exhaustion(void)
{
    struct faultline_worker_registry registry;
    uint32_t id = 0;

    faultline_worker_registry_init(&registry);
    /* Simulate the final allocation without registering billions of workers. */
    registry.next_worker_id = UINT32_MAX;
    CHECK(faultline_worker_register(&registry, 7, 1000, &id) == FAULTLINE_REGISTRY_OK);
    CHECK(id == UINT32_MAX && registry.next_worker_id == 0);
    CHECK(faultline_worker_mark_dead(&registry, id, 7) == FAULTLINE_REGISTRY_OK);
    CHECK(faultline_worker_register(&registry, 7, 2000, &id) == FAULTLINE_REGISTRY_ID_EXHAUSTED);
    CHECK(id == UINT32_MAX && registry.next_worker_id == 0);
    CHECK(faultline_worker_find(&registry, UINT32_MAX)->state == FAULTLINE_WORKER_DEAD);
    CHECK(faultline_worker_find(&registry, 1) == NULL);
    return EXIT_SUCCESS;
}

static int test_invalid_arguments(void)
{
    struct faultline_worker_registry registry;
    uint32_t id = 99;

    faultline_worker_registry_init(&registry);
    CHECK(faultline_worker_register(NULL, 7, 1000, &id) == FAULTLINE_REGISTRY_INVALID_ARGUMENT);
    CHECK(faultline_worker_register(&registry, -1, 1000, &id) == FAULTLINE_REGISTRY_INVALID_ARGUMENT);
    CHECK(faultline_worker_register(&registry, 7, -1, &id) == FAULTLINE_REGISTRY_INVALID_ARGUMENT);
    CHECK(faultline_worker_register(&registry, 7, 1000, NULL) == FAULTLINE_REGISTRY_INVALID_ARGUMENT);
    CHECK(id == 99 && registry.next_worker_id == 1);
    CHECK(faultline_worker_find(NULL, 1) == NULL);
    CHECK(faultline_worker_find(&registry, 1) == NULL);
    CHECK(faultline_worker_register(&registry, 0, 0, &id) == FAULTLINE_REGISTRY_OK);
    CHECK(id == 1);
    CHECK(faultline_worker_heartbeat(NULL, id, 0, 1) == FAULTLINE_REGISTRY_INVALID_ARGUMENT);
    CHECK(faultline_worker_heartbeat(&registry, id, -1, 1) == FAULTLINE_REGISTRY_INVALID_ARGUMENT);
    CHECK(faultline_worker_heartbeat(&registry, id, 0, -1) == FAULTLINE_REGISTRY_INVALID_ARGUMENT);
    CHECK(faultline_worker_mark_dead(NULL, id, 0) == FAULTLINE_REGISTRY_INVALID_ARGUMENT);
    CHECK(faultline_worker_mark_dead(&registry, id, -1) == FAULTLINE_REGISTRY_INVALID_ARGUMENT);
    CHECK(faultline_worker_mark_dead(&registry, 999, 0) == FAULTLINE_REGISTRY_NOT_FOUND);
    CHECK(faultline_worker_find(&registry, id)->last_heartbeat_ms == 0);
    CHECK(faultline_worker_find(&registry, id)->state == FAULTLINE_WORKER_ALIVE);
    return EXIT_SUCCESS;
}

int main(void)
{
    const struct {
        const char *name;
        int (*run)(void);
    } tests[] = {
        {"worker registration and lookup", test_registration_and_lookup},
        {"heartbeat ownership and monotonic time", test_heartbeat_ownership_and_time},
        {"disconnect and descriptor reuse", test_disconnect_and_descriptor_reuse},
        {"registry capacity, duplicates, and churn", test_capacity_duplicates_and_churn},
        {"worker ID exhaustion", test_id_exhaustion},
        {"registry invalid arguments", test_invalid_arguments}
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
