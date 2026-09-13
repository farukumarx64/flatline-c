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
    fputs("Usage: faultline-worker [--coordinator IPv4:PORT]\n"
          "Default coordinator: 127.0.0.1:9000\n", stream);
}

/* Return 1 for socket activity, 0 for a stop request, or -1 on error/timeout. */
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
                errno = ETIMEDOUT;
                return -1;
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
    *worker_id = message.worker_id;
    return 0;
}

static int wait_until_stopped(int fd)
{
    while (!stopping) {
        int ready = wait_for_input(fd, -1);
        uint8_t byte;
        ssize_t count;

        if (ready == 0) {
            break;
        }
        if (ready < 0) {
            perror("worker: wait for coordinator");
            return EXIT_FAILURE;
        }
        count = recv(fd, &byte, 1, 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        if (count < 0) {
            perror("worker: coordinator connection");
        } else if (count == 0) {
            fputs("worker: coordinator disconnected\n", stderr);
        } else {
            /* Job messages are not implemented yet; do not silently discard data. */
            fputs("worker: unexpected data after registration\n", stderr);
        }
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    char host[INET_ADDRSTRLEN] = FAULTLINE_DEFAULT_HOST;
    uint16_t port = FAULTLINE_DEFAULT_PORT;
    uint32_t worker_id = FAULTLINE_WORKER_ID_UNASSIGNED;
    const struct faultline_message registration = {FAULTLINE_MSG_WORKER_REGISTER, 0};
    uint8_t wire[FAULTLINE_HEADER_SIZE];
    size_t written;
    struct sigaction action = {0};
    int fd;
    int status = EXIT_FAILURE;

    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(stdout);
        return EXIT_SUCCESS;
    }
    if (argc != 1 && (argc != 3 || strcmp(argv[1], "--coordinator") != 0 ||
                      faultline_parse_endpoint(argv[2], host, sizeof(host), &port) < 0)) {
        usage(stderr);
        return EXIT_FAILURE;
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
    printf("[INFO] worker registered worker_id=%" PRIu32 " coordinator=%s:%u\n",
           worker_id, host, (unsigned int)port);
    status = wait_until_stopped(fd);

done:
    (void)close(fd);
    return stopping ? EXIT_SUCCESS : status;
}
