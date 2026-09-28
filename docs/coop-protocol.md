# Co-op wire protocol, version 3 (N09)

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
N07 makes the guest a presentation-only process; N08 transfers a host checkpoint,
and N09 routes movement/turning input with ordered acknowledgements. The codec
is shared engine source, not a separate mock protocol used only by tests.

Version 3 adds snapshot begin/chunk/end/readiness messages. Older versions are
rejected explicitly. Version 2 introduced `Accepted`, separating compatibility
from companion assignment. Existing messages retain their layouts, apart from
the required version field. The exact executable fingerprint also prevents
mixing different engine implementations.

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
| 8 | 2 | Version | Exactly `3` |
| 10 | 2 | Message type | Defined below; unknown types are fatal |
| 12 | 8 | Generation | Host-issued connection/world generation; zero before acceptance |
| 20 | 8 | Player | Persistent actor ID assigned by the host; zero before actor binding |
| 28 | 8 | Input sequence | Nonzero only in `Input` |
| 36 | 8 | State revision | Nonzero in `Welcome`, `StateAck` and snapshot messages |

The frame length must be between 44 and 65,536 bytes inclusive. Fixed messages
have exact lengths. `SnapshotChunk` permits only its eight-byte offset and
1–32,768 data bytes (53–32,820 bytes total). The decoder validates the prefix
after eight bytes and the complete header before buffering the body. Padding
and undeclared extensions are forbidden.

## Message definitions

| Type | Name | Direction | Total bytes | Generation / player / sequence / revision |
| --- | --- | --- | --- | --- |
| 1 | `Hello` | Guest → host | 108 | `0 / 0 / 0 / 0` |
| 2 | `Welcome` | Host → guest | 44 | `nonzero / nonzero / 0 / nonzero` |
| 3 | `Input` | Guest → host | 52 | `nonzero / nonzero / nonzero / 0` |
| 4 | `StateAck` | Host → guest | 52 | `nonzero / nonzero / 0 / nonzero` |
| 5 | `Reject` | Host → guest | 46 | `0 / 0 / 0 / 0` |
| 6 | `Accepted` | Host → guest | 108 | `nonzero / 0 / 0 / 0` |
| 7 | `SnapshotBegin` | Host → guest | 84 | `nonzero / 2 / 0 / nonzero` |
| 8 | `SnapshotChunk` | Host → guest | 53–32,820 | `nonzero / 2 / 0 / nonzero` |
| 9 | `SnapshotEnd` | Host → guest | 44 | `nonzero / 2 / 0 / nonzero` |
| 10 | `SnapshotReady` | Guest → host | 44 | `nonzero / 2 / 0 / nonzero` |

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
accepts one `Welcome`, followed by one snapshot transfer. The host accepts its
readiness acknowledgement. Other application messages close that peer with an
observable reason. An accepted connection remains open until disconnect/error
or world replacement. There is no heartbeat or idle timeout after acceptance yet.

### Welcome

No body. The host supplies the generation, assigned persistent player identity
and initial authoritative revision in the header. It is the only message that
establishes a binding. It does not mean that the guest has loaded a world or may
already send gameplay input; the snapshot barrier described below must finish.

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
`Accepted`. Revision 1 is the initial binding/snapshot baseline; ongoing
revision tracking awaits N10. The guest requires the accepted generation,
ID 2 and revision 1. A mismatched or repeated `Welcome` closes it with
`invalid_binding`; one before `Accepted` fails the handshake. A guest sending
`Welcome` to the host, or naming host/unknown player IDs after acceptance, is
disconnected with `authority_violation`. Input naming ID 2 before snapshot
readiness closes with `unexpected_message`. After readiness the rules below apply.

Binding does not create an actor in the guest world, switch its local camera or
route its input. Snapshot activation selects its camera; input routing is enabled
after readiness. The host reports its binding when the Welcome is queued; the
guest reports it only after validating the received frame.
This is assignment metadata, not a guest readiness acknowledgement.

### Initial snapshot and readiness barrier (N08)

