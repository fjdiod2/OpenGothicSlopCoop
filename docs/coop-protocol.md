# Co-op wire protocol, version 1 (N03)

This document specifies the asset-independent codec in
[`common/network/protocol.h`](../common/network/protocol.h) and
[`protocol.cpp`](../common/network/protocol.cpp). It defines binary frames for a
reliable ordered TCP byte stream. One TCP read need not contain one frame: a
frame may span reads, and a read may contain several frames.

N03 implements encoding, incremental decoding and a bounded outbound queue.
There are no sockets, listening ports, connected engines or replicated gameplay
yet. N04 adds connection handling and handshake validation; N05 binds a guest
to a saved companion; N09 enforces input authority and sequence rules. The codec
is shared engine source, not a separate mock protocol used only by tests.

## Encoding and frame header

All integers are unsigned, big-endian, unless explicitly marked signed. Signed
16-bit values use two's complement. Serialize fields individually: C++ object
layout, padding, native endianness, pointers and engine save serialization are
never part of this wire format. Fingerprints are raw bytes, not hexadecimal text.

Every frame begins with this 44-byte header. Offsets are zero-based:

| Offset | Bytes | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 4 | Magic | Literal ASCII `OGCP` (`4f 47 43 50`) |
| 4 | 4 | Frame length | Total bytes, including this header |
| 8 | 2 | Version | Exactly `1` |
| 10 | 2 | Message type | Defined below; unknown types are fatal |
| 12 | 8 | Generation | Host-issued session/world generation; zero before binding |
| 20 | 8 | Player | Persistent actor ID assigned by the host; zero before binding |
| 28 | 8 | Input sequence | Nonzero only in `Input` |
| 36 | 8 | State revision | Nonzero only in `Welcome` and `StateAck` |

The frame length must be between 44 and 65,536 bytes inclusive. Every current
message also has an **exact** length; the global maximum does not permit padding,
extra fields or an arbitrary payload. The decoder checks the prefix after eight
bytes and validates the complete header before buffering the body. The largest
current frame is 108 bytes. The larger cap leaves room for future bounded
snapshot chunks; snapshot messages are not defined in version 1.

## Message definitions

| Type | Name | Direction | Total bytes | Generation / player / sequence / revision |
| --- | --- | --- | --- | --- |
| 1 | `Hello` | Guest → host | 108 | `0 / 0 / 0 / 0` |
| 2 | `Welcome` | Host → guest | 44 | `nonzero / nonzero / 0 / nonzero` |
| 3 | `Input` | Guest → host | 52 | `nonzero / nonzero / nonzero / 0` |
| 4 | `StateAck` | Host → guest | 52 | `nonzero / nonzero / 0 / nonzero` |
| 5 | `Reject` | Host → guest | 46 | `0 / 0 / 0 / 0` |

### Hello

| Offset | Bytes | Field |
| --- | --- | --- |
| 44 | 32 | Engine compatibility fingerprint |
| 76 | 32 | Game-data/mod compatibility fingerprint |

The guest sends this first. These fixed-size fields reserve SHA-256 fingerprints
for the engine build and game-data/mod configuration. N04 must define exactly
which inputs are hashed and compare them; N03 treats them as opaque bytes and
does not establish compatibility from their presence alone. No player identity
is requested by this message.

### Welcome

No body. The host supplies the generation, assigned persistent player identity
and current authoritative revision in the header. It is the only message that
establishes a binding. It does not mean that the guest has loaded a world or may
already send gameplay input; snapshot transfer/readiness is a later protocol
extension (N08).

Host ID `1` and companion ID `2` are persistent roster identities, separate from
socket IDs and connection order. The codec accepts any nonzero player field.
The session layer must reserve ID 1 for the host and bind the first supported
guest only to ID 2 (N05). A syntactically valid frame is not proof of authority.

### Input

