# Co-op wire protocol, version 2 (N07)

This document specifies the asset-independent codec in
[`common/network/protocol.h`](../common/network/protocol.h) and
[`protocol.cpp`](../common/network/protocol.cpp). It defines binary frames for a
reliable ordered TCP byte stream. One TCP read need not contain one frame: a
frame may span reads, and a read may contain several frames.

N03 implemented encoding, incremental decoding and a bounded outbound queue.
N04 adds the nonblocking TCP connection and compatibility handshake in
[`session.cpp`](../common/network/session.cpp), with background fingerprints in
[`fingerprint.cpp`](../common/network/fingerprint.cpp). Real engines can now
connect on macOS loopback. N05 binds the host's saved companion to that connection.
N07 makes the guest a presentation-only process; N08 transfers state, and N09
routes input. The codec is shared engine source, not a separate mock protocol
used only by tests.

Version 2 adds `Accepted` so compatibility acceptance does not prematurely claim
that a character has been assigned. Version-1 peers are rejected explicitly.
N05 enables the already-defined `Welcome` message without changing its encoding;
the wire version remains 2. The exact executable fingerprint prevents an older
engine implementation from passing the compatibility handshake.

N06 adds a persistent, typed world-object registry and checkpoint format, without
changing the wire messages. See [world object identities](coop-objects.md) for
namespaces, player-role versus NPC identity, tombstones, save-local locators,
legacy migration and the generation checks required by future snapshot/update
messages. Runtime array indexes must not become network object references.

N07 does not change any wire fields. A guest without a snapshot waits without
creating a world. An explicitly loaded checkpoint uses immutable guest authority:
it can render and animate, but cannot advance the campaign, save it or acquire
authority on disconnect. See [guest authority](coop-guest.md) for loading,
clock separation, blocked effects and the acceptance test. `connected` and
`Welcome` remain transport/binding facts, not snapshot readiness.

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
| 8 | 2 | Version | Exactly `2` |
| 10 | 2 | Message type | Defined below; unknown types are fatal |
| 12 | 8 | Generation | Host-issued connection/world generation; zero before acceptance |
| 20 | 8 | Player | Persistent actor ID assigned by the host; zero before actor binding |
| 28 | 8 | Input sequence | Nonzero only in `Input` |
| 36 | 8 | State revision | Nonzero only in `Welcome` and `StateAck` |

The frame length must be between 44 and 65,536 bytes inclusive. Every current
message also has an **exact** length; the global maximum does not permit padding,
extra fields or an arbitrary payload. The decoder checks the prefix after eight
bytes and validates the complete header before buffering the body. The largest
current frame is 108 bytes. The larger cap leaves room for future bounded
snapshot chunks; snapshot messages are not defined in version 2.

## Message definitions

| Type | Name | Direction | Total bytes | Generation / player / sequence / revision |
| --- | --- | --- | --- | --- |
| 1 | `Hello` | Guest → host | 108 | `0 / 0 / 0 / 0` |
| 2 | `Welcome` | Host → guest | 44 | `nonzero / nonzero / 0 / nonzero` |
| 3 | `Input` | Guest → host | 52 | `nonzero / nonzero / nonzero / 0` |
| 4 | `StateAck` | Host → guest | 52 | `nonzero / nonzero / 0 / nonzero` |
| 5 | `Reject` | Host → guest | 46 | `0 / 0 / 0 / 0` |
| 6 | `Accepted` | Host → guest | 108 | `nonzero / 0 / 0 / 0` |

### Hello

| Offset | Bytes | Field |
| --- | --- | --- |
| 44 | 32 | Engine compatibility fingerprint |
| 76 | 32 | Game-data/mod compatibility fingerprint |

The guest sends this first. These fields contain the actual SHA-256 fingerprints
described below. The host compares both before accepting the connection. No
player identity is requested by this message.

### Accepted

The body has the same layout as `Hello`, containing the host's fingerprints. The
guest verifies both before marking the connection accepted. The nonzero header
generation is host-issued; player, sequence and revision remain zero. Acceptance
reserves the single guest slot; the host binds the character in a subsequent step.

The current session implementation accepts only `Hello` on the host and
`Accepted`/`Reject` on the guest during the handshake. After acceptance, the guest
accepts one `Welcome`. All other application messages close that peer with an
observable reason. An accepted connection remains open until disconnect/error
or world replacement. There is no heartbeat or idle timeout after acceptance yet.

### Welcome

No body. The host supplies the generation, assigned persistent player identity
and initial authoritative revision in the header. It is the only message that
establishes a binding. It does not mean that the guest has loaded a world or may
already send gameplay input; snapshot transfer/readiness is a later protocol
extension (N08).

