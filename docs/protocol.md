# Faultline wire protocol

The protocol header defines how a receiver identifies a Faultline message and
determines the number of payload bytes that follow it. Header encoding,
validation, complete-message encoding/decoding, and a TCP exchange for
empty-payload PING/PONG are implemented. Worker registration, its acknowledgment,
and heartbeat formats are defined and tested, including their payload bytes.
The worker and coordinator do not exchange these new messages yet.

## Header layout

Every frame starts with exactly 12 bytes. Offsets are zero-based. All integer
fields use unsigned, big-endian encoding, also called network byte order.

| Offset | Size | Field | Accepted value |
| --- | --- | --- | --- |
| 0 | 4 bytes | Magic | `0x464c494e`, the ASCII bytes `FLIN` |
| 4 | 2 bytes | Version | `1` |
| 6 | 2 bytes | Message type | IDs `1` through `5`, listed below |
| 8 | 4 bytes | Payload length | `0` through `1,048,576` bytes inclusive |

```text
byte offset   0            4      6      8           12
              +------------+------+------+------------+-----------------+
              |   magic    | ver  | type | length     | payload ...     |
              +------------+------+------+------------+-----------------+
field size         4          2      2        4          length bytes
```

The payload length excludes the 12-byte header. A zero-length payload is valid
at the header layer; its whole frame is 12 bytes. The maximum frame allowed by
the header's length limit is 1,048,588 bytes: a 1 MiB payload plus the header.
Current message formats impose much smaller exact lengths. The 1 MiB limit is an initial
project policy that bounds individual messages even though the length field can
represent much larger values. It does not bound total buffered data across
connections; that is a later transport concern.

The magic identifies the expected protocol; it is not a checksum or
authentication mechanism. The version selects the interpretation of the
protocol and is independent of the application's eventual `v0.1.0` release tag.
An unsupported version is rejected; version negotiation is not implemented.

Zero and IDs outside the table below are rejected. Job and query messages will
receive explicit IDs when their payload formats are designed. Existing IDs must
not be renumbered. Enum storage layout is never used as the wire representation.

## Messages and payloads

| ID | Message | Direction | Payload | Whole frame |
| --- | --- | --- | --- | --- |
| 1 | `PING` | CLI to coordinator | Empty: 0 bytes | 12 bytes |
| 2 | `PONG` | Coordinator to CLI | Empty: 0 bytes | 12 bytes |
| 3 | `WORKER_REGISTER` | Worker to coordinator | Empty: 0 bytes | 12 bytes |
| 4 | `WORKER_REGISTER_ACK` | Coordinator to worker | `worker_id`: 4-byte unsigned big-endian integer | 16 bytes |
| 5 | `HEARTBEAT` | Worker to coordinator | `worker_id`: 4-byte unsigned big-endian integer | 16 bytes |

`WORKER_REGISTER` requests an identity. It does not propose an ID or carry
metadata: MVP workers all execute one job at a time, so capacity negotiation is
not needed for this first format. `WORKER_REGISTER_ACK` represents successful
registration and carries the coordinator-issued ID. There is no rejection
payload or separate registration-error message in this step.

Both nonempty payloads have one field at payload offset 0 (frame offset 12):
`worker_id`, exactly 4 bytes, with accepted values `1` through `4,294,967,295`.
Zero is reserved for an unregistered worker and is invalid in an ACK or
heartbeat. Worker IDs identify registrations; they are not socket descriptors,
process IDs, or permanent machine identities.

The intended registration sequence is:

```text
Worker                             Coordinator
  | ------ WORKER_REGISTER ----------> |
  | <----- WORKER_REGISTER_ACK(12) ---- |
  | ------ HEARTBEAT(12) -------------> |
  | ------ HEARTBEAT(12) -------------> |
```

The worker must wait for the ACK before sending heartbeats, then use the
assigned ID on that connection. The coordinator will need to assign distinct,
nonzero IDs, associate each with its registered connection, and check heartbeat
IDs against that association. Reconnection requires registration again; this
format makes no promise of preserving IDs across connections or coordinator
restarts. `HEARTBEAT` has no reply and carries no sender timestamp. Liveness will
use the coordinator's own monotonic receive time, avoiding cross-machine clock
comparisons. Scheduling, heartbeat intervals, and timeout detection come later.

