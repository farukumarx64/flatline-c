#ifndef FAULTLINE_PROTOCOL_H
#define FAULTLINE_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

/* Wire bytes 46 4c 49 4e spell "FLIN" in ASCII. */
#define FAULTLINE_PROTOCOL_MAGIC UINT32_C(0x464c494e)
#define FAULTLINE_PROTOCOL_VERSION UINT16_C(1)
#define FAULTLINE_HEADER_SIZE 12u
#define FAULTLINE_MAX_PAYLOAD_SIZE UINT32_C(1048576)

enum faultline_message_type {
    FAULTLINE_MSG_PING = 1,
    FAULTLINE_MSG_PONG = 2
};

/* Host-order values only. Never send this struct directly over a socket. */
struct faultline_header {
    uint32_t magic;
    uint16_t version;
    uint16_t message_type;
    uint32_t payload_length;
};

enum faultline_protocol_result {
    FAULTLINE_PROTOCOL_OK = 0,
    FAULTLINE_PROTOCOL_INVALID_ARGUMENT,
    FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL,
    FAULTLINE_PROTOCOL_BAD_MAGIC,
    FAULTLINE_PROTOCOL_UNSUPPORTED_VERSION,
    FAULTLINE_PROTOCOL_UNKNOWN_MESSAGE_TYPE,
    FAULTLINE_PROTOCOL_PAYLOAD_TOO_LARGE
};

/*
 * Validate and encode one header as exactly 12 big-endian bytes.
 * wire_size is the available output capacity. Extra bytes are untouched.
 * On failure, no output is changed. Non-null arguments must refer to valid,
 * non-overlapping storage. No memory is allocated and no socket I/O is done.
 */
enum faultline_protocol_result faultline_header_encode(
    uint8_t *wire, size_t wire_size, const struct faultline_header *header);

/*
 * Decode and validate the first 12 bytes; any following bytes are ignored.
 * A short input returns BUFFER_TOO_SMALL: the caller must gather more bytes.
 * Success validates only the header, not payload availability or contents.
 * On failure, *header is unchanged. The same storage rules as encode apply.
 */
enum faultline_protocol_result faultline_header_decode(
    const uint8_t *wire, size_t wire_size, struct faultline_header *header);

#endif
