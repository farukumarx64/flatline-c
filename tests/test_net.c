#include "net.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,      \
                    #condition);                                                \
            return EXIT_FAILURE;                                                \
        }                                                                       \
    } while (0)

static int make_pair(int pair[2])
{
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0) {
        return -1;
    }
    if (faultline_set_nonblocking(pair[0]) < 0) {
        (void)close(pair[0]);
        (void)close(pair[1]);
        return -1;
    }
    return 0;
}

static int wait_child(pid_t child)
{
    int status;

    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS ? 0 : -1;
}

static int test_fragmented_receive(void)
{
    static const uint8_t expected[] = "fragmented";
    uint8_t actual[sizeof(expected)];
    int pair[2];
    pid_t child;
    enum faultline_receive_result result;

    CHECK(make_pair(pair) == 0);
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        const struct timespec delay = {.tv_nsec = 1000000};

        (void)close(pair[0]);
        if (faultline_set_nonblocking(pair[1]) < 0) {
            _exit(EXIT_FAILURE);
        }
        for (size_t i = 0; i < sizeof(expected); ++i) {
            if (faultline_send_all(pair[1], expected + i, 1, 1000) < 0) {
                _exit(EXIT_FAILURE);
            }
            (void)nanosleep(&delay, NULL);
        }
        (void)close(pair[1]);
        _exit(EXIT_SUCCESS);
    }
    (void)close(pair[1]);
    result = faultline_recv_exact(pair[0], actual, sizeof(actual), 2000);
    (void)close(pair[0]);
    CHECK(wait_child(child) == 0);
    CHECK(result == FAULTLINE_RECEIVE_OK);
    CHECK(memcmp(actual, expected, sizeof(expected)) == 0);
    return EXIT_SUCCESS;
}

static int test_eof_and_truncation(void)
{
    int pair[2];
    uint8_t data[6] = {0};

    CHECK(make_pair(pair) == 0);
    CHECK(write(pair[1], "abc", 3) == 3);
    (void)close(pair[1]);
    CHECK(faultline_recv_exact(pair[0], data, sizeof(data), 1000) ==
          FAULTLINE_RECEIVE_TRUNCATED);
    CHECK(memcmp(data, "abc", 3) == 0);
    CHECK(faultline_recv_exact(pair[0], data, sizeof(data), 1000) ==
          FAULTLINE_RECEIVE_EOF);
    (void)close(pair[0]);
    return EXIT_SUCCESS;
}

static int test_receive_deadline(void)
{
    int pair[2];
    pid_t child;
    uint8_t data[8];
    enum faultline_receive_result result;
    int error;

    CHECK(make_pair(pair) == 0);
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        const struct timespec delay = {.tv_nsec = 30000000};

        (void)close(pair[0]);
        for (size_t i = 0; i < sizeof(data); ++i) {
            (void)nanosleep(&delay, NULL);
            if (write(pair[1], "x", 1) != 1) {
                break;
            }
        }
        (void)close(pair[1]);
        _exit(EXIT_SUCCESS);
    }
    (void)close(pair[1]);
    /* Progress arrives every 30 ms; the 80 ms deadline must not restart. */
    result = faultline_recv_exact(pair[0], data, sizeof(data), 80);
    error = errno;
    (void)close(pair[0]);
    CHECK(wait_child(child) == 0);
    CHECK(result == FAULTLINE_RECEIVE_ERROR);
    CHECK(error == ETIMEDOUT);
    return EXIT_SUCCESS;
}

static int test_send_all_with_backpressure(void)
{
    const size_t size = 256u * 1024u;
    uint8_t *data = malloc(size);
    int pair[2];
    int send_buffer = 1024;
    pid_t child;
    int result;

    CHECK(data != NULL);
    for (size_t i = 0; i < size; ++i) {
        data[i] = (uint8_t)(i % 251u);
    }
    CHECK(make_pair(pair) == 0);
    CHECK(setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &send_buffer,
                     sizeof(send_buffer)) == 0);
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        uint8_t chunk[127];
        size_t received = 0;

        (void)close(pair[0]);
        /* Independently verify the stream using raw recv, not recv_exact. */
        while (received < size) {
            ssize_t count = recv(pair[1], chunk, sizeof(chunk), 0);

            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0 || (size_t)count > size - received) {
                _exit(EXIT_FAILURE);
            }
            for (size_t i = 0; i < (size_t)count; ++i) {
                if (chunk[i] != (uint8_t)((received + i) % 251u)) {
                    _exit(EXIT_FAILURE);
                }
            }
            received += (size_t)count;
        }
        (void)close(pair[1]);
        _exit(EXIT_SUCCESS);
    }
    (void)close(pair[1]);
    result = faultline_send_all(pair[0], data, size, 5000);
    free(data);
    (void)close(pair[0]);
    CHECK(wait_child(child) == 0);
    CHECK(result == 0);
    return EXIT_SUCCESS;
}

static int test_send_deadline(void)
{
    int pair[2];
    uint8_t data[1024] = {0};

    CHECK(make_pair(pair) == 0);
    for (;;) {
        ssize_t count = send(pair[0], data, sizeof(data), 0);

        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        CHECK(count > 0);
    }
    CHECK(faultline_send_all(pair[0], data, sizeof(data), 50) == -1);
    CHECK(errno == ETIMEDOUT);
    (void)close(pair[0]);
    (void)close(pair[1]);
    return EXIT_SUCCESS;
}

static int test_disconnected_send(void)
{
    int pair[2];
    const uint8_t data[] = "PING";

    CHECK(make_pair(pair) == 0);
    (void)close(pair[1]);
    CHECK(faultline_send_all(pair[0], data, sizeof(data), 1000) == -1);
    CHECK(errno == EPIPE || errno == ECONNRESET);
    (void)close(pair[0]);
    return EXIT_SUCCESS;
}

int main(void)
{
    const struct {
        const char *name;
        int (*run)(void);
    } tests[] = {
        {"fragmented receive", test_fragmented_receive},
        {"EOF and truncation", test_eof_and_truncation},
        {"total receive deadline", test_receive_deadline},
        {"send all with backpressure", test_send_all_with_backpressure},
        {"send deadline", test_send_deadline},
        {"disconnected send without SIGPIPE termination", test_disconnected_send}
    };

    CHECK(faultline_ignore_sigpipe() == 0);
    (void)alarm(20);
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        if (tests[i].run() != EXIT_SUCCESS) {
            fprintf(stderr, "FAIL: %s\n", tests[i].name);
            return EXIT_FAILURE;
        }
        printf("PASS: %s\n", tests[i].name);
    }
    (void)alarm(0);
    return EXIT_SUCCESS;
}