These connection-state rules are a contract for the upcoming worker lifecycle
implementation. The codec validates byte structure and ID range; it cannot
prove that an ID was issued, check connection ownership, or measure liveness.

The header functions only check that a type is recognized and a length is
bounded. The complete-message functions additionally enforce the exact lengths
in the table. For example, a header declaring a 3-byte ACK is rejected at the
message layer immediately; an ACK declaring 4 bytes with only 3 received is
incomplete and needs more input.

The existing TCP handlers still only accept PING at the coordinator and PONG at
the CLI, with empty payloads. Other types, nonzero payload declarations, and
malformed headers terminate that connection without a protocol error response.
Connections can carry multiple empty PING/PONG exchanges in order; the CLI
currently performs one exchange per invocation. Nonzero PING/PONG lengths in
header tests exercise generic length encoding, not valid complete messages.

## Byte order and examples

Big-endian encoding puts the most significant byte first. The numeric value
`0x00010203` is sent as `00 01 02 03`, regardless of the sender's CPU byte order.
The four bytes are binary values, not the text characters `"00010203"`.

A version 1 PING header with an empty payload is:

```text
46 4c 49 4e | 00 01 | 00 01 | 00 00 00 00
   FLIN    |   v1  |  PING |    0 bytes
```

An acknowledgment assigning worker ID 12 is a complete 16-byte frame:

```text
46 4c 49 4e | 00 01 | 00 04 | 00 00 00 04 | 00 00 00 0c
   FLIN    |   v1  |  ACK  |    4 bytes  | worker ID 12
```

Its heartbeat has the same payload and length, with message type `00 05`.
The length field is `4`, not `16`: it counts only the payload. The ID's `0c` is
hexadecimal for decimal 12. Neither the ID nor its digits are sent as text.

## C API

`include/protocol.h` declares constants, message IDs, error results, the
`struct faultline_header` host representation, and two header functions:

```c
enum faultline_protocol_result faultline_header_encode(
    uint8_t *wire, size_t wire_size, const struct faultline_header *header);

enum faultline_protocol_result faultline_header_decode(
    const uint8_t *wire, size_t wire_size, struct faultline_header *header);
```

For example, prepare a PING header for transmission:

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

### Complete-message API

`struct faultline_message` holds the host-order message type and worker ID.
The ID must be zero for empty messages and nonzero for ACK and HEARTBEAT.
The complete-message encoder derives magic, version, and payload length; callers
do not fill those fields or encode the header separately.

```c
enum faultline_protocol_result faultline_message_encode(
    uint8_t *wire, size_t wire_size, const struct faultline_message *message,
    size_t *written);

enum faultline_protocol_result faultline_message_decode(
    const uint8_t *wire, size_t wire_size, struct faultline_message *message,
    size_t *consumed);
```

For example, encode an acknowledgment with an already-assigned ID:

```c
struct faultline_message ack = {
    .message_type = FAULTLINE_MSG_WORKER_REGISTER_ACK,
    .worker_id = 12
};
uint8_t wire[FAULTLINE_HEADER_SIZE + FAULTLINE_WORKER_REGISTER_ACK_PAYLOAD_SIZE];
size_t written = 0;
enum faultline_protocol_result result =
    faultline_message_encode(wire, sizeof(wire), &ack, &written);
/* On OK, wire contains exactly the example above and written == 16. */
```

These functions work on caller-owned byte buffers. They perform no socket I/O,
allocate no memory, and do not assign IDs. The worker ID member represents the
payload for the current two messages that carry data; future job payloads will
need their own representation and validation.

## Validation and buffer ownership

All four functions require non-null pointers to valid, non-overlapping storage.
The caller owns all buffers; neither function allocates memory or performs I/O.
The encoder's size is writable capacity; the decoder's size is the number of
input bytes actually available.

Header validation proceeds in the following order and returns the first error:

