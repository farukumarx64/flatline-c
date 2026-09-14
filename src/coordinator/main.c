#include "net.h"
#include "protocol.h"
#include "worker_registry.h"

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_CLIENTS 64
#define CLIENT_FRAME_CAPACITY (FAULTLINE_HEADER_SIZE + FAULTLINE_WORKER_REGISTER_ACK_PAYLOAD_SIZE)

enum client_phase { READING_MESSAGE, WRITING_REPLY };

struct client {
    int fd;
    enum client_phase phase;
    uint32_t worker_id;
    uint8_t input[CLIENT_FRAME_CAPACITY];
    uint8_t output[CLIENT_FRAME_CAPACITY];
    size_t received;
    size_t expected;
    size_t sent;
    size_t output_size;
    uint16_t reply_type;
    int64_t last_progress_ms;
};

static volatile sig_atomic_t stopping = 0;

static void request_stop(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

static void close_client(struct client *client,
                          struct faultline_worker_registry *registry,
                          const char *reason)
{
    if (client->worker_id != FAULTLINE_WORKER_ID_UNASSIGNED) {
        enum faultline_registry_result result =
            faultline_worker_mark_dead(registry, client->worker_id, client->fd);

        if (result == FAULTLINE_REGISTRY_OK) {
            const struct faultline_worker *worker =
                faultline_worker_find(registry, client->worker_id);
            printf("[INFO] coordinator worker_dead worker_id=%" PRIu32
                   " fd=%d state=DEAD last_heartbeat_ms=%" PRId64 " reason=%s\n",
                   worker->id, client->fd, worker->last_heartbeat_ms, reason);
        } else {
            fprintf(stderr, "[ERROR] coordinator registry_disconnect_failed code=%d\n",
                    (int)result);
        }
    }
    /* Detach the worker before close() allows the OS to reuse this descriptor. */
    (void)close(client->fd);
    client->fd = -1;
    client->worker_id = FAULTLINE_WORKER_ID_UNASSIGNED;
}

static void reset_input(struct client *client)
{
    client->received = 0;
    client->expected = FAULTLINE_HEADER_SIZE;
    client->phase = READING_MESSAGE;
}

static void queue_reply(struct client *client,
                         struct faultline_worker_registry *registry,
                         uint16_t message_type, uint32_t worker_id)
{
    const struct faultline_message reply = {message_type, worker_id};

    if (faultline_message_encode(client->output, sizeof(client->output), &reply,
                                 &client->output_size) != FAULTLINE_PROTOCOL_OK) {
        fputs("[ERROR] coordinator could not encode reply\n", stderr);
        close_client(client, registry, "encode_error");
        return;
    }
    client->sent = 0;
    client->reply_type = message_type;
    client->phase = WRITING_REPLY;
}

static void handle_message(struct client *client,
                            struct faultline_worker_registry *registry,
                            const struct faultline_message *message, int64_t now)
{
    enum faultline_registry_result result;

    switch (message->message_type) {
    case FAULTLINE_MSG_PING:
        printf("[INFO] coordinator ping_received fd=%d\n", client->fd);
        queue_reply(client, registry, FAULTLINE_MSG_PONG, 0);
        break;
    case FAULTLINE_MSG_WORKER_REGISTER:
        result = faultline_worker_register(registry, client->fd, now, &client->worker_id);
        if (result != FAULTLINE_REGISTRY_OK) {
            fprintf(stderr, "[WARN] coordinator registration_rejected fd=%d code=%d\n",
                    client->fd, (int)result);
            close_client(client, registry, "registration_rejected");
            return;
        }
        printf("[INFO] coordinator worker_registered worker_id=%" PRIu32
               " fd=%d state=ALIVE last_heartbeat_ms=%" PRId64 "\n",
               client->worker_id, client->fd, now);
        queue_reply(client, registry, FAULTLINE_MSG_WORKER_REGISTER_ACK, client->worker_id);
        break;
    case FAULTLINE_MSG_HEARTBEAT:
        if (client->worker_id == FAULTLINE_WORKER_ID_UNASSIGNED ||
            message->worker_id != client->worker_id) {
            close_client(client, registry, "heartbeat_identity_mismatch");
            return;
        }
        result = faultline_worker_heartbeat(registry, message->worker_id, client->fd, now);
        if (result != FAULTLINE_REGISTRY_OK) {
            close_client(client, registry, "heartbeat_rejected");
            return;
        }
        printf("[INFO] coordinator heartbeat_received worker_id=%" PRIu32
               " fd=%d last_heartbeat_ms=%" PRId64 "\n",
               client->worker_id, client->fd, now);
        reset_input(client);
        break;
    default:
        close_client(client, registry, "unexpected_message");
        break;
    }
}

static void read_message(struct client *client,
                          struct faultline_worker_registry *registry, int64_t now)
{
    ssize_t count = recv(client->fd, client->input + client->received,
                            client->expected - client->received, 0);
    struct faultline_message message;
    enum faultline_protocol_result result;
    size_t consumed;

    if (count == 0) {
        if (client->received != 0) {
            fprintf(stderr, "[WARN] coordinator truncated_message fd=%d bytes=%zu\n",
                    client->fd, client->received);
        }
        close_client(client, registry, client->received == 0 ? "eof" : "truncated_message");
        return;
    }
    if (count < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            perror("coordinator: recv");
            close_client(client, registry, "recv_error");
        }
        return;
    }
    client->received += (size_t)count;
    client->last_progress_ms = now;
    if (client->received < client->expected) {
        return;
    }

    result = faultline_message_decode(client->input, client->received, &message, &consumed);
    if (result == FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL) {
        struct faultline_header header;

        /* The complete header declares a valid payload we have not read yet. */
        if (faultline_header_decode(client->input, client->received, &header) !=
            FAULTLINE_PROTOCOL_OK ||
            header.payload_length > sizeof(client->input) - FAULTLINE_HEADER_SIZE) {
            close_client(client, registry, "unsupported_payload");
            return;
        }
        client->expected = FAULTLINE_HEADER_SIZE + (size_t)header.payload_length;
        return;
    }
    if (result != FAULTLINE_PROTOCOL_OK) {
        fprintf(stderr, "[WARN] coordinator invalid_message fd=%d code=%d\n",
                client->fd, (int)result);
        close_client(client, registry, "invalid_message");
        return;
    }
    handle_message(client, registry, &message, now);
}

