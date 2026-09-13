#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int faultline_parse_port(const char *text, uint16_t *port)
{
    uint32_t value = 0;

    if (text == NULL || port == NULL || *text == '\0') {
        errno = EINVAL;
        return -1;
    }
    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') {
            errno = EINVAL;
            return -1;
        }
        value = value * UINT32_C(10) + (uint32_t)(*cursor - '0');
        if (value > UINT16_MAX) {
            errno = EINVAL;
            return -1;
        }
    }
    if (value == 0) {
        errno = EINVAL;
        return -1;
    }
    *port = (uint16_t)value;
    return 0;
}

int faultline_parse_endpoint(const char *text, char *host, size_t host_size,
                             uint16_t *port)
{
    const char *separator;
    char parsed_host[INET_ADDRSTRLEN];
    struct in_addr address;
    uint16_t parsed_port;
    size_t length;

    if (text == NULL || host == NULL || port == NULL) {
        errno = EINVAL;
        return -1;
    }
    separator = strchr(text, ':');
    if (separator == NULL) {
        errno = EINVAL;
        return -1;
    }
    length = (size_t)(separator - text);
    if (length == 0 || length >= sizeof(parsed_host) || length >= host_size ||
        faultline_parse_port(separator + 1, &parsed_port) < 0) {
        errno = EINVAL;
        return -1;
    }
    memcpy(parsed_host, text, length);
    parsed_host[length] = '\0';
    if (inet_pton(AF_INET, parsed_host, &address) != 1) {
        errno = EINVAL;
        return -1;
    }
    memcpy(host, parsed_host, length + 1);
    *port = parsed_port;
    return 0;
}

int faultline_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int faultline_ignore_sigpipe(void)
{
    struct sigaction action = {0};

    action.sa_handler = SIG_IGN;
    if (sigemptyset(&action.sa_mask) < 0) {
        return -1;
    }
    return sigaction(SIGPIPE, &action, NULL);
}

int64_t faultline_monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return -1;
    }
    return (int64_t)now.tv_sec * INT64_C(1000) +
           (int64_t)now.tv_nsec / INT64_C(1000000);
}

static int wait_ready(int fd, short events, int64_t deadline)
{
    struct pollfd descriptor = {.fd = fd, .events = events};

    for (;;) {
        int64_t now = faultline_monotonic_ms();
        int result;

        if (now < 0) {
            return -1;
        }
        if (now >= deadline) {
            errno = ETIMEDOUT;
            return -1;
        }
        /* The remaining interval cannot exceed the original positive int. */
        result = poll(&descriptor, 1, (int)(deadline - now));
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (result == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        if ((descriptor.revents & POLLNVAL) != 0) {
            errno = EBADF;
            return -1;
        }
        /* Let send/recv/SO_ERROR resolve HUP and ERR, including buffered data. */
        if ((descriptor.revents & (events | POLLHUP | POLLERR)) != 0) {
            return 0;
        }
    }
}

static int close_failed_socket(int fd)
{
    int saved_errno = errno;

    (void)close(fd);
    errno = saved_errno;
    return -1;
}

static int make_address(const char *host, uint16_t port,
                        struct sockaddr_in *address)
{
    if (host == NULL || port == 0) {
        errno = EINVAL;
        return -1;
    }
    *address = (struct sockaddr_in){0};
    address->sin_family = AF_INET;
    address->sin_port = htons(port);
    if (inet_pton(AF_INET, host, &address->sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int faultline_listen(const char *host, uint16_t port, int backlog)
{
    struct sockaddr_in address;
    int reuse = 1;
    int fd;

    if (make_address(host, port, &address) < 0) {
        return -1;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0 ||
        faultline_set_nonblocking(fd) < 0 ||
        bind(fd, (const struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(fd, backlog) < 0) {
        return close_failed_socket(fd);
    }
    return fd;
}

int faultline_connect(const char *host, uint16_t port, int timeout_ms)
{
    struct sockaddr_in address;
    int64_t now;
    int fd;
    int socket_error = 0;
    socklen_t error_size = sizeof(socket_error);

    if (timeout_ms <= 0) {
        errno = EINVAL;
        return -1;
    }
    if (make_address(host, port, &address) < 0) {
        return -1;
    }
    now = faultline_monotonic_ms();
    if (now < 0) {
        return -1;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    if (faultline_set_nonblocking(fd) < 0) {
        return close_failed_socket(fd);
    }
    if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) == 0) {
        return fd;
    }
    if (errno != EINPROGRESS && errno != EINTR) {
        return close_failed_socket(fd);
    }
    if (wait_ready(fd, POLLOUT, now + timeout_ms) < 0 ||
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size) < 0) {
        return close_failed_socket(fd);
    }
    if (socket_error != 0) {
        errno = socket_error;
        return close_failed_socket(fd);
    }
    return fd;
}

int faultline_send_all(int fd, const uint8_t *data, size_t size, int timeout_ms)
{
    size_t sent = 0;
    int64_t start;

    if (fd < 0 || (data == NULL && size != 0) || timeout_ms <= 0) {
        errno = EINVAL;
        return -1;
    }
    start = faultline_monotonic_ms();
    if (start < 0) {
        return -1;
    }
    while (sent < size) {
        ssize_t count;

        if (wait_ready(fd, POLLOUT, start + timeout_ms) < 0) {
            return -1;
        }
        count = send(fd, data + sent, size - sent, 0);
        if (count > 0) {
            sent += (size_t)count;
        } else if (count == 0) {
            errno = EPIPE;
            return -1;
        } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }
    }
    return 0;
}

enum faultline_receive_result faultline_recv_exact(
    int fd, uint8_t *data, size_t size, int timeout_ms)
{
    size_t received = 0;
    int64_t start;

    if (fd < 0 || (data == NULL && size != 0) || timeout_ms <= 0) {
        errno = EINVAL;
        return FAULTLINE_RECEIVE_ERROR;
    }
    start = faultline_monotonic_ms();
    if (start < 0) {
        return FAULTLINE_RECEIVE_ERROR;
    }
    while (received < size) {
        ssize_t count;

        if (wait_ready(fd, POLLIN, start + timeout_ms) < 0) {
            return FAULTLINE_RECEIVE_ERROR;
        }
        count = recv(fd, data + received, size - received, 0);
        if (count > 0) {
            received += (size_t)count;
        } else if (count == 0) {
            return received == 0 ? FAULTLINE_RECEIVE_EOF :
                                   FAULTLINE_RECEIVE_TRUNCATED;
        } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            return FAULTLINE_RECEIVE_ERROR;
        }
    }
    return FAULTLINE_RECEIVE_OK;
}
