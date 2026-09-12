#include "protocol.h"

static void write_u16_be(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
}

static void write_u32_be(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static uint16_t read_u16_be(const uint8_t *in)
{
    return (uint16_t)(((uint16_t)in[0] << 8) | (uint16_t)in[1]);
}

static uint32_t read_u32_be(const uint8_t *in)
{
    return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
           ((uint32_t)in[2] << 8) | (uint32_t)in[3];
}

static enum faultline_protocol_result message_payload_size(
    uint16_t message_type, uint32_t *payload_size)
{
    switch (message_type) {
    case FAULTLINE_MSG_PING:
    case FAULTLINE_MSG_PONG:
        *payload_size = 0;
        break;
    case FAULTLINE_MSG_WORKER_REGISTER:
        *payload_size = FAULTLINE_WORKER_REGISTER_PAYLOAD_SIZE;
        break;
    case FAULTLINE_MSG_WORKER_REGISTER_ACK:
        *payload_size = FAULTLINE_WORKER_REGISTER_ACK_PAYLOAD_SIZE;
        break;
    case FAULTLINE_MSG_HEARTBEAT:
        *payload_size = FAULTLINE_HEARTBEAT_PAYLOAD_SIZE;
        break;
    default:
        return FAULTLINE_PROTOCOL_UNKNOWN_MESSAGE_TYPE;
    }
    return FAULTLINE_PROTOCOL_OK;
}

static enum faultline_protocol_result validate_header(
    const struct faultline_header *header)
{
    uint32_t payload_size;

    if (header->magic != FAULTLINE_PROTOCOL_MAGIC) {
        return FAULTLINE_PROTOCOL_BAD_MAGIC;
    }
    if (header->version != FAULTLINE_PROTOCOL_VERSION) {
        return FAULTLINE_PROTOCOL_UNSUPPORTED_VERSION;
    }
    if (message_payload_size(header->message_type, &payload_size) !=
        FAULTLINE_PROTOCOL_OK) {
        return FAULTLINE_PROTOCOL_UNKNOWN_MESSAGE_TYPE;
    }
    if (header->payload_length > FAULTLINE_MAX_PAYLOAD_SIZE) {
        return FAULTLINE_PROTOCOL_PAYLOAD_TOO_LARGE;
    }

    return FAULTLINE_PROTOCOL_OK;
}

enum faultline_protocol_result faultline_header_encode(
    uint8_t *wire, size_t wire_size, const struct faultline_header *header)
{
    enum faultline_protocol_result result;

    if (wire == NULL || header == NULL) {
        return FAULTLINE_PROTOCOL_INVALID_ARGUMENT;
    }
    if (wire_size < FAULTLINE_HEADER_SIZE) {
        return FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL;
    }

    result = validate_header(header);
    if (result != FAULTLINE_PROTOCOL_OK) {
        return result;
    }

    write_u32_be(wire, header->magic);
    write_u16_be(wire + 4, header->version);
    write_u16_be(wire + 6, header->message_type);
    write_u32_be(wire + 8, header->payload_length);
    return FAULTLINE_PROTOCOL_OK;
}

enum faultline_protocol_result faultline_header_decode(
    const uint8_t *wire, size_t wire_size, struct faultline_header *header)
{
    struct faultline_header decoded;
    enum faultline_protocol_result result;

    if (wire == NULL || header == NULL) {
        return FAULTLINE_PROTOCOL_INVALID_ARGUMENT;
    }
    if (wire_size < FAULTLINE_HEADER_SIZE) {
        return FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL;
    }

    decoded.magic = read_u32_be(wire);
    decoded.version = read_u16_be(wire + 4);
    decoded.message_type = read_u16_be(wire + 6);
    decoded.payload_length = read_u32_be(wire + 8);

    result = validate_header(&decoded);
    if (result != FAULTLINE_PROTOCOL_OK) {
        return result;
    }

    *header = decoded;
    return FAULTLINE_PROTOCOL_OK;
}

static enum faultline_protocol_result validate_worker_id(
    uint32_t payload_size, uint32_t worker_id)
{
    /* Empty messages have no ID; both current nonempty payloads require one. */
    if ((payload_size == 0 && worker_id != FAULTLINE_WORKER_ID_UNASSIGNED) ||
        (payload_size != 0 && worker_id == FAULTLINE_WORKER_ID_UNASSIGNED)) {
        return FAULTLINE_PROTOCOL_INVALID_WORKER_ID;
    }
    return FAULTLINE_PROTOCOL_OK;
}

enum faultline_protocol_result faultline_message_encode(
    uint8_t *wire, size_t wire_size, const struct faultline_message *message,
    size_t *written)
{
    struct faultline_header header = {
        .magic = FAULTLINE_PROTOCOL_MAGIC,
        .version = FAULTLINE_PROTOCOL_VERSION
    };
    enum faultline_protocol_result result;
    size_t frame_size;

    if (wire == NULL || message == NULL || written == NULL) {
        return FAULTLINE_PROTOCOL_INVALID_ARGUMENT;
    }
    header.message_type = message->message_type;
    result = message_payload_size(message->message_type, &header.payload_length);
    if (result != FAULTLINE_PROTOCOL_OK) {
        return result;
    }
    result = validate_worker_id(header.payload_length, message->worker_id);
    if (result != FAULTLINE_PROTOCOL_OK) {
        return result;
    }
    frame_size = FAULTLINE_HEADER_SIZE + (size_t)header.payload_length;
    if (wire_size < frame_size) {
        return FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL;
    }

    /* Validate everything before writing either the header or the payload. */
    result = faultline_header_encode(wire, wire_size, &header);
    if (result != FAULTLINE_PROTOCOL_OK) {
        return result;
    }
    if (header.payload_length != 0) {
        write_u32_be(wire + FAULTLINE_HEADER_SIZE, message->worker_id);
    }
    *written = frame_size;
    return FAULTLINE_PROTOCOL_OK;
}

enum faultline_protocol_result faultline_message_decode(
    const uint8_t *wire, size_t wire_size, struct faultline_message *message,
    size_t *consumed)
{
    struct faultline_header header;
    struct faultline_message decoded = {0};
    enum faultline_protocol_result result;
    uint32_t payload_size;
    size_t frame_size;

    if (wire == NULL || message == NULL || consumed == NULL) {
        return FAULTLINE_PROTOCOL_INVALID_ARGUMENT;
    }
    result = faultline_header_decode(wire, wire_size, &header);
    if (result != FAULTLINE_PROTOCOL_OK) {
        return result;
    }
    result = message_payload_size(header.message_type, &payload_size);
    if (result != FAULTLINE_PROTOCOL_OK) {
        return result;
    }
    if (header.payload_length != payload_size) {
        return FAULTLINE_PROTOCOL_INVALID_PAYLOAD_LENGTH;
    }
    frame_size = FAULTLINE_HEADER_SIZE + (size_t)payload_size;
    if (wire_size < frame_size) {
        return FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL;
    }
    decoded.message_type = header.message_type;
    if (payload_size != 0) {
        decoded.worker_id = read_u32_be(wire + FAULTLINE_HEADER_SIZE);
    }
    result = validate_worker_id(payload_size, decoded.worker_id);
    if (result != FAULTLINE_PROTOCOL_OK) {
        return result;
    }

    *message = decoded;
    *consumed = frame_size;
    return FAULTLINE_PROTOCOL_OK;
}