static void write_reply(struct client *client,
                         struct faultline_worker_registry *registry, int64_t now)
{
    ssize_t count = send(client->fd, client->output + client->sent,
                         client->output_size - client->sent, 0);

    if (count < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            perror("coordinator: send");
            close_client(client, registry, "send_error");
        }
        return;
    }
    if (count == 0) {
        close_client(client, registry, "send_closed");
        return;
    }
    client->sent += (size_t)count;
    client->last_progress_ms = now;
    if (client->sent == client->output_size) {
        if (client->reply_type == FAULTLINE_MSG_PONG) {
            printf("[INFO] coordinator pong_sent fd=%d\n", client->fd);
        } else {
            printf("[INFO] coordinator worker_register_ack_sent worker_id=%" PRIu32
                   " fd=%d\n", client->worker_id, client->fd);
        }
        reset_input(client);
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
                .fd = fd, .phase = READING_MESSAGE, .expected = FAULTLINE_HEADER_SIZE,
                .last_progress_ms = now
            };
            printf("[INFO] coordinator client_connected fd=%d\n", fd);
        }
    }
}

static int heartbeat_poll_timeout(const struct faultline_worker_registry *registry,
                                   int64_t now, int timeout_ms)
{
    int wait_ms = 1000;

    for (size_t i = 0; i < FAULTLINE_MAX_WORKERS; ++i) {
        const struct faultline_worker *worker = &registry->workers[i];

        if (worker->state == FAULTLINE_WORKER_ALIVE) {
            int64_t remaining = timeout_ms - (now - worker->last_heartbeat_ms);

            if (remaining <= 0) {
                return 0;
            }
            if (remaining < wait_ms) {
                wait_ms = (int)remaining;
            }
        }
    }
    return wait_ms;
}

