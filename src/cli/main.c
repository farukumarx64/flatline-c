#include "net.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(FILE *stream)
{
    fputs("Usage: faultline ping [--coordinator IPv4:PORT]\n"
          "       faultline submit TASK [--args TEXT | --args-hex HEX]\n"
          "                        [--max-retries N] [--coordinator IPv4:PORT]\n"
          "Tasks: sleep, prime_count, fibonacci, hash. Arguments: at most 1024 bytes.\n"
          "sleep: milliseconds 0..86400000; prime_count: inclusive bound 0..100000000.\n"
          "fibonacci: index 0..93; hash: raw bytes, FNV-1a 64-bit checksum.\n"
          "Numeric arguments must be decimal digits. Workers validate task inputs.\n"
          "Submission prints a job ID; completed results appear in the coordinator log.\n"
          "Default coordinator: 127.0.0.1:9000; max retries: 0.\n", stream);
}

static int run_ping(int argc, char **argv)
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

static int hex_digit(char byte)
{
    if (byte >= '0' && byte <= '9') { return byte - '0'; }
    if (byte >= 'a' && byte <= 'f') { return byte - 'a' + 10; }
    if (byte >= 'A' && byte <= 'F') { return byte - 'A' + 10; }
    return -1;
}

static int parse_retries(const char *text, uint32_t *retries)
{
    uint32_t value = 0;
    if (*text == '\0') { return -1; }
    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') { return -1; }
        uint32_t digit = (uint32_t)(*cursor - '0');
        if (value > (UINT32_MAX - digit) / UINT32_C(10)) { return -1; }
        value = value * UINT32_C(10) + digit;
    }
    *retries = value;
    return 0;
}

static int receive_submit_ack(int fd, uint64_t *job_id)
{
    uint8_t wire[FAULTLINE_HEADER_SIZE + FAULTLINE_JOB_SUBMIT_ACK_PAYLOAD_SIZE];
    struct faultline_header header;
    struct faultline_message message;
    size_t consumed;
    int64_t start = faultline_monotonic_ms();
    if (start < 0) { return -1; }
    if (faultline_recv_exact(fd, wire, FAULTLINE_HEADER_SIZE, FAULTLINE_IO_TIMEOUT_MS) !=
        FAULTLINE_RECEIVE_OK) {
        fputs("faultline: failed to receive submission ACK header\n", stderr);
        return -1;
    }
    if (faultline_header_decode(wire, FAULTLINE_HEADER_SIZE, &header) != FAULTLINE_PROTOCOL_OK ||
        header.message_type != FAULTLINE_MSG_JOB_SUBMIT_ACK ||
        header.payload_length != FAULTLINE_JOB_SUBMIT_ACK_PAYLOAD_SIZE) {
        fputs("faultline: expected submission ACK with an 8-byte job ID\n", stderr);
        return -1;
    }
    int64_t now = faultline_monotonic_ms();
    if (now < 0 || now - start >= FAULTLINE_IO_TIMEOUT_MS ||
        faultline_recv_exact(fd, wire + FAULTLINE_HEADER_SIZE, FAULTLINE_JOB_SUBMIT_ACK_PAYLOAD_SIZE,
                             FAULTLINE_IO_TIMEOUT_MS - (int)(now - start)) != FAULTLINE_RECEIVE_OK) {
        fputs("faultline: failed to receive submission ACK payload\n", stderr);
        return -1;
    }
    if (faultline_message_decode(wire, sizeof(wire), &message, &consumed) != FAULTLINE_PROTOCOL_OK) {
        fputs("faultline: invalid submission ACK payload\n", stderr);
        return -1;
    }
    *job_id = message.payload.job_submit_ack;
    return 0;
}