Host ID `1` and companion ID `2` are persistent roster identities, separate from
socket IDs and connection order. The codec accepts any nonzero player field.
The session layer reserves ID 1 for the host and binds the single supported
guest only to ID 2. A syntactically valid frame is not proof of authority.

The host's game-thread integration waits for an idle loader and a host actor,
then resolves ID 2 in the persistent world roster. Only if absent does it create
a companion. Resolving an existing actor changes no HP, inventory, position,
heading or script identity. `HERO` and `PC_HERO` continue to reference host ID 1.
The transport keeps no NPC pointer or runtime array index. Its host-only
`bindCompanion(generation)` API takes no requested player ID and refuses a stale
generation. Repeating it for the live generation succeeds without another frame.

The host sends `Welcome(generation, player=2, sequence=0, revision=1)` after
`Accepted`. Revision 1 is the initial binding baseline; no replicated state or
ongoing revision tracking exists yet. The guest requires the accepted generation,
ID 2 and revision 1. A mismatched or repeated `Welcome` closes it with
`invalid_binding`; one before `Accepted` fails the handshake. A guest sending
`Welcome` to the host, or naming host/unknown player IDs after acceptance, is
disconnected with `authority_violation`. Even input naming ID 2 is disabled and
closes with `unexpected_message` until the later snapshot/readiness work.

Binding does not create an actor in the guest world, switch its local camera or
route its input. These require N08–N09. The host reports its binding when the
Welcome is queued; the guest reports it only after validating the received frame.
This is assignment metadata, not a guest readiness acknowledgement.

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

Reserved for later gameplay replication; not enabled by the N05 session.
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
closing without a reply is also valid; a peer might not understand a version-2
reject. Runtime connection failure and stale input are handled by the session
layer, not by sending an unbound reject in the middle of gameplay.

## Session and ordering rules for the connection layer

The codec validates structural constraints. The session enforces compatibility,
direction/phase checks, admission, and companion assignment. Gameplay input,
sequence tracking and advancing state revisions remain requirements for later tasks.

1. An unbound connection accepts one `Hello`. The host either rejects it or
   returns one `Accepted`. Both sides verify compatibility; only then may the
   host issue a `Welcome` binding. World readiness comes later. Unexpected
   message types for the direction/phase close the peer. A valid `Input` cannot
   bypass this.
2. The host issues a fresh nonzero generation for each accepted connection,
   using a random 64-bit seed and increasing counter; counter wrap is refused.
   Reconnect changes it. Starting a checkpoint/new-world load or exiting the
   session closes all peers and discards their binding and queues, retaining
   the host listener and generation counter. Generation is a
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

N05 tests assignment authority and invalidates connections on world replacement.
Ordinary saves retain the binding and generation; the binder does not touch a
world while the asynchronous loader owns it. Disconnect preserves the companion
in the host world and its next checkpoint, while clearing transport authority.
Restart loads the version-1 `coop/players` roster, which accepts only IDs 1 and 2;
no socket binding/generation is stored in a save. Rejoining resolves that actor
and receives a fresh generation. Synchronizing active input with a checkpoint,
clearing held network input and resuming a guest from a snapshot remain N08–N14.

## Starting and observing a connection

Pass exactly one of these explicit startup options:

```text
-coop-host PORT
-coop-join 127.0.0.1:PORT
```

Host ports are `0..65535`; zero requests an OS-assigned free port. Guest ports
are `1..65535`. The host binds only IPv4 `127.0.0.1`; the guest accepts that exact
address, with no DNS resolution or other interfaces. Duplicate/conflicting
options, missing values and malformed endpoints fail command-line validation.
Without either option, no fingerprint scan or network socket is started.

The macOS implementation is tested. Fingerprinting currently uses CommonCrypto
and macOS executable discovery; other platforms report unsupported preparation
instead of accepting an unchecked peer. Porting fingerprint support is separate
work. POSIX transport code is isolated from the engine/renderer.

Fingerprinting runs once in a cancellable background job. Sockets start after it
succeeds; preparation failure leaves the game running and reports the error.
Socket operations are nonblocking, including connect (zero-timeout readiness
poll plus `SO_ERROR`). Each peer gets at most four 4-KiB reads and four writes per
render tick; at most four accepts are attempted. There are at most four sockets
including accepted and pending peers. Excess pending peers are closed and
reported as `pending_limit`. An ordinary second guest receives `Reject(Full)`
without replacing the accepted guest.

The five-second deadline covers connect plus handshake, measured from socket
creation/acceptance, and is checked every network tick. A rejected peer has a
bounded five-second drain window before closure. Silent or partial-handshake
peers are closed on timeout; the host listener and game remain active. EOF/reset
releases admission so a fresh guest can connect. Failed guests do not reconnect
automatically. Socket errors cannot raise SIGPIPE and terminate the engine.

