#ifndef FAULTLINE_PROTOCOL_H
#define FAULTLINE_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

/* Wire bytes 46 4c 49 4e spell "FLIN" in ASCII. */
#define FAULTLINE_PROTOCOL_MAGIC UINT32_C(0x464c494e)
#define FAULTLINE_PROTOCOL_VERSION UINT16_C(1)
#define FAULTLINE_HEADER_SIZE 12u
#define FAULTLINE_MAX_PAYLOAD_SIZE UINT32_C(1048576)
#define FAULTLINE_WORKER_REGISTER_PAYLOAD_SIZE 0u
#define FAULTLINE_WORKER_REGISTER_ACK_PAYLOAD_SIZE 4u
#define FAULTLINE_HEARTBEAT_PAYLOAD_SIZE 4u
#define FAULTLINE_WORKER_ID_UNASSIGNED UINT32_C(0)

enum faultline_message_type {
    FAULTLINE_MSG_PING = 1,
    FAULTLINE_MSG_PONG = 2,
    FAULTLINE_MSG_WORKER_REGISTER = 3,
    FAULTLINE_MSG_WORKER_REGISTER_ACK = 4,
    FAULTLINE_MSG_HEARTBEAT = 5
};

/* Host-order values only. Never send this struct directly over a socket. */
struct faultline_header {
    uint32_t magic;
    uint16_t version;
    uint16_t message_type;
    uint32_t payload_length;
};

/*
 * Host-order representation of the currently supported complete messages.
 * worker_id is nonzero for WORKER_REGISTER_ACK and HEARTBEAT, and zero for
 * PING, PONG, and WORKER_REGISTER. Never send this struct directly.
 */
struct faultline_message {
    uint16_t message_type;
    uint32_t worker_id;
};

enum faultline_protocol_result {
    FAULTLINE_PROTOCOL_OK = 0,
    FAULTLINE_PROTOCOL_INVALID_ARGUMENT,
    FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL,
    FAULTLINE_PROTOCOL_BAD_MAGIC,
    FAULTLINE_PROTOCOL_UNSUPPORTED_VERSION,
    FAULTLINE_PROTOCOL_UNKNOWN_MESSAGE_TYPE,
    FAULTLINE_PROTOCOL_PAYLOAD_TOO_LARGE,
    FAULTLINE_PROTOCOL_INVALID_PAYLOAD_LENGTH,
    FAULTLINE_PROTOCOL_INVALID_WORKER_ID
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

/*
 * Encode a complete message, deriving its header and exact payload length.
 * wire_size is output capacity; *written receives the frame size on success.
 * All pointers are required and storage must be valid and non-overlapping.
 * No output (including *written) changes on failure. Extra capacity is untouched.
 * No allocation, socket I/O, ID assignment, or connection-state checks are done.
 */
enum faultline_protocol_result faultline_message_encode(
    uint8_t *wire, size_t wire_size, const struct faultline_message *message,
    size_t *written);

/*
 * Decode one complete frame from wire_size available bytes. A partial header
 * or payload returns BUFFER_TOO_SMALL; a wrong declared length returns
 * INVALID_PAYLOAD_LENGTH. *consumed reports this frame's size on success, so
 * the caller can retain following frames. All output stays unchanged on error.
 * The same pointer/storage rules as message_encode apply.
 */
enum faultline_protocol_result faultline_message_decode(
    const uint8_t *wire, size_t wire_size, struct faultline_message *message,
    size_t *consumed);

#endif
