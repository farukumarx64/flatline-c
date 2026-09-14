#ifndef FAULTLINE_NET_H
#define FAULTLINE_NET_H

#include <stddef.h>
#include <stdint.h>

#define FAULTLINE_DEFAULT_HOST "127.0.0.1"
#define FAULTLINE_DEFAULT_PORT UINT16_C(9000)
#define FAULTLINE_IO_TIMEOUT_MS 5000
#define FAULTLINE_DEFAULT_HEARTBEAT_INTERVAL_MS 2000
#define FAULTLINE_DEFAULT_HEARTBEAT_TIMEOUT_MS 6000

enum faultline_receive_result {
    FAULTLINE_RECEIVE_OK = 0,
    FAULTLINE_RECEIVE_EOF,
    FAULTLINE_RECEIVE_TRUNCATED,
    FAULTLINE_RECEIVE_ERROR
};

/* Helpers return -1 on error and set errno. Ports must be decimal 1..65535. */
int faultline_parse_port(const char *text, uint16_t *port);
/* Decimal milliseconds in 1..INT_MAX; errors leave the output unchanged. */
int faultline_parse_duration_ms(const char *text, int *duration_ms);
/* Parse numeric IPv4:PORT into non-overlapping outputs; errors leave both unchanged. */
int faultline_parse_endpoint(const char *text, char *host, size_t host_size,
                             uint16_t *port);
int faultline_set_nonblocking(int fd);
int faultline_ignore_sigpipe(void);
int64_t faultline_monotonic_ms(void);

/* Numeric IPv4 only. Returned sockets are nonblocking and owned by the caller. */
int faultline_listen(const char *host, uint16_t port, int backlog);
int faultline_connect(const char *host, uint16_t port, int timeout_ms);

/*
 * Require a nonblocking socket and a positive timeout. Each call has one total
 * monotonic deadline, including all partial transfers and retries. send_all
 * returns 0 or -1; recv_exact distinguishes clean EOF from a truncated read.
 * On error, errno describes the failure (ETIMEDOUT for an expired deadline).
 * Partial progress may already have changed the stream or receive buffer.
 * Call ignore_sigpipe before sending. Neither function closes the socket.
 */
int faultline_send_all(int fd, const uint8_t *data, size_t size, int timeout_ms);
enum faultline_receive_result faultline_recv_exact(
    int fd, uint8_t *data, size_t size, int timeout_ms);

#endif
