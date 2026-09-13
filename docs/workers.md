# Workers and the coordinator registry

The coordinator accepts WORKER_REGISTER, assigns a worker ID, returns
WORKER_REGISTER_ACK, and records HEARTBEAT messages from that connection.
Its registry is in `src/coordinator/worker_registry.c`, with the public interface
in `include/worker_registry.h`. The wire format is documented in
[protocol.md](protocol.md); the event loop is described in [networking.md](networking.md).

The worker executable now connects and registers, reads its assigned ID from
the acknowledgment, and stays connected. Periodic heartbeat sending,
missed-heartbeat detection, scheduling, and persistent recovery are not
implemented yet. Integration tests cover both real workers and controlled peers.

## Run two workers

Build with `make`, then start the coordinator and two workers in separate terminals:

```sh
# Terminal 1
./build/debug/faultline-coordinator

# Terminal 2
./build/debug/faultline-worker

# Terminal 3
./build/debug/faultline-worker
```

The first two registrations on a fresh coordinator receive distinct IDs:

```text
[INFO] worker registered worker_id=1 coordinator=127.0.0.1:9000
[INFO] worker registered worker_id=2 coordinator=127.0.0.1:9000
```

The IDs are assigned by registration order, not by terminal or process number.
Both workers remain connected. Stop either with Ctrl+C; the coordinator marks
that registration dead while the other stays connected. Each program also handles
SIGTERM. Output is line-buffered so the registration line appears immediately
when a worker's output is redirected to a file or captured by a test.

Use `faultline-worker --coordinator IPv4:PORT` to override the default endpoint,
or `--help` for usage (with the executable path above unless it is on PATH).
The CLI and worker share endpoint parsing and accept numeric IPv4 addresses
with port numbers 1 through 65535.

## Worker startup

`src/worker/main.c` performs these steps:

1. Parse arguments and configure signal handling.
2. Connect through the shared nonblocking socket helper.
3. Encode and send the 12-byte WORKER_REGISTER frame.
4. Collect and validate the 12-byte ACK header, then its four ID bytes.
5. Decode the complete ACK and retain the assigned ID.
6. Print registration success and wait with the connection open.
7. Close the socket on a local stop, disconnection, or error.

The worker accepts only WORKER_REGISTER_ACK with a four-byte payload and nonzero
ID. It does not announce registration after only receiving the header or a
partial ID. Its receive loop preserves fragments and has one five-second deadline
for the complete ACK. Invalid headers fail immediately; EOF during the header
or payload is a failed registration. Connect and send each have their own
five-second budget.

SIGINT/SIGTERM interrupt the ACK or idle wait promptly; connect/send may finish
their bounded operation first. A local stop exits successfully. Coordinator
disconnection, invalid ACKs, or unexpected data after registration exit with
failure. The worker does not yet send periodic heartbeats, process jobs, or
automatically reconnect. Its retained ID applies only to this connection.

## Identity and storage

The registry contains 64 records. Each record holds:

| Field | Meaning |
| --- | --- |
| `id` | Coordinator-issued unsigned 32-bit worker ID. Zero is unassigned. |
| `fd` | Current connection's socket descriptor; -1 for unused/dead records. |
| `state` | UNUSED, ALIVE, or DEAD. |
| `last_heartbeat_ms` | Registration's initial liveness baseline, then the coordinator's monotonic time of the latest accepted heartbeat. |

Registration takes the first unused or dead slot and assigns a fresh ID starting
at 1. IDs increase independently of descriptors and array positions. A request
rejected by the registry does not consume an ID. Once assigned, an ID stays spent
even if sending the acknowledgment fails. Assigning `UINT32_MAX` exhausts the counter;
subsequent registrations fail instead of wrapping to zero or reusing an old ID.

Consider this sequence:

```text
register A:       ID 1, fd 7, ALIVE
disconnect A:    ID 1, fd -1, DEAD
register B:       ID 2, fd 7, ALIVE
```

The operating system reused descriptor 7, but worker B did not inherit worker
A's identity. Heartbeat and death operations require a matching live ID and
descriptor pair. A stale operation for ID 1 cannot modify ID 2.

Dead records retain their ID and last timestamp until a new registration reuses
the slot. This is a bounded current registry, not an unlimited history. A live
slot is never replaced. Look up workers by ID each time; pointers into dead slots
must not be retained as identities because those slots may later hold new workers.

