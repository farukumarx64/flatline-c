#include "net.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static volatile sig_atomic_t stopping = 0;

static void request_stop(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

static void usage(FILE *stream)
{
    fprintf(stream, "Usage: faultline-worker [--coordinator IPv4:PORT] "
            "[--heartbeat-interval-ms MS]\n"
            "Default coordinator: 127.0.0.1:9000; heartbeat interval: %d ms\n"
            "MS must be a positive decimal integer in 1..INT_MAX.\n",
            FAULTLINE_DEFAULT_HEARTBEAT_INTERVAL_MS);
}

enum wait_result { WAIT_ERROR = -1, WAIT_STOP, WAIT_INPUT, WAIT_DEADLINE };

/* The same interruptible wait serves both ACK expiry and heartbeat scheduling. */
static int wait_for_input(int fd, int64_t deadline)
{
    struct pollfd descriptor = {.fd = fd, .events = POLLIN};

    while (!stopping) {
        int timeout = 250;
        int ready;

        if (deadline >= 0) {
            int64_t now = faultline_monotonic_ms();

            if (now < 0) {
                return -1;
            }
            if (now >= deadline) {
                return WAIT_DEADLINE;
            }
            if (deadline - now < timeout) {
                timeout = (int)(deadline - now);
            }
        }
        /* A bounded wait also handles a signal arriving just before poll(). */
        ready = poll(&descriptor, 1, timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if ((descriptor.revents & POLLNVAL) != 0) {
            errno = EBADF;
            return -1;
        }
        if ((descriptor.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            return stopping ? 0 : 1;
        }
    }
    return 0;
}

static int receive_registration_ack(int fd, uint32_t *worker_id)
{
    uint8_t wire[FAULTLINE_HEADER_SIZE + FAULTLINE_WORKER_REGISTER_ACK_PAYLOAD_SIZE];
    size_t received = 0;
    size_t expected = FAULTLINE_HEADER_SIZE;
    size_t consumed;
    struct faultline_message message;
    int64_t start = faultline_monotonic_ms();

    if (start < 0) {
        perror("worker: clock");
        return -1;
    }
    while (received < expected) {
        int ready = wait_for_input(fd, start + FAULTLINE_IO_TIMEOUT_MS);
        ssize_t count;

        if (ready == WAIT_DEADLINE) {
            errno = ETIMEDOUT;
            ready = WAIT_ERROR;
        }
        if (ready <= 0) {
            if (ready < 0) {
                perror("worker: receive registration ACK");
            }
            return -1;
        }
        count = recv(fd, wire + received, expected - received, 0);
        if (count == 0) {
            fprintf(stderr, "worker: coordinator closed %s registration ACK\n",
                    received == 0 ? "before" : "during");
            return -1;
        }
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            perror("worker: receive registration ACK");
            return -1;
        }
        received += (size_t)count;
        if (received == FAULTLINE_HEADER_SIZE) {
            struct faultline_header header;

            if (faultline_header_decode(wire, received, &header) != FAULTLINE_PROTOCOL_OK ||
                header.message_type != FAULTLINE_MSG_WORKER_REGISTER_ACK ||
                header.payload_length != FAULTLINE_WORKER_REGISTER_ACK_PAYLOAD_SIZE) {
                fputs("worker: expected registration ACK with a 4-byte worker ID\n", stderr);
                return -1;
            }
            expected = sizeof(wire);
        }
    }
    if (faultline_message_decode(wire, received, &message, &consumed) != FAULTLINE_PROTOCOL_OK) {
        fputs("worker: invalid registration ACK payload\n", stderr);
        return -1;
    }
    *worker_id = message.payload.worker_id;
    return 0;
}

static int run_worker(int fd, uint32_t worker_id, int interval_ms)
{
    const struct faultline_message heartbeat = {
        .message_type = FAULTLINE_MSG_HEARTBEAT, .payload.worker_id = worker_id
    };
    uint8_t wire[FAULTLINE_HEADER_SIZE + FAULTLINE_HEARTBEAT_PAYLOAD_SIZE];
    size_t written;
    uint8_t input[FAULTLINE_MESSAGE_MAX_FRAME_SIZE];
    size_t received = 0, expected = FAULTLINE_HEADER_SIZE;
    int64_t frame_started = -1;
    struct faultline_job_assign_payload active = {0};
    int64_t now = faultline_monotonic_ms();
    int64_t next_heartbeat;

    if (now < 0) {
        perror("worker: clock");
        return EXIT_FAILURE;
    }
    if (faultline_message_encode(wire, sizeof(wire), &heartbeat, &written) !=
        FAULTLINE_PROTOCOL_OK) {
        fputs("worker: could not encode heartbeat\n", stderr);
        return EXIT_FAILURE;
    }
    next_heartbeat = now + interval_ms;
    while (!stopping) {
        int64_t deadline = next_heartbeat;
        if (frame_started >= 0 && frame_started + FAULTLINE_IO_TIMEOUT_MS < deadline) {
            deadline = frame_started + FAULTLINE_IO_TIMEOUT_MS;
        }
        int ready = wait_for_input(fd, deadline);
        if (ready == WAIT_STOP) { break; }
        if (ready == WAIT_ERROR) {
            perror("worker: wait for coordinator");
            return EXIT_FAILURE;
        }
        now = faultline_monotonic_ms();
        if (now < 0) { perror("worker: clock"); return EXIT_FAILURE; }
        if (frame_started >= 0 && now - frame_started >= FAULTLINE_IO_TIMEOUT_MS) {
            fputs("worker: assignment receive timeout\n", stderr);
            return EXIT_FAILURE;
        }
        /* Input traffic and partial assignments never postpone heartbeats. */
        if (now >= next_heartbeat) {
            if (faultline_send_all(fd, wire, written, FAULTLINE_IO_TIMEOUT_MS) < 0) {
                perror("worker: send heartbeat");
                return EXIT_FAILURE;
            }
            now = faultline_monotonic_ms();
            if (now < 0) { perror("worker: clock"); return EXIT_FAILURE; }
            printf("[INFO] worker heartbeat_sent worker_id=%" PRIu32 "\n", worker_id);
            next_heartbeat = now + interval_ms;
            continue;
        }
        if (ready != WAIT_INPUT) { continue; }
        ssize_t count = recv(fd, input + received, expected - received, 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) { continue; }
        if (count <= 0) {
            if (count < 0) { perror("worker: coordinator connection"); }
            else { fputs("worker: coordinator disconnected\n", stderr); }
            return EXIT_FAILURE;
        }
        if (received == 0) { frame_started = now; }
        received += (size_t)count;
        const uint8_t prefix[] = {0x46, 0x4c, 0x49, 0x4e, 0, 1};
        size_t prefix_size = received < sizeof(prefix) ? received : sizeof(prefix);
        if (memcmp(input, prefix, prefix_size) != 0) {
            fputs("worker: unexpected data after registration\n", stderr);
            return EXIT_FAILURE;
        }
        if (received < expected) { continue; }
        if (received == FAULTLINE_HEADER_SIZE) {
            struct faultline_header header;
            if (faultline_header_decode(input, received, &header) != FAULTLINE_PROTOCOL_OK ||
                header.message_type != FAULTLINE_MSG_JOB_ASSIGN ||
                header.payload_length < FAULTLINE_JOB_ASSIGN_PREFIX_SIZE ||
                header.payload_length > sizeof(input) - FAULTLINE_HEADER_SIZE) {
                fputs("worker: unexpected data after registration; expected job assignment\n", stderr);
                return EXIT_FAILURE;
            }
            expected = FAULTLINE_HEADER_SIZE + (size_t)header.payload_length;
            continue;
        }
        struct faultline_message message;
        size_t consumed;
        if (faultline_message_decode(input, received, &message, &consumed) != FAULTLINE_PROTOCOL_OK ||
            message.payload.job_assign.identity.worker_id != worker_id || active.identity.job_id != 0) {
            fputs("worker: invalid assignment or worker already busy\n", stderr);
            return EXIT_FAILURE;
        }
        active = message.payload.job_assign;
        printf("[INFO] worker job_assigned job_id=%" PRIu64 " worker_id=%" PRIu32
               " attempt=%" PRIu64 " task_type=%u argument_bytes=%zu execution=pending\n",
               active.identity.job_id, worker_id, active.identity.attempt,
               (unsigned int)active.task_type, active.argument_size);
        /* Keep ownership and heartbeats until task executors can process active. */
        received = 0;
        expected = FAULTLINE_HEADER_SIZE;
        frame_started = -1;
    }
    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    char host[INET_ADDRSTRLEN] = FAULTLINE_DEFAULT_HOST;
    uint16_t port = FAULTLINE_DEFAULT_PORT;
    int interval_ms = FAULTLINE_DEFAULT_HEARTBEAT_INTERVAL_MS;
    int endpoint_seen = 0;
    int interval_seen = 0;
    uint32_t worker_id = FAULTLINE_WORKER_ID_UNASSIGNED;
    const struct faultline_message registration = {
        .message_type = FAULTLINE_MSG_WORKER_REGISTER, .payload.worker_id = 0
    };
    uint8_t wire[FAULTLINE_HEADER_SIZE];
    size_t written;
    struct sigaction action = {0};
    int fd;
    int status = EXIT_FAILURE;

    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(stdout);
        return EXIT_SUCCESS;
    }
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 < argc && strcmp(argv[i], "--coordinator") == 0 && !endpoint_seen &&
            faultline_parse_endpoint(argv[i + 1], host, sizeof(host), &port) == 0) {
            endpoint_seen = 1;
        } else if (i + 1 < argc && strcmp(argv[i], "--heartbeat-interval-ms") == 0 &&
                   !interval_seen && faultline_parse_duration_ms(argv[i + 1], &interval_ms) == 0) {
            interval_seen = 1;
        } else {
            usage(stderr);
            return EXIT_FAILURE;
        }
    }
    (void)setvbuf(stdout, NULL, _IOLBF, 0);
    action.sa_handler = request_stop;
    if (sigemptyset(&action.sa_mask) < 0 ||
        sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0 || faultline_ignore_sigpipe() < 0) {
        perror("worker: configure signals");
        return EXIT_FAILURE;
    }
    fd = faultline_connect(host, port, FAULTLINE_IO_TIMEOUT_MS);
    if (fd < 0) {
        if (stopping) {
            return EXIT_SUCCESS;
        }
        perror("worker: connect");
        return EXIT_FAILURE;
    }
    if (stopping) {
        goto done;
    }
    if (faultline_message_encode(wire, sizeof(wire), &registration, &written) !=
        FAULTLINE_PROTOCOL_OK) {
        fputs("worker: could not encode registration\n", stderr);
        goto done;
    }
    if (faultline_send_all(fd, wire, written, FAULTLINE_IO_TIMEOUT_MS) < 0) {
        perror("worker: send registration");
        goto done;
    }
    if (receive_registration_ack(fd, &worker_id) < 0 || stopping) {
        goto done;
    }
    printf("[INFO] worker registered worker_id=%" PRIu32
           " coordinator=%s:%u heartbeat_interval_ms=%d\n",
           worker_id, host, (unsigned int)port, interval_ms);
    status = run_worker(fd, worker_id, interval_ms);

done:
    (void)close(fd);
    return stopping ? EXIT_SUCCESS : status;
}