| Result | Meaning |
| --- | --- |
| `FAULTLINE_PROTOCOL_INVALID_ARGUMENT` | A required pointer is null. |
| `FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL` | Fewer than 12 input bytes or output bytes of capacity. |
| `FAULTLINE_PROTOCOL_BAD_MAGIC` | Magic does not match `FLIN`. |
| `FAULTLINE_PROTOCOL_UNSUPPORTED_VERSION` | Version is not 1. |
| `FAULTLINE_PROTOCOL_UNKNOWN_MESSAGE_TYPE` | Type is not one of the five defined message IDs. |
| `FAULTLINE_PROTOCOL_PAYLOAD_TOO_LARGE` | Declared payload exceeds 1 MiB. |
| `FAULTLINE_PROTOCOL_OK` | Header was successfully encoded or decoded. |

On any error the output stays unchanged. Encoding validates before writing any
bytes. Decoding uses a temporary header and copies it to the caller only after
validation succeeds.

On success, encoding writes exactly 12 bytes and leaves any extra capacity
untouched. Decoding reads only the first 12 bytes and ignores trailing input.
It does not consume a stream buffer or report payload completion.

The complete-message functions have two additional errors:

| Result | Meaning |
| --- | --- |
| `FAULTLINE_PROTOCOL_INVALID_PAYLOAD_LENGTH` | The declared length differs from the message's exact required length. |
| `FAULTLINE_PROTOCOL_INVALID_WORKER_ID` | ACK/HEARTBEAT has ID zero, or an empty message's host representation supplies a nonzero ID. |

Message encoding checks pointers, message type, ID, and output capacity before
writing. It returns `BUFFER_TOO_SMALL` if the entire frame will not fit, without
writing even a partial header. Message decoding checks pointers, the header,
the required payload length, availability of the complete frame, and the ID.
A partial header or payload returns `BUFFER_TOO_SMALL`.

On any failure, message outputs and the `written`/`consumed` counters stay
unchanged. On success, encoding reports exactly 12 or 16 bytes written and leaves
extra capacity untouched. Decoding reports exactly one frame's size in
`consumed`, ignores following bytes, and sets `worker_id` to zero for empty
messages. The caller advances its own buffer only after a successful decode.

## Relationship to TCP framing

TCP provides a stream of bytes, not one complete message per `recv()` call.
The general receiving flow is:

1. Accumulate at least 12 bytes without discarding earlier partial input.
2. Decode the header and reject invalid field values.
3. Accumulate exactly the declared payload length before dispatching the message.
4. Retain any bytes belonging to following frames for subsequent parsing.

A short header is incomplete input, not necessarily a malformed message. The
current coordinator keeps partial headers per connection, and the CLI uses
`faultline_recv_exact()` to collect a response. Both detect EOF during a header.
The coordinator reads only the bytes remaining in one 12-byte header, leaving
any following frames in the socket's receive buffer until it is ready for them.
It rejects nonzero payload lengths immediately because the current handlers
require empty payloads.

The header encoding/decoding functions remain independent of transport: they
do not keep partial-read state, retry sends, or close connections. See
[the networking walkthrough](networking.md) for that implementation.

The complete-message decoder is also stateless. If only 14 bytes of an ACK
are buffered, its header is complete but two ID bytes are missing. It returns
`BUFFER_TOO_SMALL`; the caller keeps those 14 bytes, appends the remaining two,
and tries again. If two ACK frames are buffered together, a successful decode
reports `consumed == 16`, leaving the next 16 bytes for a second call.
Tests exercise this buffer behavior; wiring it into the live worker and
coordinator is the next step.

## Verification

Run `make test` for the normal build and `make test-sanitize` for AddressSanitizer
and UndefinedBehaviorSanitizer. The seven header test groups cover literal wire vectors,
payload boundaries, all header truncation lengths, invalid fields, invalid
encoder inputs, null arguments, and unaligned buffers with trailing bytes.

Literal expected byte sequences verify conformance independently of round-trip
tests. Failure cases also check that outputs remain unchanged. Socket unit tests
and process integration tests additionally exercise partial transfers,
timeouts, concurrent clients, malformed messages, and disconnected peers.

Seven additional complete-message test groups cover literal frames for all five
types, worker ID boundaries, every incomplete frame prefix (including payload
truncation), short output buffers, wrong lengths, invalid IDs/headers, null
arguments, unaligned buffers, untouched outputs, and consecutive complete frames
followed by a heartbeat completed in fragments.
