#include "net.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(FILE *stream)
{
    fputs("Usage: faultline ping [--coordinator IPv4:PORT]\n"
          "Default coordinator: 127.0.0.1:9000\n", stream);
}

int main(int argc, char **argv)
{
    char host[INET_ADDRSTRLEN] = FAULTLINE_DEFAULT_HOST;
    uint16_t port = FAULTLINE_DEFAULT_PORT;
    struct faultline_header header = {
        .magic = FAULTLINE_PROTOCOL_MAGIC,
        .version = FAULTLINE_PROTOCOL_VERSION,
        .message_type = FAULTLINE_MSG_PING,
        .payload_length = 0
    };
    uint8_t wire[FAULTLINE_HEADER_SIZE];
    enum faultline_receive_result receive_result;
    enum faultline_protocol_result protocol_result;
    int fd;
    int status = EXIT_FAILURE;

    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(stdout);
        return EXIT_SUCCESS;
    }
    if ((argc != 2 && argc != 4) || strcmp(argv[1], "ping") != 0) {
        usage(stderr);
        return EXIT_FAILURE;
    }
    if (argc == 4 && (strcmp(argv[2], "--coordinator") != 0 ||
                     faultline_parse_endpoint(argv[3], host, sizeof(host), &port) < 0)) {
        usage(stderr);
        return EXIT_FAILURE;
    }
    if (faultline_ignore_sigpipe() < 0) {
        perror("faultline: configure SIGPIPE");
        return EXIT_FAILURE;
    }
    fd = faultline_connect(host, port, FAULTLINE_IO_TIMEOUT_MS);
    if (fd < 0) {
        perror("faultline: connect");
        return EXIT_FAILURE;
    }
    protocol_result = faultline_header_encode(wire, sizeof(wire), &header);
    if (protocol_result != FAULTLINE_PROTOCOL_OK) {
        fprintf(stderr, "faultline: encode PING failed (protocol error %d)\n",
                (int)protocol_result);
        goto done;
    }
    if (faultline_send_all(fd, wire, sizeof(wire), FAULTLINE_IO_TIMEOUT_MS) < 0) {
        perror("faultline: send PING");
        goto done;
    }
    receive_result = faultline_recv_exact(fd, wire, sizeof(wire),
                                          FAULTLINE_IO_TIMEOUT_MS);
    if (receive_result != FAULTLINE_RECEIVE_OK) {
        if (receive_result == FAULTLINE_RECEIVE_ERROR) {
            perror("faultline: receive PONG");
        } else {
            fprintf(stderr, "faultline: coordinator closed %s PONG\n",
                    receive_result == FAULTLINE_RECEIVE_EOF ? "before" : "during");
        }
        goto done;
    }
    protocol_result = faultline_header_decode(wire, sizeof(wire), &header);
    if (protocol_result != FAULTLINE_PROTOCOL_OK) {
        fprintf(stderr, "faultline: invalid PONG header (protocol error %d)\n",
                (int)protocol_result);
        goto done;
    }
    if (header.message_type != FAULTLINE_MSG_PONG || header.payload_length != 0) {
        fputs("faultline: expected PONG with an empty payload\n", stderr);
        goto done;
    }
    puts("PONG");
    status = EXIT_SUCCESS;

done:
    (void)close(fd);
    return status;
}