`SnapshotBegin` contains a big-endian u64 archive length at byte 44 and a raw
32-byte SHA-256 at byte 52. Length must be 1–67,108,864 bytes (64 MiB).
`SnapshotChunk` contains a big-endian u64 archive offset at byte 44 and 1–32 KiB
of data starting at byte 52. `SnapshotEnd` and `SnapshotReady` have no body.
Every frame must match the accepted generation, companion ID 2 and the same
nonzero snapshot revision. Begin/end occur once; chunks must be contiguous,
start at zero and fill the declared length exactly. Wrong direction, phase,
offset, generation, identity, revision, size or checksum closes the connection.

After binding on the game thread, the host clears held controls and serializes
its current version-56 checkpoint, including both actors, inventories, script
variables/quests, world objects, registry and saved world tick. A tiny zeroed
preview replaces the normal screenshot. Generation/revision belong to the
transport envelope, not the persistent save. Capture is synchronous at one
simulation boundary. The host then holds simulation and input until the guest's
`SnapshotReady` arrives. Therefore no world updates accumulate during loading;
N08 needs no catch-up log. Joining during dialogue, inventory, video or a cutscene
waits for that interaction to finish. Host saving is denied during this barrier.

The connection queues bounded chunks as capacity permits; it does not enqueue
the entire archive. The guest allocates only received bytes, bounded by the
announced total, checks SHA-256 after the complete end marker, then hands those
bytes to the background checkpoint loader with immutable **Guest** authority.
It selects local companion ID 2 while retaining canonical host `HERO`, then
queues `SnapshotReady`. No campaign startup or guest save-file copy is involved.
The host resumes on receipt. N09 enables input only after this barrier.

Transfer plus load has a 180-second deadline on each side, starting at send/begin.
Disconnect, timeout or load failure discards transfer state and releases the host
barrier. Partial bytes never reach the loader. If a completed loader returns
after its connection/generation was cancelled, the result is discarded before
a frame can display it or acknowledge readiness. A loaded guest remains read-only
on subsequent disconnect. TCP/SHA-256 detect incomplete/corrupt data; they do not
authenticate a hostile host or make arbitrary native save parsing safe.

Development observations include `snapshot_state`, `snapshot_revision`,
`snapshot_bytes`, `snapshot_transferred`, `snapshot_digest`, `snapshot_tick` and
`paused`. Host transferred bytes count queued archive bytes; guest bytes count
received archive bytes. Test-control launches retain the original host archive
at `snapshot_artifact` for independent comparisons. Normal launches use memory.
The asset-free tests exercise large transfers, offsets, checksum, duplicate
begin, early end, wrong direction/generation/identity/revision, timeout, partial
EOF and cancellation while the application owns a completed snapshot.

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
simulation timing and gameplay effects. N09 enables bits 0–5 through the guest's
normal key mappings (including alternate keys). Jump/Walk remain reserved and
are rejected by the session as `unsupported_input`; interactions, inventory and
combat need later gameplay tasks. Walk is a toggle in the original controller,
so it must not be incorrectly interpreted as a held modifier.

Each changed held mask sends one sequence; relative mouse packets preserve the
mask and apply their look once. Mouse uses raw pixels, host sensitivity/window
size and a 36-degree total impulse cap per render frame. Guest camera look stays
local. Host processing never uses client elapsed time or client positions.
Only companion ID 2's controller receives these commands. Before readiness,
during loading or after disconnect, guest controls cannot send gameplay input.
Losing guest window focus sends a release; alternate keys for the same action
are combined so releasing one does not cancel another still held.

The host validates ID/generation and a contiguous sequence starting at 1 before
placing input in its bounded 64-message application queue. Old-generation and
duplicate/older-sequence packets are ignored and counted without changing held
state, mouse impulse or the expected next sequence. A forward gap, wrong player,
unsupported action or full queue closes the peer. The game thread drains inputs
only with the host world available and snapshot ready, applies control changes,
then queues acknowledgements. Disconnect/world replacement clears queued input;
controller-held buttons and mouse impulses are released on generation loss.
The full disconnect/save acceptance scenario remains N11–N13.

### StateAck