State transitions and reasons appear in `log.txt` as `Coop: ...`. Development
`state` replies expose `network`: role, state, reason, bound port, protocol,
generation, `bound_player`, `binding_revision`, cumulative `bindings`, live peer
count, acceptance/rejection/timeout/disconnect counters and both fingerprints.
`bound_player` and `binding_revision` are zero until binding and after disconnect.
States include `disabled`, `fingerprinting`, `listening`,
`connecting`, `handshaking`, `connected`, `rejected`, `failed`, `disconnected`, or
`fingerprint_error: ...`. Host state remains `connected` when refusing an extra
guest; the reason/counter reports that separate event. `connected` means the
compatibility handshake passed, even while waiting for binding. A bound guest
owns companion ID 2 on the host; it still awaits replicated world readiness.

## Exact compatibility fingerprint inputs

The engine fingerprint is SHA-256 of the running executable's file bytes,
located through the platform executable API rather than a caller-supplied
version string. This deliberately requires identical binaries, including dirty
builds, architecture and build mode; cross-build compatibility is not negotiated.

The game-data fingerprint is SHA-256 of a canonical manifest. Every integer in
the manifest is unsigned 64-bit big-endian; every text field is its byte length
followed by its UTF-8 bytes. Digests are 32 raw bytes. Append in this order:

1. Text `OpenGothic-data-manifest-v1`.
2. Settings count, then these text fields in order: `game=N` (detected game),
   `patch=N` (detected patch), `language=N` (`[GAME] language`), and
   `mod=FILENAME` (selected mod INI basename, empty if none).
3. Successful mounted-archive count, then each archive's path relative to the
   installation's Data directory, in the actual engine mount order. This records
   override order and failed/omitted mounts as well as configuration.
4. File-entry count, then each entry's logical name and content SHA-256. Entries
   are sorted by logical name. Names are `Data/RELATIVE_PATH`, `_work/RELATIVE_PATH`
   and, when selected, `selected-mod` for the mod INI. Paths use `/`, with original
   case preserved. Absolute installation paths are excluded.

All regular files under Data and _work are included, including unused archives
and resources; this is a conservative compatibility check. `.DS_Store` is
excluded. The selected mod INI's bytes are included even though it lives outside
those trees. Symlinks, special files, missing/unreadable inputs, more than 100,000
tree entries or manifest text over 4,096 bytes fail preparation. There is no
mtime-only cache: equal-size modifications with preserved mtime still change the
digest. File size and mtime are also checked around each read to reject observed
concurrent modification. Runtime saves/logs and display/control settings are
outside the manifest. Same content in a different installation directory agrees.

The installation, selected settings and executable must stay unchanged while a
network session runs; the fingerprint is a startup snapshot, not a file watcher
or a filesystem transaction. Changing assets/configuration requires restarting
the peers. The hashes compare compatibility and do not authenticate a player.

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
Connections are loopback-only. Internet transport/security is outside N04.

## Golden example

This 52-byte `Input` uses generation `0x0102030405060708`, player `2`, sequence
`9`, revision `0`, held `Forward | Walk`, look X `-5` and look Y `100`:

```text
4f474350 00000034 0002 0003
0102030405060708 0000000000000002
0000000000000009 0000000000000000
00000081 fffb 0064
```

The test suite checks this literal against both encoding and decoding. It also
checks an independent `Hello` vector, every split point and byte-at-a-time input,
combined frames, invalid headers/payloads, all truncated prefixes, bounded queues,
blocked/partial writes, 10,000 ordered input messages under changing read/write
boundaries and 10,000 deterministic mutated/truncated frames.
On macOS, seven additional test cases exercise endpoint parsing, real loopback
acceptance and reconnect, both directions of fingerprint validation, version
rejection, second-guest rejection, deadlines, pending-socket limits, and the
actual file/manifest hash implementation using temporary files. Binding tests
cover host-only assignment, idempotence, reconnect/load invalidation, second-guest
isolation, incorrect IDs/generations/revisions, early/duplicate Welcome frames,
and forged guest assignments/inputs. The engine integration test additionally
checks live and saved NPC reuse, separate inventory/HP/poses and script identity.

## Build and test without game files

Only a C++20 compiler and CMake are required; the commands below use Ninja.
This standalone project does not configure OpenGothic's dependencies, read Steam
assets, start a GUI, or require initialized engine submodules. It runs 13 codec
cases plus four object-registry cases everywhere, and seven connection/fingerprint
cases on macOS (24 total); the latter use
loopback sockets and may need permission outside an agent's network sandbox.

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