The registry is owned by the coordinator's single event loop. It allocates no
heap memory, opens/closes no sockets, and needs no locking in this usage. Only the
coordinator and registry test binary link its implementation. Registry memory
and the ID counter reset on coordinator restart; IDs are not durable identities
across coordinator runs. Old connections must register again after reconnecting.

## Lifecycle and timestamps

An empty client connection has worker ID zero and no registry entry. Receiving a
complete WORKER_REGISTER creates an ALIVE entry before queuing its ACK. If sending
that ACK fails, normal connection cleanup marks the new entry dead.

A complete heartbeat must contain the ID assigned to the sending connection.
The coordinator and registry both check ownership. An unregistered sender,
duplicate registration, a wrong ID, or a malformed message causes that connection
to close; an offending worker cannot update another worker's heartbeat time.

ALIVE means registered and not yet marked unavailable, rather than proof that
the remote process is healthy. DEAD means the registration's connection has
closed or been rejected. All registered-client close paths mark the entry dead
and clear its descriptor before the OS can reuse it. The last timestamp remains
available until the slot is replaced.

Times use `CLOCK_MONOTONIC` milliseconds, not wall-clock dates or worker-supplied
timestamps. Registration initializes the timestamp to give a future heartbeat
timeout a baseline before the first heartbeat arrives. Later, only complete,
validated heartbeats advance it. PINGs, replies, and partial messages only update
the separate I/O progress timestamp. The registry rejects a heartbeat timestamp
older than the one already stored; equal timestamps are allowed.

No heartbeat expiration scan runs yet. Registered workers waiting between
messages stay connected even if silent. The five-second transport inactivity
limit still protects unregistered clients, partial incoming frames, and pending
replies. The next heartbeat phase must add its own configurable timeout using
the registry timestamp.

## API

| Function | Operation |
| --- | --- |
| `faultline_worker_registry_init()` | Initialize an empty registry and ID counter once. |
| `faultline_worker_register()` | Add an ALIVE record and return a fresh ID. |
| `faultline_worker_find()` | Find an ALIVE or retained DEAD record by ID. |
| `faultline_worker_heartbeat()` | Verify the ID/connection pair and update heartbeat time. |
| `faultline_worker_mark_dead()` | Verify the ID/connection pair, mark DEAD, and detach the descriptor. |

The caller supplies monotonic timestamps, making the registry testable without
sleeping. Registry errors distinguish bad arguments, duplicate connections,
capacity, ID exhaustion, unknown IDs, dead workers, and connection mismatches.
Failed operations leave records and registration output unchanged. Socket
ownership stays with `main.c`; marking a worker dead does not itself close a socket.

## Manual wire-format peer

Build and start the coordinator in one terminal:

```sh
make
./build/debug/faultline-coordinator
```

For protocol inspection, this optional Python peer registers, prints its assigned
ID, sends one heartbeat, and disconnects. It uses only the Python standard library;
the real worker executable currently registers and waits without sending heartbeats.

```sh
python3 - <<'PY'
import socket
import struct

register = bytes.fromhex('46 4c 49 4e 00 01 00 03 00 00 00 00')
ack_header = bytes.fromhex('46 4c 49 4e 00 01 00 04 00 00 00 04')
heartbeat_header = bytes.fromhex('46 4c 49 4e 00 01 00 05 00 00 00 04')

with socket.create_connection(('127.0.0.1', 9000), timeout=2) as connection:
    connection.sendall(register)
    reply = bytearray()
    while len(reply) < 16:
        chunk = connection.recv(16 - len(reply))
        if not chunk:
            raise RuntimeError('Coordinator closed before the complete ACK')
        reply.extend(chunk)
    if reply[:12] != ack_header:
        raise RuntimeError('Unexpected acknowledgment header')
    worker_id = struct.unpack('!I', reply[12:])[0]
    if worker_id == 0:
        raise RuntimeError('Coordinator returned an unassigned ID')
    print(f'Registered as worker {worker_id}')
    connection.sendall(heartbeat_header + struct.pack('!I', worker_id))
PY
```

The coordinator logs `worker_registered`, `worker_register_ack_sent`,
`heartbeat_received`, and `worker_dead`. Registration/death events include state;
registration/heartbeat/death events include the last heartbeat timestamp. Each
event carries both `worker_id` and `fd` to distinguish identity from connection.
The descriptor on a death log identifies the socket being closed; the registry
record's descriptor has already been cleared to -1.

Run `make test` and `make test-sanitize` for registry unit tests, real coordinator
worker-message tests, and the existing protocol and TCP regression tests.