| Offset | Bytes | Field |
| --- | --- | --- |
| 44 | 8 | Highest consecutively applied input sequence, or zero before any input |

N09 enables this after applying input to the assigned controller. The header
revision is currently the snapshot baseline 1; advancing pose revisions are N10.
This acknowledges control-state application, not a completed movement tick or
committed save. The guest rejects acknowledgements for another generation/player,
sequences it never sent, or decreasing sequence/revision. This message carries
a revision and acknowledgement only; it contains no
pose or world state and does not replicate either. N08 snapshots and N10 pose
updates use separate messages.

### Reject

| Offset | Bytes | Field |
| --- | --- | --- |
| 44 | 2 | Rejection reason |

Reasons: `1` protocol version, `2` engine fingerprint, `3` game-data fingerprint,
`4` host full, `5` invalid session/handshake. Any other value is malformed. No
arbitrary error string crosses the wire. A reject is a terminal handshake reply,
followed by closing the connection. On an undecodable/unsupported peer version,
closing without a reply is also valid; a peer might not understand a version-3
reject. Runtime connection failure and stale input are handled by the session
layer, not by sending an unbound reject in the middle of gameplay.

## Session and ordering rules for the connection layer

The codec validates structural constraints. The session enforces compatibility,
direction/phase checks, admission, companion assignment, snapshot readiness and
input sequence tracking. Advancing pose revisions are N10.

1. An unbound connection accepts one `Hello`. The host either rejects it or
   returns one `Accepted`. Both sides verify compatibility; only then may the
   host issue a `Welcome` binding. World readiness follows the snapshot barrier. Unexpected
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
   N09 retains the snapshot revision until N10. Repeated acknowledgements may
   have the same revision and applied-input value;
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
clearing held network input and coordinating save/load input remain N09–N14.

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
poll plus `SO_ERROR`). Each peer gets at most four 16-KiB reads and four writes of at most 16 KiB per
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
owns companion ID 2 on the host. Input observations add `input_sent`,
`input_received`, `input_applied`,
`input_acknowledged`, `acknowledged_revision`, cumulative `input_duplicates` /
`input_stale`, guest `held_buttons` and host `remote_buttons`. Sequence counters
reset with the generation. `snapshot_state == "ready"` reports snapshot
activation (guest) or receipt of that acknowledgement (host).

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
4f474350 00000034 0003 0003
0102030405060708 0000000000000002
0000000000000009 0000000000000000
00000081 fffb 0064
```

The test suite checks this literal against both encoding and decoding. It also
checks an independent `Hello` vector, every split point and byte-at-a-time input,
combined frames, invalid headers/payloads, all truncated prefixes, bounded queues,
blocked/partial writes, 10,000 ordered input messages under changing read/write
boundaries and 10,000 deterministic mutated/truncated frames.
On macOS, eleven additional test cases exercise endpoint parsing, real loopback
acceptance and reconnect, both directions of fingerprint validation, version
rejection, second-guest rejection, deadlines, pending-socket limits, and the
actual file/manifest hash implementation using temporary files. Input tests cover
readiness, delivery/acknowledgement, duplicate and old-generation packets, gaps,
wrong identities, reserved actions, application queue bounds and impossible acks.
Binding tests cover host-only assignment, idempotence, reconnect/load invalidation, second-guest
isolation, incorrect IDs/generations/revisions, early/duplicate Welcome frames,
and forged guest assignments/inputs. The engine integration test additionally
checks live and saved NPC reuse, separate inventory/HP/poses and script identity.

## Build and test without game files

Only a C++20 compiler and CMake are required; the commands below use Ninja.
This standalone project does not configure OpenGothic's dependencies, read Steam
assets, start a GUI, or require initialized engine submodules. It runs 13 codec
cases plus four object-registry cases everywhere, and eleven connection/fingerprint
cases on macOS (28 total); the latter use
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

Unknown versions and message types are rejected, and body sizes are validated.
Adding fields, changing their meaning or adding new messages requires a protocol
version bump (or an explicitly designed negotiation extension in a later version).
Update this document and the fixed byte vectors with every wire change. Do not
reinterpret saved player identity as a connection ID or silently relax limits.
