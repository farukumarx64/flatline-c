#include "net.h"
#include "protocol.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_CLIENTS 64

enum client_phase { READING_PING, WRITING_PONG };

struct client {
    int fd;
    enum client_phase phase;
    uint8_t input[FAULTLINE_HEADER_SIZE];
    uint8_t output[FAULTLINE_HEADER_SIZE];
    size_t received;
    size_t sent;
    int64_t last_progress_ms;
};

static volatile sig_atomic_t stopping = 0;

static void request_stop(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

static void close_client(struct client *client)
{
    (void)close(client->fd);
    client->fd = -1;
}

static void read_ping(struct client *client, int64_t now)
{
    ssize_t count = recv(client->fd, client->input + client->received,
                            sizeof(client->input) - client->received, 0);
    struct faultline_header header;
    enum faultline_protocol_result result;

    if (count == 0) {
        if (client->received != 0) {
            fprintf(stderr, "[WARN] coordinator truncated_header fd=%d bytes=%zu\n",
                    client->fd, client->received);
        }
        close_client(client);
        return;
    }
    if (count < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            perror("coordinator: recv");
            close_client(client);
        }
        return;
    }
    client->received += (size_t)count;
    client->last_progress_ms = now;
    if (client->received < sizeof(client->input)) {
        return;
    }

    result = faultline_header_decode(client->input, sizeof(client->input), &header);
    if (result != FAULTLINE_PROTOCOL_OK) {
        fprintf(stderr, "[WARN] coordinator invalid_header fd=%d code=%d\n",
                client->fd, (int)result);
        close_client(client);
        return;
    }
    if (header.message_type != FAULTLINE_MSG_PING || header.payload_length != 0) {
        fprintf(stderr, "[WARN] coordinator expected_empty_ping fd=%d\n", client->fd);
        close_client(client);
        return;
    }
    header.message_type = FAULTLINE_MSG_PONG;
    if (faultline_header_encode(client->output, sizeof(client->output), &header) !=
        FAULTLINE_PROTOCOL_OK) {
        fputs("[ERROR] coordinator could not encode PONG\n", stderr);
        close_client(client);
        return;
    }
    client->sent = 0;
    client->phase = WRITING_PONG;
    printf("[INFO] coordinator ping_received fd=%d\n", client->fd);
}

static void write_pong(struct client *client, int64_t now)
{
    ssize_t count = send(client->fd, client->output + client->sent,
                         sizeof(client->output) - client->sent, 0);

    if (count < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            perror("coordinator: send");
            close_client(client);
        }
        return;
    }
    if (count == 0) {
        close_client(client);
        return;
    }
    client->sent += (size_t)count;
    client->last_progress_ms = now;
    if (client->sent == sizeof(client->output)) {
        printf("[INFO] coordinator pong_sent fd=%d\n", client->fd);
        client->received = 0;
        client->phase = READING_PING;
    }
}

static void accept_clients(int listener, struct client clients[MAX_CLIENTS],
                           int64_t now)
{
    /* Bound each batch so an incoming connection flood cannot monopolize it. */
    for (size_t accepted = 0; accepted < MAX_CLIENTS && !stopping; ++accepted) {
        size_t slot;
        int fd = accept(listener, NULL, NULL);

        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("coordinator: accept");
            }
            return;
        }
        for (slot = 0; slot < MAX_CLIENTS; ++slot) {
            if (clients[slot].fd < 0) {
                break;
            }
        }
        if (slot == MAX_CLIENTS) {
            fputs("[WARN] coordinator client_limit_reached\n", stderr);
            (void)close(fd);
        } else if (faultline_set_nonblocking(fd) < 0) {
            perror("coordinator: nonblocking client");
            (void)close(fd);
        } else {
            clients[slot] = (struct client){
                .fd = fd, .phase = READING_PING, .last_progress_ms = now
            };
            printf("[INFO] coordinator client_connected fd=%d\n", fd);
        }
    }
}

static int run_coordinator(int listener)
{
    struct client clients[MAX_CLIENTS];
    struct pollfd descriptors[MAX_CLIENTS + 1];
    int status = EXIT_SUCCESS;

    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        clients[i] = (struct client){.fd = -1};
    }
    while (!stopping) {
        int ready;
        int64_t now;

        descriptors[0] = (struct pollfd){.fd = listener, .events = POLLIN};
        for (size_t i = 0; i < MAX_CLIENTS; ++i) {
            descriptors[i + 1] = (struct pollfd){
                .fd = clients[i].fd,
                .events = clients[i].phase == READING_PING ? POLLIN : POLLOUT
            };
        }
        ready = poll(descriptors, MAX_CLIENTS + 1, 1000);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("coordinator: poll");
            status = EXIT_FAILURE;
            break;
        }
        now = faultline_monotonic_ms();
        if (now < 0) {
            perror("coordinator: clock");
            status = EXIT_FAILURE;
            break;
        }
        for (size_t i = 0; i < MAX_CLIENTS && !stopping; ++i) {
            short events = descriptors[i + 1].revents;

            if (clients[i].fd < 0) {
                continue;
            }
            if ((events & POLLNVAL) != 0) {
                close_client(&clients[i]);
                continue;
            }
            if (clients[i].phase == READING_PING &&
                (events & (POLLIN | POLLHUP | POLLERR)) != 0) {
                read_ping(&clients[i], now);
            } else if (clients[i].phase == WRITING_PONG &&
                       (events & (POLLOUT | POLLHUP | POLLERR)) != 0) {
                write_pong(&clients[i], now);
            }
            if (clients[i].fd >= 0 &&
                now - clients[i].last_progress_ms >= FAULTLINE_IO_TIMEOUT_MS) {
                fprintf(stderr, "[WARN] coordinator client_timeout fd=%d\n",
                        clients[i].fd);
                close_client(&clients[i]);
            }
        }
        if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            fputs("[ERROR] coordinator listener unavailable\n", stderr);
            status = EXIT_FAILURE;
            break;
        }
        if ((descriptors[0].revents & POLLIN) != 0) {
            accept_clients(listener, clients, now);
        }
    }
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i].fd >= 0) {
            close_client(&clients[i]);
        }
    }
    return status;
}

int main(int argc, char **argv)
{
    uint16_t port = FAULTLINE_DEFAULT_PORT;
    struct sigaction action = {0};
    int listener;
    int status;

    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        puts("Usage: faultline-coordinator [--port PORT]\nDefault: 127.0.0.1:9000");
        return EXIT_SUCCESS;
    }
    if (argc != 1 && (argc != 3 || strcmp(argv[1], "--port") != 0 ||
                      faultline_parse_port(argv[2], &port) < 0)) {
        fputs("Usage: faultline-coordinator [--port PORT] (1..65535)\n", stderr);
        return EXIT_FAILURE;
    }
    (void)setvbuf(stdout, NULL, _IOLBF, 0);
    action.sa_handler = request_stop;
    if (sigemptyset(&action.sa_mask) < 0 ||
        sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0 || faultline_ignore_sigpipe() < 0) {
        perror("coordinator: configure signals");
        return EXIT_FAILURE;
    }
    listener = faultline_listen(FAULTLINE_DEFAULT_HOST, port, MAX_CLIENTS);
    if (listener < 0) {
        perror("coordinator: listen");
        return EXIT_FAILURE;
    }
    printf("[INFO] coordinator listening address=%s port=%u\n",
           FAULTLINE_DEFAULT_HOST, (unsigned int)port);
    status = run_coordinator(listener);
    (void)close(listener);
    puts("[INFO] coordinator stopped");
    return status;
}