| Offset | Bytes | Field |
| --- | --- | --- |
| 44 | 4 | Held-button bit mask |
| 48 | 2 | Signed relative look X, in input pixels |
| 50 | 2 | Signed relative look Y, in input pixels |

| Bit | Mask | Action |
| --- | --- | --- |
| 0 | `0x01` | Forward |
| 1 | `0x02` | Backward |
| 2 | `0x04` | Turn left |
| 3 | `0x08` | Turn right |
| 4 | `0x10` | Strafe left |
| 5 | `0x20` | Strafe right |
| 6 | `0x40` | Jump |
| 7 | `0x80` | Walk modifier |

All other bits must be zero. Both look deltas must be in `[-1000, 1000]`.
The button mask replaces the previously held state; zero releases every button.
Look deltas are applied once per accepted sequence. This is input intent, not
position, elapsed simulation time, damage or a script command. The host determines
simulation timing and gameplay effects. N09 will map these actions to controls;
discrete interactions and inventory commands require a later extension.

### StateAck

| Offset | Bytes | Field |
| --- | --- | --- |
| 44 | 8 | Highest consecutively applied input sequence, or zero before any input |

The header revision identifies the authoritative state after applying that
input. This message carries a revision and acknowledgement only; it contains no
pose or world state and does not replicate either. N08/N10 will define snapshots
and state updates separately.

### Reject

| Offset | Bytes | Field |
| --- | --- | --- |
| 44 | 2 | Rejection reason |

Reasons: `1` protocol version, `2` engine fingerprint, `3` game-data fingerprint,
`4` host full, `5` invalid session/handshake. Any other value is malformed. No
arbitrary error string crosses the wire. A reject is a terminal handshake reply,
followed by closing the connection. On an undecodable/unsupported peer version,
closing without a reply is also valid; a peer might not understand a version-1
reject. Runtime connection failure and stale input are handled by the session
layer, not by sending an unbound reject in the middle of gameplay.

## Session and ordering rules for the connection layer

These rules specify how later tasks must use the fields. N03 validates their
structural constraints, but does **not** implement a handshake state machine,
direction checks, authority, sequence tracking or gameplay acceptance.

1. An unbound connection accepts one `Hello`. The host either rejects it or
   returns one `Welcome`. Reject unexpected message types for the connection's
   direction and phase. Do not accept `Input` merely because it decodes.
2. The host issues a fresh nonzero generation for a binding/world epoch. Change
   it on reconnect, checkpoint load or world replacement before accepting more
   input. Never reuse a previous generation for a new binding. Generation is a
   stale-session fence, not authentication; a player ID is not a credential.
3. After binding and world readiness, every input must match that connection's
   generation and assigned player. A guest cannot retarget an input to host ID 1
   by editing its header. Saved actor identity survives reconnect; the connection
   generation does not.
4. Input sequences start at 1 per generation and increase by exactly 1. The host
   applies each at most once. Duplicate/older sequences have no additional
   effect; a forward gap is a protocol/session error on this ordered stream.
   Old-generation input cannot mutate the current world. Do not wrap sequence
   counters; establish a new generation before exhaustion.
5. Host revisions start at 1 and increase for authoritative state changes.
   Repeated acknowledgements may have the same revision and applied-input value;
   neither may decrease within a generation. An acknowledgement cannot refer to
   input the guest has not sent. Revisions are scoped by generation, and must not
   wrap. They do not promise that a save has committed to disk.
6. On disconnect, clear the actor's held controls so the last movement command
   cannot keep running. Reconnect requires fresh binding/readiness and resets
   sequence tracking, while preserving the saved companion's identity/state.

Actual freshness generation, compatibility fingerprints, connection deadlines,
read/write budgets per tick, handshake enforcement and authority tests belong to
N04/N05/N09. This codec makes no internet-security or campaign-correctness claim.

## Bounds, failures and stream handling

