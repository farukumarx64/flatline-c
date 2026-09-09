# Faultline wire protocol

The protocol header defines how a receiver identifies a Faultline message and
determines the number of payload bytes that follow it. Header encoding and
validation are implemented. TCP transport, stream buffering, and payload
handlers will be added in the next milestones.

## Header layout

Every frame starts with exactly 12 bytes. Offsets are zero-based. All integer
fields use unsigned, big-endian encoding, also called network byte order.

| Offset | Size | Field | Accepted value |
| --- | --- | --- | --- |
| 0 | 4 bytes | Magic | `0x464c494e`, the ASCII bytes `FLIN` |
| 4 | 2 bytes | Version | `1` |
| 6 | 2 bytes | Message type | `1` for `PING`, `2` for `PONG` |
| 8 | 4 bytes | Payload length | `0` through `1,048,576` bytes inclusive |

```text
byte offset   0            4      6      8           12
              +------------+------+------+------------+-----------------+
              |   magic    | ver  | type | length     | payload ...     |
              +------------+------+------+------------+-----------------+
field size         4          2      2        4          length bytes
```

The payload length excludes the 12-byte header. A zero-length payload is valid
at the header layer; its whole frame is 12 bytes. The maximum accepted frame is
1,048,588 bytes: a 1 MiB payload plus the header. The 1 MiB limit is an initial
project policy that bounds individual messages even though the length field can
represent much larger values. It does not bound total buffered data across
connections; that is a later transport concern.

The magic identifies the expected protocol; it is not a checksum or
authentication mechanism. The version selects the interpretation of the
protocol and is independent of the application's eventual `v0.1.0` release tag.
An unsupported version is rejected; version negotiation is not implemented.

Only `PING` and `PONG` have assigned message IDs at this stage. Zero and other
IDs are rejected. Worker registration, heartbeat, job, and query messages will
receive explicit IDs when their payload formats are designed. Existing IDs must
not be renumbered. Enum storage layout is never used as the wire representation.

The header layer checks that a type is recognized and a length is bounded. It
does not define or validate that type's payload contents. A nonzero length in
the tests exercises the length field; it does not establish PING/PONG handler
behavior. Payload rules belong to the upcoming message handlers.

## Byte order and examples

Big-endian encoding puts the most significant byte first. The numeric value
`0x00010203` is sent as `00 01 02 03`, regardless of the sender's CPU byte order.
The four bytes are binary values, not the text characters `"00010203"`.

A version 1 PING header with an empty payload is:

```text
46 4c 49 4e | 00 01 | 00 01 | 00 00 00 00
   FLIN    |   v1  |  PING |    0 bytes
```

A header naming PONG with a declared payload length of 66,051 (`0x00010203`) is:

```text
46 4c 49 4e | 00 01 | 00 02 | 00 01 02 03
   FLIN    |   v1  |  PONG |  66,051 bytes
```

Only headers are shown. A complete frame in the second example would need the
declared payload bytes after the header.

## C API

`include/protocol.h` declares constants, message IDs, error results, the
`struct faultline_header` host representation, and two functions:

```c
enum faultline_protocol_result faultline_header_encode(
    uint8_t *wire, size_t wire_size, const struct faultline_header *header);

enum faultline_protocol_result faultline_header_decode(
    const uint8_t *wire, size_t wire_size, struct faultline_header *header);
```

For example, prepare a PING header for future transmission:

```c
struct faultline_header header = {
    .magic = FAULTLINE_PROTOCOL_MAGIC,
    .version = FAULTLINE_PROTOCOL_VERSION,
    .message_type = FAULTLINE_MSG_PING,
    .payload_length = 0
};
uint8_t wire[FAULTLINE_HEADER_SIZE];
enum faultline_protocol_result result =
    faultline_header_encode(wire, sizeof(wire), &header);
/* Only use wire for transmission if result == FAULTLINE_PROTOCOL_OK. */
```

Struct fields are ordinary host-order numbers. Callers must not apply `htonl()`
or `htons()` themselves before encoding: the encoder already writes network
order. Likewise, decoded fields need no further byte-order conversion.

Never transmit `sizeof(struct faultline_header)` bytes from the struct. Its
padding, alignment, and memory byte order are separate from the protocol.
`src/common/protocol.c` explicitly reads and writes individual bytes at the
specified offsets using unsigned shifts. It makes no typed pointer casts into
the wire buffer and does not require that buffer to be aligned for integers.

## Validation and buffer ownership

Both functions require non-null pointers to valid, non-overlapping storage.
The caller owns all buffers; neither function allocates memory or performs I/O.
The encoder's size is writable capacity; the decoder's size is the number of
input bytes actually available.

Validation proceeds in the following order and returns the first error:

| Result | Meaning |
| --- | --- |
| `FAULTLINE_PROTOCOL_INVALID_ARGUMENT` | A required pointer is null. |
| `FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL` | Fewer than 12 input bytes or output bytes of capacity. |
| `FAULTLINE_PROTOCOL_BAD_MAGIC` | Magic does not match `FLIN`. |
| `FAULTLINE_PROTOCOL_UNSUPPORTED_VERSION` | Version is not 1. |
| `FAULTLINE_PROTOCOL_UNKNOWN_MESSAGE_TYPE` | Type is not PING or PONG. |
| `FAULTLINE_PROTOCOL_PAYLOAD_TOO_LARGE` | Declared payload exceeds 1 MiB. |
| `FAULTLINE_PROTOCOL_OK` | Header was successfully encoded or decoded. |

On any error the output stays unchanged. Encoding validates before writing any
bytes. Decoding uses a temporary header and copies it to the caller only after
validation succeeds.

On success, encoding writes exactly 12 bytes and leaves any extra capacity
untouched. Decoding reads only the first 12 bytes and ignores trailing input.
It does not consume a stream buffer or report payload completion.

## Relationship to TCP framing

TCP provides a stream of bytes, not one complete message per `recv()` call.
The future receiver must:

1. Accumulate at least 12 bytes without discarding earlier partial input.
2. Decode the header and reject invalid field values.
3. Accumulate exactly the declared payload length before dispatching the message.
4. Retain any bytes belonging to following frames for subsequent parsing.

A short header is incomplete input, not necessarily a malformed message. If a
connection closes before the header or payload finishes arriving, the transport
must handle the truncated frame. These functions do not keep partial-read state,
retry sends, or close connections.

## Verification

Run `make test` for the normal build and `make test-sanitize` for AddressSanitizer
and UndefinedBehaviorSanitizer. The seven test groups cover literal wire vectors,
payload boundaries, all header truncation lengths, invalid fields, invalid
encoder inputs, null arguments, and unaligned buffers with trailing bytes.

Literal expected byte sequences verify conformance independently of round-trip
tests. Failure cases also check that outputs remain unchanged.