static int run_coordinator(int listener, int heartbeat_timeout_ms)
{
    struct faultline_worker_registry registry;
    struct client clients[MAX_CLIENTS];
    struct pollfd descriptors[MAX_CLIENTS + 1];
    int status = EXIT_SUCCESS;

    faultline_worker_registry_init(&registry);
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        clients[i] = (struct client){.fd = -1};
    }
    while (!stopping) {
        int ready;
        int64_t now = faultline_monotonic_ms();

        if (now < 0) {
            perror("coordinator: clock");
            status = EXIT_FAILURE;
            break;
        }

        descriptors[0] = (struct pollfd){.fd = listener, .events = POLLIN};
        for (size_t i = 0; i < MAX_CLIENTS; ++i) {
            descriptors[i + 1] = (struct pollfd){
                .fd = clients[i].fd,
                .events = clients[i].phase == READING_MESSAGE ? POLLIN : POLLOUT
            };
        }
        ready = poll(descriptors, MAX_CLIENTS + 1,
                     heartbeat_poll_timeout(&registry, now, heartbeat_timeout_ms));
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
            const struct faultline_worker *worker;

            if (clients[i].fd < 0) {
                continue;
            }
            worker = faultline_worker_find(&registry, clients[i].worker_id);
            /* Expire before reading: late bytes cannot revive an expired identity. */
            if (faultline_worker_timed_out(worker, now, heartbeat_timeout_ms)) {
                printf("[INFO] coordinator heartbeat_timeout worker_id=%" PRIu32
                       " fd=%d timeout_ms=%d detected_at_ms=%" PRId64
                       " silence_ms=%" PRId64 "\n", clients[i].worker_id,
                       clients[i].fd, heartbeat_timeout_ms, now,
                       now - worker->last_heartbeat_ms);
                close_client(&clients[i], &registry, "heartbeat_timeout");
                continue;
            }
            if ((events & POLLNVAL) != 0) {
                close_client(&clients[i], &registry, "invalid_descriptor");
                continue;
            }
            if (clients[i].phase == READING_MESSAGE &&
                (events & (POLLIN | POLLHUP | POLLERR)) != 0) {
                read_message(&clients[i], &registry, now);
            } else if (clients[i].phase == WRITING_REPLY &&
                       (events & (POLLOUT | POLLHUP | POLLERR)) != 0) {
                write_reply(&clients[i], &registry, now);
            }
            if (clients[i].fd >= 0 &&
                (clients[i].worker_id == FAULTLINE_WORKER_ID_UNASSIGNED ||
                 clients[i].phase == WRITING_REPLY || clients[i].received != 0) &&
                now - clients[i].last_progress_ms >= FAULTLINE_IO_TIMEOUT_MS) {
                fprintf(stderr, "[WARN] coordinator client_timeout fd=%d\n",
                        clients[i].fd);
                close_client(&clients[i], &registry, "io_timeout");
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
            close_client(&clients[i], &registry, "shutdown");
        }
    }
    return status;
}

static void usage(FILE *stream)
{
    fprintf(stream, "Usage: faultline-coordinator [--port PORT] "
            "[--heartbeat-timeout-ms MS]\n"
            "Default: 127.0.0.1:9000; heartbeat timeout: %d ms\n"
            "PORT must be in 1..65535; MS in 1..INT_MAX (decimal integers).\n",
            FAULTLINE_DEFAULT_HEARTBEAT_TIMEOUT_MS);
}

int main(int argc, char **argv)
{
    uint16_t port = FAULTLINE_DEFAULT_PORT;
    int heartbeat_timeout_ms = FAULTLINE_DEFAULT_HEARTBEAT_TIMEOUT_MS;
    int port_seen = 0;
    int timeout_seen = 0;
    struct sigaction action = {0};
    int listener;
    int status;

    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(stdout);
        return EXIT_SUCCESS;
    }
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 < argc && strcmp(argv[i], "--port") == 0 && !port_seen &&
            faultline_parse_port(argv[i + 1], &port) == 0) {
            port_seen = 1;
        } else if (i + 1 < argc && strcmp(argv[i], "--heartbeat-timeout-ms") == 0 &&
                   !timeout_seen &&
                   faultline_parse_duration_ms(argv[i + 1], &heartbeat_timeout_ms) == 0) {
            timeout_seen = 1;
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
        perror("coordinator: configure signals");
        return EXIT_FAILURE;
    }
    listener = faultline_listen(FAULTLINE_DEFAULT_HOST, port, MAX_CLIENTS);
    if (listener < 0) {
        perror("coordinator: listen");
        return EXIT_FAILURE;
    }
    printf("[INFO] coordinator listening address=%s port=%u heartbeat_timeout_ms=%d\n",
           FAULTLINE_DEFAULT_HOST, (unsigned int)port, heartbeat_timeout_ms);
    status = run_coordinator(listener, heartbeat_timeout_ms);
    (void)close(listener);
    puts("[INFO] coordinator stopped");
    return status;
}