`Decoder` and `WriteQueue` each default to 256 queued messages and 262,144 wire
bytes. Optional lower limits are supported for testing/tuning; byte limits must
be at least 44 and cannot exceed the defaults, and message limits are `1..256`.
These are wire-byte accounting bounds, not exact heap sizes; message/container
overhead is also bounded by the message count. No allocation uses an unchecked
peer-supplied frame length.

The decoder counts completed queued frames plus the partial frame against its
byte limit. Its message limit counts completed frames; one partial frame may
also be held within the byte limit. Feed arbitrary nonempty or empty read fragments, then call `pop()` to
obtain complete messages in wire order. It retains no borrowed read-buffer
pointers. A clean EOF (`finish()`) permits draining completed messages. EOF inside
a frame is a truncation error, even if earlier complete frames were queued.

Bad magic, version, type, length, fields, payload or receive-queue overflow is
fatal. The decoder discards pending messages and partial bytes, remembers the
failure, and rejects later `feed`, `pop` and `finish` calls. Do not scan for the
next magic or reuse a poisoned decoder. Already-consumed messages cannot be
rolled back; validation of a stream is not a transaction. On a clean EOF, further
`feed` calls fail with `Closed`; repeated `finish` calls are harmless.

Outbound queue overflow returns `false` without modifying the queue. The owner
must back off or disconnect; it must never silently drop, overwrite or reorder
an input/state message. Invalid outbound messages throw a protocol error before
queuing. Retained-byte accounting includes the **entire** partially sent front
frame until it is fully consumed, so tiny partial sends cannot evade the cap.

`front()` returns only the unsent suffix of the oldest frame. After a nonblocking
`send`, pass exactly its positive return count to `consume()`. A short write
keeps the suffix for the next writable event. On would-block, leave it untouched
(`consume(0)` is harmless); on interrupted calls, retry within the connection's
work budget. Socket errors or closure are the owner's responsibility: close and
discard that connection's queue. `consume(n)` greater than the offered suffix
throws without changing the queue; counts cannot cross frame boundaries. Treat
the returned span as borrowed and reacquire it after modifying the queue.

There is no checksum in a frame and no encryption/authentication in this codec.
Task N04 starts on loopback. Internet transport/security is outside N03.

## Golden example

This 52-byte `Input` uses generation `0x0102030405060708`, player `2`, sequence
`9`, revision `0`, held `Forward | Walk`, look X `-5` and look Y `100`:

```text
4f474350 00000034 0001 0003
0102030405060708 0000000000000002
0000000000000009 0000000000000000
00000081 fffb 0064
```

The test suite checks this literal against both encoding and decoding. It also
checks an independent `Hello` vector, every split point and byte-at-a-time input,
combined frames, invalid headers/payloads, all truncated prefixes, bounded queues,
blocked/partial writes, 10,000 ordered input messages under changing read/write
boundaries and 10,000 deterministic mutated/truncated frames.

## Build and test without game files

Only a C++20 compiler and CMake are required; the commands below use Ninja.
This standalone project does not configure OpenGothic's dependencies, read Steam
assets, start a GUI, or require initialized engine submodules.

```sh
cmake -S tests/protocol -B build-protocol -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-protocol --parallel 4
ctest --test-dir build-protocol --output-on-failure
```

With Clang/GCC, add `-DCOOP_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug` at configuration
time for AddressSanitizer and UndefinedBehaviorSanitizer. The tests use explicit
checks that remain active in optimized builds, and undefined-behavior findings
are fatal in the sanitized build. The parent tools repository's
`tools/run-protocol-tests.py` records configure/build/test logs, source hashes and
`result.json`; `--sanitizers` selects the sanitized build.

## Version changes

Unknown versions and message types are rejected, and body sizes are exact.
Adding fields, changing their meaning or adding new messages requires a protocol
version bump (or an explicitly designed negotiation extension in a later version).
Update this document and the fixed byte vectors with every wire change. Do not
reinterpret saved player identity as a connection ID or silently relax limits.
