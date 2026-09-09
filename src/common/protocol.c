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

static enum faultline_protocol_result validate_header(
    const struct faultline_header *header)
{
    if (header->magic != FAULTLINE_PROTOCOL_MAGIC) {
        return FAULTLINE_PROTOCOL_BAD_MAGIC;
    }
    if (header->version != FAULTLINE_PROTOCOL_VERSION) {
        return FAULTLINE_PROTOCOL_UNSUPPORTED_VERSION;
    }
    if (header->message_type != FAULTLINE_MSG_PING &&
        header->message_type != FAULTLINE_MSG_PONG) {
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