static int run_submit(int argc, char **argv)
{
    char host[INET_ADDRSTRLEN] = FAULTLINE_DEFAULT_HOST;
    uint16_t port = FAULTLINE_DEFAULT_PORT;
    struct faultline_message request = {.message_type = FAULTLINE_MSG_JOB_SUBMIT};
    struct faultline_job_submit_payload *submit = &request.payload.job_submit;
    const char *tasks[] = {"sleep", "prime_count", "fibonacci", "hash"};
    int arguments_seen = 0, retries_seen = 0, endpoint_seen = 0;
    if (argc < 3) { usage(stderr); return EXIT_FAILURE; }
    for (size_t i = 0; i < sizeof(tasks) / sizeof(tasks[0]); ++i) {
        if (strcmp(argv[2], tasks[i]) == 0) { submit->task_type = (uint16_t)(i + 1); }
    }
    if (submit->task_type == 0) { usage(stderr); return EXIT_FAILURE; }
    for (int i = 3; i < argc; i += 2) {
        if (i + 1 >= argc) { usage(stderr); return EXIT_FAILURE; }
        if (strcmp(argv[i], "--coordinator") == 0 && !endpoint_seen &&
            faultline_parse_endpoint(argv[i + 1], host, sizeof(host), &port) == 0) {
            endpoint_seen = 1;
        } else if (strcmp(argv[i], "--max-retries") == 0 && !retries_seen &&
                   parse_retries(argv[i + 1], &submit->max_retries) == 0) {
            retries_seen = 1;
        } else if (strcmp(argv[i], "--args") == 0 && !arguments_seen &&
                   strlen(argv[i + 1]) <= sizeof(submit->arguments)) {
            arguments_seen = 1;
            submit->argument_size = strlen(argv[i + 1]);
            memcpy(submit->arguments, argv[i + 1], submit->argument_size);
        } else if (strcmp(argv[i], "--args-hex") == 0 && !arguments_seen &&
                   strlen(argv[i + 1]) <= 2 * sizeof(submit->arguments) &&
                   strlen(argv[i + 1]) % 2 == 0) {
            arguments_seen = 1;
            submit->argument_size = strlen(argv[i + 1]) / 2;
            for (size_t j = 0; j < submit->argument_size; ++j) {
                int high = hex_digit(argv[i + 1][2 * j]);
                int low = hex_digit(argv[i + 1][2 * j + 1]);
                if (high < 0 || low < 0) { usage(stderr); return EXIT_FAILURE; }
                submit->arguments[j] = (uint8_t)(high * 16 + low);
            }
        } else { usage(stderr); return EXIT_FAILURE; }
    }
    uint8_t wire[FAULTLINE_MESSAGE_MAX_FRAME_SIZE];
    size_t written;
    if (faultline_message_encode(wire, sizeof(wire), &request, &written) != FAULTLINE_PROTOCOL_OK) {
        fputs("faultline: could not encode submission\n", stderr);
        return EXIT_FAILURE;
    }
    if (faultline_ignore_sigpipe() < 0) { perror("faultline: configure SIGPIPE"); return EXIT_FAILURE; }
    int fd = faultline_connect(host, port, FAULTLINE_IO_TIMEOUT_MS);
    if (fd < 0) { perror("faultline: connect"); return EXIT_FAILURE; }
    uint64_t job_id = 0;
    int status = EXIT_FAILURE;
    if (faultline_send_all(fd, wire, written, FAULTLINE_IO_TIMEOUT_MS) < 0) {
        perror("faultline: send submission");
    } else if (receive_submit_ack(fd, &job_id) == 0) {
        printf("job_id=%" PRIu64 "\n", job_id);
        status = EXIT_SUCCESS;
    }
    if (status != EXIT_SUCCESS) {
        fputs("faultline: submission unconfirmed; the coordinator may already have accepted it\n", stderr);
    }
    (void)close(fd);
    return status;
}

int main(int argc, char **argv)
{
    return argc >= 2 && strcmp(argv[1], "submit") == 0 ? run_submit(argc, argv) : run_ping(argc, argv);
}
