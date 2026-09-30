# Co-op wire protocol, version 13 (guest jump/sneak)

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

Version 13 enables guest jump (bit 6) and adds sneak toggle (bit 10), routed
through the original authoritative player controller. Version-12 peers are
rejected; Input remains 110 bytes and checkpoints remain version 56.

Version 12 adds buy/sell/close-trade intents in the existing 110-byte Input, a
trade-open story flag and the companion inventory in story projections. Version-11
peers are rejected; checkpoints remain version 56.
Version 11 adds both players’ combat accuracy and learning counters to the story
archive. Version-10 peers are rejected; checkpoints remain version 56.
Version 10 appends dialogue intent to `Input` and a control lease to the story
projection. Version-9 peers are rejected; checkpoints remain version 56.
Version 9 appends an optional host dialogue/story projection to `Poses`. Version-8
peers are rejected. Checkpoints remain version 56; the projection is transient.
Version 8 adds audited consumable-use intent, ordered inventory removals and
personal talent/counter/held-item state. Version-7 peers are rejected; checkpoints
remain version 56 because original actor and inventory records already persist
these effects and pending animations.
Version 7 enables melee input bits and appends player progression and bounded
NPC combat updates to `Poses`. Version-6 peers are rejected. Checkpoints remain
version 56: individual actor records and the original reward guards already persist.
Version 6 added desired weapon equipment to `Input` and both actors’ weapon slots
to `Poses`. Version-5 peers are rejected; checkpoint version remains 56.
Version 5 added explicit pickup targets to `Input` and ordered pickup transfers
at the end of `Poses`. Both layouts change; version-4 peers are rejected.
Version 4 added character poses/animation layers and pose acknowledgements.
Version 3 added snapshot begin/chunk/end/readiness messages. Older versions are
rejected explicitly. Version 2 introduced `Accepted`, separating compatibility
from companion assignment. Other message layouts are unchanged apart from the required version field. The exact executable fingerprint also prevents
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
layout, padding, native endianness and pointers are never used for wire fields.
Snapshot and story-archive bodies explicitly use the compatible engine checkpoint
serialization format.
Fingerprints are raw bytes, not hexadecimal text.

Every frame begins with this 44-byte header. Offsets are zero-based:

| Offset | Bytes | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 4 | Magic | Literal ASCII `OGCP` (`4f 47 43 50`) |
| 4 | 4 | Frame length | Total bytes, including this header |
| 8 | 2 | Version | Exactly `13` |
| 10 | 2 | Message type | Defined below; unknown types are fatal |
| 12 | 8 | Generation | Host-issued connection/world generation; zero before acceptance |
| 20 | 8 | Player | Persistent actor ID assigned by the host; zero before actor binding |
| 28 | 8 | Input sequence | Nonzero only in `Input` |
| 36 | 8 | State revision | Nonzero in `Welcome`, `StateAck`, snapshot and pose messages |

The frame length must be between 44 and 65,536 bytes inclusive. Fixed messages
have exact lengths. `SnapshotChunk` permits only its eight-byte offset and
1–32,768 data bytes (53–32,820 bytes total). The decoder validates the prefix
after eight bytes and the complete header before buffering the body. Padding
and undeclared extensions are forbidden except for the explicitly zero-padded
animation-name/unused slots specified below.

## Message definitions

| Type | Name | Direction | Total bytes | Generation / player / sequence / revision |
| --- | --- | --- | --- | --- |
| 1 | `Hello` | Guest → host | 108 | `0 / 0 / 0 / 0` |
| 2 | `Welcome` | Host → guest | 44 | `nonzero / nonzero / 0 / nonzero` |
| 3 | `Input` | Guest → host | 110 | `nonzero / nonzero / nonzero / 0` |
| 4 | `StateAck` | Host → guest | 52 | `nonzero / nonzero / 0 / nonzero` |
| 5 | `Reject` | Host → guest | 46 | `0 / 0 / 0 / 0` |
| 6 | `Accepted` | Host → guest | 108 | `nonzero / 0 / 0 / 0` |
| 7 | `SnapshotBegin` | Host → guest | 84 | `nonzero / 2 / 0 / nonzero` |
| 8 | `SnapshotChunk` | Host → guest | 53–32,820 | `nonzero / 2 / 0 / nonzero` |
| 9 | `SnapshotEnd` | Host → guest | 44 | `nonzero / 2 / 0 / nonzero` |
| 10 | `SnapshotReady` | Guest → host | 44 | `nonzero / 2 / 0 / nonzero` |
| 11 | `Poses` | Host → guest | 2,154–61,258 | `nonzero / 2 / 0 / nonzero` |
| 12 | `PoseAck` | Guest → host | 44 | `nonzero / 2 / 0 / nonzero` |

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
pose revisions start at 2 after snapshot readiness. The guest requires the accepted generation,
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
| 52 | 8 | Pickup target world namespace, or zero |
| 60 | 8 | Pickup target typed item ID, or zero |
| 68 | 8 | Inventory instance + 1, or zero |
| 76 | 1 | Inventory action: 0 none, 1 equip, 2 unequip, 3 consume once |
| 77–109 | 33 | Dialogue intent; see [G09 fields](#guest-initiated-host-story-dialogue-g09) |

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
| 8 | `0x100` | Melee action held (normal Ctrl/action mapping) |
| 9 | `0x200` | Draw/sheath melee on rising edge (Space or melee key) |
| 10 | `0x400` | Toggle sneak on rising edge; requires the actor's learned sneak skill |

All other bits must be zero. Both look deltas must be in `[-1000, 1000]`.
The button mask replaces the previously held state; zero releases every button.
Look deltas are applied once per accepted sequence. This is input intent, not
position, elapsed simulation time, damage or a script command. The host determines
simulation timing and gameplay effects. N09 enables bits 0–5 through the guest's
normal key mappings (including alternate keys). Version 13 enables Jump and
Sneak; the host preserves normal collision, jump animation and learned-skill
checks. Sneak toggles only on a new press, never on release or continued hold.
Walk remains reserved and is rejected by the session as `unsupported_input`. G01 permits item pickup;
G03 permits melee/ranged weapon equipment. G06 enables bits 8–9 through the
original controller, including action+forward attacks and directional melee
controls. Action without a drawn melee weapon retains the G01 pickup behavior;
the host also masks the attack bit when its actor has no drawn melee weapon.
The host computes focus, range, collision, animation hit windows and damage.
The guest never supplies a hit, damage amount, reward or target for an attack.
Ranged/spell controls and other interactions remain later work. Walk is a toggle in the original controller,
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
Disconnect/save acceptance is covered by the N11–N13 scenarios.

The two pickup fields must both be zero (no request), or both nonzero. Pressing
the guest's normal generic-action key (Ctrl by default, also Ctrl+Up) sends the
currently focused item once. Holding it suppresses walking and does not repeat
pickup requests. The host resolves the supplied typed identity in its current
world, recomputes normal focus (range, angle, line of sight), and checks normal
player busy/weapon/body-state conditions. Wrong-world, removed, wrong-kind,
unknown or out-of-focus targets have no effect. No reward, class, amount or
recipient is supplied by the guest. The bound companion is always the requester.
Pickup requests use the same generation, sequence, duplicate rejection and save
barrier as movement. An input acknowledgement means processed, not successful;
the corresponding authoritative transfer below is the success result.

### StateAck

| Offset | Bytes | Field |
| --- | --- | --- |
| 44 | 8 | Highest consecutively applied input sequence, or zero before any input |

N13 installs input only inside an actual host simulation/controller step; paused
frames keep received input queued. It queues this acknowledgement immediately before
the corresponding `Poses` frame at the same revision. Acknowledged inputs have
been processed by companion control; future movement while a button remains
held still belongs to future host ticks. The pose frame repeats the highest
applied sequence. The guest rejects acknowledgements for another generation/player,
sequences it never sent, or decreasing sequence/revision. This acknowledges no
disk save and contains no transforms. The independent `PoseAck` below confirms
that guest presentation reached a particular pose endpoint.

### Poses (N10)

All floats are finite IEEE-754 binary32, transmitted as big-endian bit patterns.
This frame contains a 24-byte prefix, two 729-byte actor records in player order
1, 2, two 16-byte weapon-equipment records, a u16 pickup count and zero to
sixteen 56-byte pickup records, two 66-byte combat/progression records, a u16 NPC
count and zero to 32 NPC combat records of 795 bytes each, followed by two
228-byte personal records (host, companion), a u32 story length and optional story
archive. The fixed fields contain no native structs, pointers or array indexes. The header's player 2 identifies the recipient binding, not an actor
index within the payload.

| Payload offset | Bytes | Field |
| --- | --- | --- |
| 0 | 8 | Persistent world-object namespace, nonzero |
| 8 | 8 | Authoritative simulation tick (milliseconds) |
| 16 | 8 | Highest applied guest input sequence |
| 24 | 729 | Host actor record |
| 753 | 729 | Companion actor record |
| 1482 | 16 | Host weapon equipment: u64 melee, u64 ranged |
| 1498 | 16 | Companion weapon equipment: u64 melee, u64 ranged |
| 1514 | 2 | Pickup transfer count, 0–16 |
| 1516 | count × 56 | Ordered pickup transfers |
| 1516 + pickup count × 56 | 132 | Host then companion combat/progression records |
| 1648 + pickup count × 56 | 2 | NPC combat count, 0–32 |
| 1650 + pickup count × 56 | NPC count × 795 | NPC updates |
| 1650 + pickup count × 56 + NPC count × 795 | 456 | Host then companion personal records |
| 2106 + pickup count × 56 + NPC count × 795 | 4 | Story archive byte count, 0–32,768 |
| 2110 + pickup count × 56 + NPC count × 795 | variable | Optional story archive |

Each actor record is:

| Actor offset | Bytes | Field |
| --- | --- | --- |
| 0 | 8 | Persistent player role (1 then 2) |
| 8 | 8 | Persistent typed NPC object ID, nonzero and distinct |
| 16 | 12 | Position x/y/z, float32 each |
| 28 | 12 | Yaw, pitch, running tilt, float32 degrees each |
| 40 | 8 | Head rotation x/y, float32 each |
| 48 | 1 | Active layer count, 0–8 |
| 49 | 680 | Eight fixed 85-byte animation slots |

Each active slot holds a 64-byte printable-ASCII sequence name, zero-padded
(unless all 64 bytes are used), u64 animation-start tick, u64 blend elapsed
milliseconds, u32 body-state flags, and u8 combination index. Unused slots must
be entirely zero. Names are nonempty, start tick cannot exceed the frame tick,
body flags must fit 17 bits and combination must be 0–9. The engine additionally
resolves names in the loaded compatible skeleton/overlays, checks combination
bounds and rejects duplicate animation layers. Position components are bounded
to ±10,000,000 game units; yaw ±1,000,000°, pitch ±90°, tilt/head rotation ±360°.
NaN, infinity, invalid counts, padding or identities are rejected by the codec.

After readiness, pose revisions start at 2 and increase by exactly one. Host
capture occurs after its simulation and controller step, at most once per render
frame, and is skipped when the tick and applied-input boundary are unchanged
and no inventory, equipment, progression, personal or NPC updates are pending.
Guest frames must match generation/binding, arrive in revision order, carry
nondecreasing host ticks/applied-input sequences and never refer to unsent input.
There is a separate 64-frame application queue; overflow closes the connection.
The guest resolves both typed NPC IDs in the received world namespace and validates
both animations, the complete pickup batch, both weapon-equipment records and
all combat/personal states, held-item references, NPC identities, references and
animations before applying anything. Known sequences may share an animation-layer
number: original item-ending transitions can legitimately overlap the standing
pose. Preserve their received order; do not impose layer-number uniqueness.
Sequence lookup, sample data, combination bounds and wire bounds still apply. Invalid
world/object/animation/transfer state closes the connection without partially
applying the frame.

### Combat and progression (G06)

Each 66-byte combat record contains, in order:

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 32 | Eight signed i32 attributes in script order: HP, max HP, mana, max mana, strength, dexterity, regeneration fields |
| 32 | 16 | Signed i32 XP, next threshold, level and LP |
| 48 | 16 | Typed NPC IDs: current target, last attacker; zero means none |
| 64 | 1 | Weapon state, 0–6 in original engine order |
| 65 | 1 | Outcome: 0 standing, 1 unconscious, 2 dead |

Integers use network byte order; signed values use two's complement. Maximum
HP/mana and progression must be nonnegative. The engine resolves every nonzero
target/attacker against the loaded snapshot. A combat NPC record contains a
729-byte actor pose with player ID **0**, followed by this combat record. NPC
IDs must be nonzero, unique in the batch, distinct from both players and resolve
to existing non-player NPCs. The same animation checks apply as for players.

After whole-frame validation, the guest installs attributes/progression, target
references, weapon mode and death/unconsciousness flags directly. It never calls
damage, death, reward, AI or campaign functions. Player poses retain their 100-ms
interpolation; NPC poses currently apply directly. Updates are absolute state,
not additive rewards, and existing strict revision checks reject replayed frames.

The host compares NPC state with the snapshot/last sent state. Nearby NPC poses
(within 3,000 units of either player) and any changed combat state are eligible.
A rotating scan emits at most 32 changed NPCs per frame; unsent changes remain
pending, including while the simulation is held. Reconnect discards this cache
and establishes a fresh snapshot baseline. This covers NPCs in the snapshot;
live NPC spawning/removal, remote corpse inventory and projectiles remain outside
this first combat slice. Unknown new NPC identities fail closed and require a
fresh snapshot rather than constructing an independent NPC on the guest.

For campaign hosts with a saved companion, an engine hook fans out the installed
`B_GivePlayerXP` call to both roster members, including an offline companion.
The original function executes for the host and then with `HERO` temporarily
bound to the companion, using each actor's existing XP/threshold/LP/attributes.
The companion pass suppresses the function's final `B_CheckLog` call and duplicate
host-side print/sound effects. Other death/quest/item effects execute once in the
host context. `HERO` is restored on scope exit; `PC_HERO` stays bound to the host.
Defeating a roster player does not produce a shared XP reward.

Monster eligibility and deduplication remain in the original defeat scripts:
the installed `ZS_Dead` checks the victim's persistent `aivar[16]` before its
eligible XP call and marks it rewarded afterward. One host script execution
awards both actors without an intervening input/save boundary. The actor records
and that guard survive save/restart; rewinding restores both to the checkpoint.
No second death script, cloned loot, new reward RPC or guest reward execution is
introduced. `shared_xp_events` is a process-local diagnostic counter, not the
durable reward guard. The no-network G05 baseline retains original single-hero XP.
Quest reward delivery is still gated by G07–G09 acceptance; the XP hook alone
does not establish quest/dialogue replication or per-player script compatibility.

The `combat-state` codec case checks exact layout, every split/truncation, maximum
batch, invalid outcomes/references by type, duplicate IDs and nonfinite poses.
The parent `shared-combat` scenario uses real host/guest attacks, both finishers,
concurrent attacks, duplicate/stale input, original level-up, an offline companion,
aggressive AI against either player, disk restarts and atomic malformed-frame
rejection. Concrete run results are recorded in the parent `TESTING.md`.

### Shared death and checkpoint recovery (D01)

With co-op enabled, `GameSession` latches failure when either roster actor enters
the original **dead** state. Original unconsciousness is distinct and does not
end the session. A host completes the current world step, observes both actors
(to cover simultaneous damage), and freezes before another controller/script
step or queued world transition. The simulation clock and animation updates stop.
Both controllers and held input are cleared, active inventory/dialogue UI closes,
and both windows show the failure message. The host's Escape menu remains available
to choose a checkpoint or start a new game if no checkpoint exists. A transport
disconnect alone does not set or clear this failure.

No new RPC or wire field is required. The existing validated `Poses.progression`
records carry `down = 2` for death and `down = 1` for unconsciousness. The guest
latches failure only after installing a valid authoritative frame or host
snapshot, never from local HP arithmetic or a client death claim. A connecting
or reconnecting guest can receive a frozen failed-world snapshot; that does not
revive either actor or resume the host. Guest campaign execution remains disabled.
Protocol/checkpoint versions stay **8/56**, with exact executable compatibility
still enforced by the handshake.

Late inputs on the failed connection are sequence-validated then discarded before
controller, pickup, equipment or consumable dispatch. They are not acknowledged
as applied. The last legitimate pre-failure input boundary remains intact; no
unbounded deferred gameplay queue survives waiting at the failure screen. Normal
queue limits still apply to a peer sending an excessive burst. A load destroys
old transport queues and changes the generation, so delayed pre-death commands
cannot affect the restored world.

User disk-save requests are refused with `denied_failure`, including a queued
request that reaches its write boundary after a fatal step. No pending writer is
started and the last committed checkpoint is preserved. A save already writing
holds simulation, so death cannot advance concurrently with that save. In-memory
snapshots remain available to show the failed world to reconnecting peers.
Failure is a latch on the current session object, not transport state or a new
checkpoint field. Loading a healthy checkpoint constructs a fresh session and
restores the original actors, inventories, progression and world; a checkpoint
containing an already-dead player is detected again. New-game recovery also
constructs a fresh session. Offline play without a co-op role keeps its original
behavior. Revive/incapacitation gameplay remains the later D02 task.

The parent `death-reload` scenario uses real monster attacks, ordinary fist
unconsciousness and simultaneous physics landing damage. Fixtures set initial
positions/HP only, never expected death/failure outcomes. It also checks rejected
saves, rollback of an actual potion use and kill reward, delayed input, failed
reconnects and recovery without a checkpoint. See parent `TESTING.md` for evidence.

### Consumable use and personal effects (G06b)

Input action **3** requests one use of the encoded inventory instance. It shares
ordered, generation-bound input sequencing with movement, pickups and equipment;
it contains no result, amount, effect or requested actor. A nonzero inventory
intent cannot also request a world pickup. The host resolves the bound companion,
requires an owned, nonempty, audited item and its original use conditions, and
rejects downed, airborne, sliding, interacting, armed or cutscene-locked actors.
Host dialogue/video also blocks the request. Acceptance invokes the original
`Inventory::use` and unchanged Daedalus callback with `SELF` scoped to the consumer.
`HERO` remains the host. Guest inventory actions never execute scripts.

The supported original Gothic II items/callbacks are:

| Instances | Callback(s) | Original personal effect |
| --- | --- | --- |
| `ITFO_BREAD`, `ITFO_CHEESE`, `ITFO_FISH`, `ITFOMUTTON` | `USE_BREAD`, `USE_CHEESE`, `USE_FISH`, `USE_MEAT` | +10/+15/+5/+12 HP, clamped |
| `ITPO_HEALTH_01`, `_02`, `_03`, `ITPO_HEALTH_ADDON_04` | `USEITPO_HEALTH_01` through `_04` | +50/+70/+100 HP or full HP |
| `ITPO_MANA_01`, `_02`, `_03`, `ITPO_MANA_ADDON_04` | `USEITPO_MANA_01` through `_04` | +50/+75/+100 mana or full mana |
| `ITPO_PERM_STR`, `ITPO_PERM_DEX` | Matching `USEITPO_PERM_*` | +3 attribute and corresponding personal learning counter; dexterity >=90 can unlock acrobatics |
| `ITPO_PERM_HEALTH`, `ITPO_PERM_MANA` | Matching `USEITPO_PERM_*` | +20 max/current HP or +5 max/current mana; mana learning counter +5 |

Both instance and callback names must match; mission/equipped items are excluded.
This list audits the installed original scripts, not arbitrary mods that replace
callback bodies under the same names. Apples use the global `APPLE_BONUS`, so
apples, quest items and other unaudited consumables remain blocked for the guest
until the personal scripting audit. Host offline item-use behavior is unchanged.
Original script prints/sounds still originate on the host; this slice replicates
attributes, talents, counters, animation and hand props, not script UI messages.

**Original timing is preserved.** The script applies its effect immediately;
original item animation tags remove the unit later. These can be different pose
revisions. G06b does not make them an atomic gameplay transaction, refund an
interrupted effect, add a new removal timer, or fix original interruption exploits.
A fresh Use press follows the original animation rules. Duplicate network sequences
never call Use again. Disconnect releases controls but does not replay an accepted
use. Save/reload restores the original pending item/animation state; the existing
save boundary flushes accepted inventory state before its acknowledgement.

Inventory transfers retain the existing 56-byte layout and shared ordering.
**World item ID zero now denotes a removal from the named player's inventory**:
`amount > 0`, `amount <= before`, `after = before - amount`. No world tombstone
is created. Nonzero IDs retain pickup semantics below. Repeated zero IDs are
allowed; only nonzero world IDs must be unique. The host records actual removal
from the inventory path, including the original animation tag, without advancing
its timing. The guest validates every pre-count against the projected result of
preceding transfers, requires audited item types for removals and validates both
actors' equipment and hand props before any mutation. It then adjusts existing
items without executing constructors/use callbacks. Invalid later records reject
the entire frame. The decoder releases variable transfer/NPC bytes when a frame
is dequeued, so ongoing item traffic cannot exhaust a phantom queue budget.

Each 228-byte personal record contains:

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 88 | 22 signed i32 talent skills, each 0–6 |
| 88 | 88 | 22 signed i32 talent values, each 0–100 |
| 176 | 12 | Nonnegative i32 `REAL_STRENGTH`, `REAL_DEXTERITY`, `REAL_MANA_MAX` counters |
| 188 | 8 | Held consumable instance + 1, or zero |
| 196 | 32 | Printable ASCII bone name, zero padded |

A zero held instance requires an empty slot; a nonzero instance must resolve to
an audited item remaining in the projected inventory and slot `ZS_RIGHTHAND` or
`ZS_LEFTHAND`. Nonzero bytes after NUL padding are rejected. Counter names resolve
to validated `C_NPC.aivar` indexes in the compatible scripts (81/82/83 in the
installed game). Direct talent installation updates engine overlays without
executing scripts. Guest hand props come from these records; guest animation tags
remain disabled. Pending snapshot props are reconciled without deleting items.
All personal fields are absolute state; pose revisions prevent repeated effects.

`protocol-consumables` covers action encoding, mixed pickup/removal order, count
bounds, personal fields, padding, fragmented frames and repeated queue draining.
Parent scenarios `consumable-baseline`, `consumables` and `consumable-save` exercise
the original item scripts and real host/guest inventory controls. See the parent
`TESTING.md` for commands, offline comparisons, restarts and evidence.

### Pickup transfers (G01)

Each transfer contains seven u64 fields in order:

| Record offset | Field |
| --- | --- |
| 0 | Recipient player role, 1 or 2 |
| 8 | Recipient typed NPC object ID, matching that frame's actor |
| 16 | Removed typed world-item ID; zero selects G06b inventory removal |
| 24 | Item instance/class index in the fingerprint-matched game scripts |
| 32 | Transferred stack count, positive |
| 40 | Recipient's previous count of that instance |
| 48 | Recipient's resulting count, previous + transferred |

Instance and inventory counts must fit u32 without overflow. The record's world
is the enclosing pose namespace. Duplicate nonzero item IDs within a batch are invalid.
Before mutating anything the guest validates all live typed references, item
class/count/unequipped state and inventory preconditions; sequential transfers
of the same class use the preceding transfer's resulting count. A valid batch
moves the snapshot's existing item instances into their recipients' inventories,
using the normal container merge and world-item tombstone operations. It never
runs item constructors, pickup animations, scripts, theft perceptions, use or
equipment effects locally. Normal removal also clears stale VM/AI references to
the removed world item without executing a script. Host animation arrives through the pose layers.
Discrete transfers apply once on receipt; transforms retain their interpolation.
The connection generation, exact pose revision and record index identify the
transaction. Old/duplicate/out-of-order revisions cannot reapply a transfer.

Host pickup recording is active only with a ready guest. More than 16 transfers
between frames, outbound queue failure or a guest state mismatch closes the
connection; a fresh snapshot is the recovery path. Pending transfers are transient
and are not saved or replayed into a new generation. Saves contain the resulting
inventories and tombstones, using the existing checkpoint barrier. Checkpoint
version remains 56. This task supports picking up existing snapshot items;
item spawning/dropping, consumable use, other equipment, NPC inventory changes
and other campaign outcomes require their own replication. Guest Tab opens its
own inventory; G03 adds weapon equip/unequip requests.

Snapshots and new saves include optional `coop/focus` schema 1: a little-endian
u32 version followed by Normal, Melee, Ranged and Magic targeting definitions.
Each has 19 four-byte fields in `zenkit::IFocus` declaration order (16 float32
ranges/angles and three i32 priorities), totaling 308 bytes. Length, schema and
finite floats are validated. This lets the guest select targets without script
constructors. Older host checkpoints without the entry remain readable and
regenerate definitions normally; their fresh network snapshot includes it.

### Competing pickup arbitration (G02)

The first valid pickup processed by the authoritative game thread wins. Its
normal world removal tombstones the item immediately, before the next contender
can resolve it. A loser gets no transfer or inventory addition. There is no
reservation, rollback or client-time fairness policy. Currently queued guest
inputs are processed before the host's local controller in a simulation step;
a guest request arriving after a host pickup loses. Both orderings are tested.

The local co-op controller now retains the typed item/world selected on the
initial generic-action key press. The next interaction step consumes that exact
intent whether it succeeds or fails, and clears it on release/input reset.
Without this, the host could target the next nearby item after the guest took
the original. The losing action must not become a pickup of that second item,
or a different interaction. Ordinary single-player targeting is unchanged.
The guest already sends an explicit typed target through the G01 input stream.
A new deliberate action can select another remaining item normally.

Retransmitting an accepted input sequence is discarded before gameplay; an old
generation cannot claim a live item. A new sequence naming the removed ID also
fails, even when a different live item now occupies its old runtime-array slot.
A duplicate successful pose/transfer revision fails the guest's ordering check
without applying the transaction twice. Recovery uses a fresh host snapshot.
Saving the result persists the winner's inventory, the loser unchanged, and the
single tombstone; restart/rejoin never replays the old pending intent. No wire
or checkpoint format changes are needed: versions remain **5/56**.

Parent runner `competing-pickups` uses real generic-action keys and a transparent
TCP frame relay. Two plants start within both actors' normal focus range. The
test holds simulation to queue both contenders, advances one ordinary engine
step, exercises each winner, then checks one transfer, one addition and one
removal. It verifies array compaction leaves the second item live, duplicate and
stale request rejection, fresh sequences with a removed/wrong-world/NPC target,
replayed result rejection, and save/restart/fresh join for both outcomes. A later
explicit pickup proves the remaining item still works, including a 1→2 stack
merge. `fixture_shared_plants` only creates world items; `step_simulation` only
advances the ordinary controller/simulation path from a held test runtime.

### Weapon equipment (G03)

Inventory Enter/generic action or a normal inventory click selects an owned
weapon instance and sends its desired equipped state. Equipment and pickup
intents are mutually exclusive within an `Input`; movement fields retain their
usual meaning. The equipment item/action fields are both zero for no intent.
Otherwise action is 1 or 2 and the item field is the u32 script instance index
plus one, in 1–4,294,967,296. An inventory instance belongs to the bound companion's
inventory in the current snapshot generation; it is not a world-item object ID.
There is no guest-supplied owner, slot, reward, forced-equip flag or stat value.
For stacked identical weapons, the selected inventory row determines the intent:
the equipped row requests unequip; the spare row requests equip and therefore
keeps the already equipped copy in place without reapplying its effects.

The host resolves the item in that inventory, requires an ordinary melee/ranged
weapon (excluding shields), checks the actor's body/interaction/cutscene state
and requires a sheathed weapon. It applies normal `Npc::useItem`/`unequipItem`
with forced equip disabled; original attribute/rune checks and equipment script
effects run only there. Already having the requested state is an idempotent
success. Missing inventory items, non-weapons and unmet conditions leave the
slots unchanged. Input ordering, generation/duplicate checks and the save barrier
are the same as movement and pickups. An input acknowledgement reports processing;
the authoritative slot state reports the result.

Each pose frame carries complete melee/ranged slots for both actors in role order.
Each slot is instance + 1, or zero for empty; a nonzero instance cannot occupy both
slots. These are discrete state fields and are not interpolated. The host also
emits an update if equipment changes while the simulation tick is held, as when
inventory actions occur between simulation steps. Repeated unchanged state does
not rebuild meshes or reapply effects.

Before applying any pickup or equipment change, the guest validates both actors'
projected inventories, including items transferred by pickups in this same frame.
All nonempty slots must refer to owned positive-count items of the correct weapon
category. It then applies pickups, updates the weapon-slot pointers/equipped flags
and numeric slots (1 melee, 2 ranged), and rebuilds the affected weapon meshes and
bone attachments. No guest item-use/equip scripts, conditions, bonuses, damage or
stat effects execute. An invalid second actor also prevents the first actor and
any otherwise valid pickup from changing. Invalid state closes the connection;
fresh snapshot loading is the recovery path.

G03 covers sheathed melee/ranged equipment, replacement and unequipping, visible
attachments, inventory flags and persistence. Armor, jewelry, shields, runes,
consumable use, drawing/holstering controls and arbitrary campaign/script-effect
replication remain later work. Equipment scripts/stat changes are authoritative
on the host and are included in its saved/freshly joined snapshot. Existing save
format 56 already stores inventory slots/flags and restores their visual meshes;
no new save schema or client checkpoint is introduced.

`weapon-equipment` in the asset-free codec suite checks literal bytes, every
fragmentation boundary, invalid/mixed intents, duplicate slots and bounds. The
parent GUI scenario of the same name uses ordinary inventory navigation and
Enter to equip, replace and unequip both players' melee/ranged weapons, rejects
unowned/non-weapon/too-demanding items and repeated requests, checks actual mesh
presence and separate equipped/spare stack rows, and saves/restarts/rejoins both
equipped and empty-slot checkpoints.
Fake-host tests combine a valid pickup and valid first actor with invalid second
actor equipment; no inventory, object, pose or mesh may partially apply.

### PoseAck and presentation

`PoseAck` has no body. The header identifies the generation, binding and revision
whose two displayed transforms reached their exact authoritative endpoints.
The host accepts only nondecreasing acknowledgements for revisions it sent,
strictly after the snapshot baseline. Acknowledgements may skip intermediate
revisions while movement is being interpolated. `poses_received` alone is not
proof of a rendered endpoint.

The guest interpolates positions, pitch and running tilt over 100 ms toward the
latest validated target; yaw takes the shortest arc across ±180°. It advances
the previous blend before retargeting from the displayed transforms. Identical
transform updates keep the existing blend deadline, so stopping reaches the
exact endpoint. A displacement over 1,000 units or a changed generation/world/
object identity snaps instead. No extrapolation or movement prediction runs.
The animation layer descriptors and head rotation come from the host; the guest
samples bone animation on its presentation clock synchronized to received host
ticks. Campaign time and all animation gameplay effects stay host-only.
Each window's existing local camera follows its own actor (host 1, guest 2).

The two characters' movement/animation and their successful world-item pickups
and weapon equipment are replicated. G06 also replicates combat/progression and
existing NPC combat poses/outcomes; AI remains host-only. Ranged/spell controls,
other equipment/use, dialogue, quests and other world changes require later tasks.
See [guest authority](coop-guest.md) for the campaign authority boundary.
`Poses` does not imply a full-world revision or a disk commit.

### Reject

| Offset | Bytes | Field |
| --- | --- | --- |
| 44 | 2 | Rejection reason |

Reasons: `1` protocol version, `2` engine fingerprint, `3` game-data fingerprint,
`4` host full, `5` invalid session/handshake. Any other value is malformed. No
arbitrary error string crosses the wire. A reject is a terminal handshake reply,
followed by closing the connection. On an undecodable/unsupported peer version,
closing without a reply is also valid; a peer might not understand a version-8
reject. Runtime connection failure and stale input are handled by the session
layer, not by sending an unbound reject in the middle of gameplay.

## Session and ordering rules for the connection layer

The codec validates structural constraints. The session enforces compatibility,
direction/phase checks, admission, companion assignment, snapshot readiness and
input sequence tracking and advancing pose revisions.

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
   N10 advances pose revisions after readiness. Repeated acknowledgements may
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
no live socket binding is restored from a save. N13's historical generation is
provenance only. Rejoining resolves that actor
and receives a fresh generation.

N11 grants movement control only while the current generation has companion ID 2
bound and its snapshot is ready. Losing any of these clears held actions, pending
mouse impulses, installed-input tracking and guest interpolation before the next
simulation step. If a save/load worker owns the world, release waits for game-thread
ownership; simulation cannot run in between. Socket destruction discards its queued
input, poses and acknowledgements. The host's own controller is not cleared by
companion disconnect. Ordinary stop animation/floor settling may finish afterward;
the disconnected actor retains its identity, HP and inventory and is still saved.
The host listener and campaign remain active. EOF/read/write errors trigger release;
detecting a live but unresponsive peer without a socket error is not yet implemented.
Wire version 4 and checkpoint version 56 are unchanged.

The `disconnect-held` integration scenario kills the guest during forward/turn
input while the host also moves, then verifies idle companion state and an offline
checkpoint after full host restart. The asset-free `input-disconnect` case leaves
unapplied input queued at socket closure and requires its removal.

### Rejoining the same companion (N12)

A guest with a disconnected, failed or rejected connection can press **R** to
retry the original `-coop-join` endpoint. The window displays this action after
the loader releases ownership. The reconnect action is unavailable on a healthy
or in-progress connection, on the host, and while loading. A fresh guest process
can also join normally.
There is no automatic retry loop or endpoint change. Executable/data fingerprints
remain the startup values; changing either still requires process restart.

Retry destroys the old transport, decoder, pending input/pose/ack queues, held
buttons and mouse deltas, interpolation target and snapshot metadata. Any old
world stays read-only while connecting and transferring. A fresh checksum-verified
snapshot replaces it through the guest-authority loader, then companion ID 2 is
selected before readiness is acknowledged. Input remains disabled until then.
The host resolves its existing companion; it does not spawn/reset another actor.
Its current world, including changes while the guest was absent, becomes the
snapshot. Each accepted join gets a new nonzero host generation; input restarts
at sequence 1, snapshot revision 1, and pose revision 2.

Old-generation input is ignored/counted without consuming a new input sequence.
Old-generation pose, input-ack or snapshot messages fail the new guest session
before application. Queues belonging to a closed connection cannot be drained
by a new connection, and old load completion cannot acknowledge a new generation.
This is session consistency, not authentication: the current prototype accepts
one compatible local guest and always assigns the existing companion to it.

`rejoin-companion` kills a moving guest, moves the host while absent, then compares
a fresh snapshot's identities, inventory/health and poses. A test-owned TCP relay
inserts old input and poses into the real sockets; old input has no effect and an
old pose disconnects before either actor changes. The same guest process then
uses ordinary R input to rejoin again and resume movement. Asset-free
`rejoin-queues` / `rejoin-stale` cover undrained host input/guest poses, unsent
bytes, a changed snapshot and obsolete acknowledgements across connection reuse.
Coordinated loading is described below (N14). Wire/checkpoint versions stay 4/56.

### Checkpoint input boundary (N13)

Only the host requests a disk checkpoint. The request becomes pending, then is
captured after the next eligible controller step and its pose/input acknowledgement.
If simulation is paused, unprocessed input stays queued and lies outside the cut.
The cut includes the last input sequence processed by that controller step, not
all bytes received by TCP. While the worker owns the world, the host continues
receiving into the existing bounded FIFO, but cannot drain or acknowledge later
input. Success or failure releases that FIFO in order on the next simulation step.
Duplicates still consume no input sequence/effect; overflow still disconnects.
Losing a connection during the write discards its queue as usual.

The same world returns from a save; its held controls/camera are retained rather
than rebuilt. This lets held movement resume without a second keydown. A held
button's future movement is outside the checkpoint; loading clears transient
controls. Snapshot transfer and world replacement use their separate barriers.

Disk archives add optional `coop/checkpoint`, schema 1, exactly 44 bytes. The
existing checkpoint encoding uses little-endian integers on supported macOS:

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | Schema version, 1 |
| 4 | 8 | World object namespace |
| 12 | 8 | Authoritative world tick at capture |
| 20 | 8 | Connection generation, or 0 while offline |
| 28 | 8 | Included pose revision, or 0 while offline |
| 36 | 8 | Last input included in that generation, or 0 while offline |

The world namespace/tick identifies the authoritative capture boundary; the pose
revision is scoped to the recorded live generation. Offline captures have no
active transport revision/sequence, but still contain all already-applied world
effects. Legacy archives without this optional entry load normally. Present
records must have the exact layout/schema and agree with the loaded world/tick;
an offline record cannot claim input or a pose revision. The loaded record grants
no authority and never seeds new input sequence counters. Memory snapshots omit
disk-checkpoint provenance and describe their own fresh connection instead.

The writer finalizes and flushes the temporary archive, fsyncs it and replaces the
prior checkpoint before the host reports `save_status = committed`. Failure reports
`failed`, preserves the prior checkpoint and returns the same live world. Ordinary
`StateAck`/`PoseAck` remain simulation/presentation acknowledgements; they do not
announce disk success, and no new wire message is introduced. Test observations
expose `save_boundary`, `loaded_checkpoint` and `network.input_held`.

`save-boundary` uses a bounded test-only writer delay (0–2,000 ms) to inject later
look input and a duplicate during I/O. It independently decodes the archive cut,
checks exactly one calibrated turn after success/failure, verifies unchanged prior
bytes on failure, resumes held movement and reloads connected/offline checkpoints.

### Loading starts a fresh lifetime (N14)

A host load closes the existing peer and discards pending transport input, poses
and acknowledgements **before** discarding the old world. It releases the input
hold, clears both controllers' actions/focus, mouse deltas, guest interpolation,
snapshot metadata and transient inventory/dialogue references. The listener stays
available. Loads during an active save/load are ignored; a queued save blocks a
load request (`denied_busy`). No load completion can report a save commit.

The checkpoint restores the persistent player roster and typed object IDs. Its
optional boundary record is historical provenance, never a socket binding or a
sequence counter. Both actors start without held controls. Old transient
conversation/trade UI ownership is not restored. Invalid checkpoint metadata
fails loading before gameplay; there is no fallback to the old connected world.

The old guest remains read-only after disconnection. Press R to rejoin; the host
issues a new nonzero generation and transfers a fresh snapshot of the restored
world. Input begins at 1 only after readiness. Rejoin may connect while loading,
but the host cannot bind/capture until it owns a ready world. Old input is ignored
without consuming a new sequence; old poses/acks/snapshot completion cannot apply
or acknowledge the new lifetime. The prototype uses explicit R rejoin, rather
than an automatic load/reconnect loop or an in-place transport reset message.
Wire/checkpoint versions remain 4/56.

`load-generation` saves distinguishable companion HP/inventory, then changes them
and moves both players. It loads the earlier checkpoint with input held, checks
immediate revocation and restored idle state before sending key-up, rejoins, and
injects genuinely delayed old input/poses. After rejecting these, both controllers
can move again. A second load closes an open host inventory and requires another
fresh snapshot. The disk checkpoint must remain byte-identical throughout.

### Restart and rejoin from disk (N15)

The parent tools repository's `restart-rejoin` scenario combines saving,
process shutdown, disk loading and fresh snapshot transfer. Run it with the
installed game assets and the native engine build:

```sh
python3 tools/run-gothic-tests.py --scenario restart-rejoin
```

The host and companion start with different HP and inventory counts, prepared
through host-side world fixtures and normal pickup controls. Both windows move
their own actors, then the host commits a connected checkpoint. Both processes
exit before a new host loads slot 99 and a fresh guest joins without a local
save. A second cycle kills the guest while moving, verifies control release,
collects one more companion item on the host and saves while disconnected.
Both processes are replaced again before testing that offline checkpoint.

Each cycle checks the persisted player roles, typed NPC identities and world
namespace; exact inventory symbols/counts/equipped flags and HP; position within
five game units and heading within 0.2 degrees of the committed checkpoint.
Once held at the same acknowledged pose revision, the two views must agree
within 0.01 units/degrees and have identical animation descriptors. It also
checks fresh process sessions and connection generations, cleared input counters,
the host snapshot's independent SHA-256 against both reported digests, local
camera/HUD ownership, guest campaign isolation and movement through both windows
after each rejoin. Loading/joining must leave checkpoint bytes unchanged.
Checkpoint copies, process logs, state comparisons and screenshots from both
windows are retained by the runner; see the parent project's `TESTING.md` for
recorded acceptance and regression evidence.

This completes the two-window persistence milestone. The tested scope is
loopback movement/poses plus host-owned saves and fresh snapshots. Live pickup,
equipment, combat, dialogue/quest effects and joint region changes remain
separate gameplay work; preparing inventories on the host does not establish
guest pickup replication. N15 changes no wire or checkpoint layout (4/56).

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
reset with the generation. N10 adds `poses_sent`, `poses_received`, `poses_applied`,
`poses_acknowledged`, `pose_tick`, `interpolation_ms` and `interpolation_alpha`.
`poses_applied` is the last exact displayed endpoint, while `pose_tick` describes
the most recent sent/received target. `snapshot_state == "ready"` reports snapshot
activation (guest) or receipt of that acknowledgement (host).
N12 adds `can_rejoin`, which matches availability of the ordinary R-key action.

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

This 110-byte `Input` uses generation `0x0102030405060708`, player `2`, sequence
`9`, revision `0`, held `Forward | Walk`, look X `-5` and look Y `100`:

```text
4f474350 0000006e 000d 0003
0102030405060708 0000000000000002
0000000000000009 0000000000000000
00000081 fffb 0064
0000000000000000 0000000000000000
0000000000000000 00
00 0000000000000000 0000000000000000
0000000000000000 0000000000000000
```

The test suite checks this literal against both encoding and decoding. It also
checks an independent `Hello` vector, every split point and byte-at-a-time input,
combined frames, invalid headers/payloads, all truncated prefixes, bounded queues,
blocked/partial writes, 10,000 ordered input messages under changing read/write
boundaries and 10,000 deterministic mutated/truncated frames.
On macOS, eighteen additional test cases exercise endpoint parsing, real loopback
acceptance and reconnect, both directions of fingerprint validation, version
rejection, second-guest rejection, deadlines, pending-socket limits, and the
actual file/manifest hash implementation using temporary files. Input tests cover
readiness, delivery/acknowledgement, duplicate and old-generation packets, gaps,
wrong identities, reserved actions, application queue bounds and impossible acks.
Pose cases cover typed fields, exact floats, a literal prefix, readiness, sequence
order, time/input boundaries, applied-revision acknowledgements and queue limits.
Interpolation tests cover midpoints, retarget continuity, shortest-arc turns,
stationary completion, backward clocks, large jumps and generation/world resets.
Binding tests cover host-only assignment, idempotence, reconnect/load invalidation, second-guest
isolation, incorrect IDs/generations/revisions, early/duplicate Welcome frames,
and forged guest assignments/inputs. The engine integration test additionally
checks live and saved NPC reuse, separate inventory/HP/poses and script identity.

## Build and test without game files

Only a C++20 compiler and CMake are required; the commands below use Ninja.
This standalone project does not configure OpenGothic's dependencies, read Steam
assets, start a GUI, or require initialized engine submodules. It runs 16 codec
cases plus four object-registry and two interpolation cases everywhere, and
eighteen connection/fingerprint cases on macOS (40 total); the latter use
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

## G01 validation

`pickup-transfers` in the asset-free suite covers literal target bytes, every
fragmentation point, transfer roundtrips, invalid counts, target pairs, actor
identities, arithmetic overflow and duplicate IDs. G01 added the 38th macOS case;
G03 adds a 39th for weapon equipment; G06 adds the 40th for combat state. Parent runner `guest-pickup` exercises normal keyboard pickup, inventory
inspection, committed save/restart/fresh rejoin and whole-frame rejection of
wrong-kind items, inconsistent counts and an invalid second transfer after a
valid first record. `guest-isolation` verifies only the intended pickup inventory,
world item/tombstone and pose entries change; scripts/quests/damage stay host-only.
See parent `TESTING.md` for run artifacts and regression results.


## Host-controlled dialogue and story projection (G08)

For host-initiated conversations the host owns the choices and runs the unchanged
original conditions/actions with canonical host `HERO`/`OTHER`. The guest cannot
select or skip them. G09 adds guest initiation below; it preserves this story actor
and the existing sequence, generation and direction validation.

`Poses` ends with a big-endian u32 archive length and that many bytes. A zero
length means unchanged story state. A nonempty value is a compatible engine
`Serialize` ZIP with exactly one entry, `story`, limited to 32 KiB compressed and
1 MiB expanded. Its contents use the existing checkpoint serialization format,
not the outer wire integer format. The exact executable/data handshake is required,
as with the initial snapshot. This is not a portable or hostile-host archive API.
The whole frame, including a full NPC batch, remains below the 64 KiB cap.

The entry contains, in order:

- In version 10: four u64 values for control owner (0/1/2), conversation ID,
  menu/subtitle revision and staging actor object ID. These use `Serialize` format.
- In version 12: a serialized boolean indicating an open trade UI.
- Dialogue active/choices-visible/speaker-is-host flags; host and NPC object IDs;
  selected row; current subtitle; choice count and pairs of title/function symbol.
- In version 11, host then companion each supply five signed i32 hit chances
  and four signed i32 real combat-learning counters (1H, 2H, bow, crossbow).
  These 72 bytes use `Serialize` encoding; accepted values are 0–1,000,000.
  Replica installation writes actor fields directly without script callbacks.
- Existing quest-log, known-info and guild-attitude records (`saveQuests`).
- Inventory actor count, followed by object ID and existing inventory records for
  the host, the companion on trade events (version 12), and the current/most recent
  dialogue NPC. Previously installed NPC
  outcomes remain in the replica; the update does not accumulate past NPC inventories.

Guest installation resolves all identities in the current world and uses saved
item allocation, never item constructors, dialogue conditions/actions, equip/use
callbacks or reward helpers. Companion inventory is excluded from this projection.
Both players' XP/LP/level/attributes continue through the existing progression
records; `B_GIVEPLAYERXP` shares the original positive XP with the companion once,
while the host alone updates the campaign journal. Unique handovers are executed
once against the host inventory. Nearby NPC combat/animation changes use the
existing NPC pose records. Global Daedalus variables remain authoritative on the
host; guest journal/history are display replicas, not a second quest simulation.

The host compares uncompressed contents to avoid timestamp-dependent updates,
sends changed projections in the same strictly ordered pose stream, and clears
its comparison cache on a new connection. Duplicate, stale or wrong-direction
pose frames fail session validation before installation. A projection cannot run
a reward script even when received repeatedly. Initial/rejoin snapshots and disk
checkpoints carry canonical durable state; transient menu ownership is not saved.

Saving during a conversation or cutscene queues the request with `pending` status.
It stays pending until both end; only then is the checkpoint boundary captured
and the normal atomic writer started. Death cancels the request with
`denied_failure`, and world replacement cancels it under the existing load fence.
No mid-dialogue request is silently acknowledged as completed.

Acceptance covers Bosper's bow (150 XP each, one bow consumed, history prevents a
second turn-in even with a spare) and Lares's original chapter-3 ornament recovery
(one `ITMI_ORNAMENT_ADDON_VATRAS` to the host). Fixtures arrange NPCs/items/chapter
prerequisites offline; normal dialogue performs the outcomes. This does not claim
a full campaign audit: live NPC creation/removal, level transitions, trade/trainer
replication and personal faction/story state remain outside this tested slice.
Dialogue voice playback and transient reward popups are not mirrored in G08;
the guest receives subtitle text, choices and durable reward state.
A projection exceeding its limits terminates replication explicitly rather than
silently truncating the journal or inventories.


## Guest-initiated host-story dialogue (G09)

Both the mapped keyboard action and mapped mouse button produce the same open
intent below. Mouse clicks never execute the guest's local gameplay controller.
Held keyboard and mouse actions are merged before producing the button mask;
releasing one binding preserves another held binding of that action. UI clicks
must not fall through into gameplay intents. The guest journal is a local view
of the replicated shared quest log and does not send a new protocol message.

`Input` appends these fields after the equipment action at offset 76. All use the
outer big-endian format and the same contiguous input sequence as movement.

| Offset | Bytes | Field |
| --- | --- | --- |
| 77 | 1 | Dialogue action: 0 none, 1 open, 2 choose, 3 skip, 4 previous, 5 next, 6 buy, 7 sell, 8 close trade |
| 78 | 8 | World namespace |
| 86 | 8 | Open: NPC object ID; choose: function symbol index + 1; trade: packed quantity/item (below); otherwise zero |
| 94 | 8 | Conversation ID (zero for open) |
| 102 | 8 | Menu/subtitle revision (zero for open) |

Action zero requires all dialogue fields zero. Other actions require a world;
open requires a nonzero target and zero conversation/revision; subsequent actions
require nonzero conversation/revision. Choose accepts a function index through
UINT32_MAX, encoded plus one. Dialogue cannot share a frame with movement, look,
pickup or equipment/consumption. Invalid encodings close the stream; valid but
unauthorized requests are processed without effects. An acknowledgement is not
proof that a conversation opened or an action succeeded.

The host recomputes companion focus, including range, angle and line of sight,
and resolves the typed NPC identity. Only one conversation may exist. The first
eligible interaction owns its controls until exit, disconnect or world load;
the other player's selection/skip keys and both players' movement are suppressed.
A new open increments the conversation ID; menu/subtitle transitions increment
its revision. A choice must match both and a currently offered function. Sequence
validation rejects replayed packets; the lease also rejects fresh packets carrying
an obsolete menu or conversation. No client supplies a reward or script actor.

The companion stages the camera and NPC facing. The canonical host stays in place
and remains HERO/OTHER for original conditions, known-info pairs, globals, journal,
guild checks, inventory and rewards. The guest NPC output pipe is opened without
running the perception-driven walk-to-host start routine. No script files change.
The guest resolves focus from replicated NPCs without ticking their AI or VM.
XP uses the existing shared reward path. Other tested item effects use the host.
Ownership is transient; a load invalidates the peer generation and closes it.
Disconnect clears guest dialogue output queues/barriers before releasing the lock.
A pending save still waits for the conversation's safe boundary.

This first stage explicitly allows Bosper's bow quest, its Bartok conversations,
Lares's previously audited greeting/ornament callbacks and Garond's exit. Other
NPCs cannot be opened by the guest. Within these menus, unreviewed actions and
trade are denied before `GameScript::exec`, including before known-info recording;
the guest sees `[Host only]` on those choices. Unreviewed automatic important infos
are skipped. Training, recruitment, general campaign route coverage and personal
story state are not enabled. Expand this policy only after tracing the complete
callback/helper/world effects in G09a, including deferred chapter flags.

The real `DIA_GAROND_SUCCESS_INFO` is a denied deferred chapter turn-in. The test
arranges chapter 2, three mine reports and a running Evidence quest offline. A
guest selection changes no story state or rewards. A host selection grants the
paladin letter to the host, 500 XP to each, completes the quest and sets
`MIS_READYFORCHAPTER3`; KAPITEL remains 2 until the later world-entry route. This
is not a test of a chapter-3 map transition. Disk restart/rejoin preserves the
completed action and does not offer it again. Guest voice/popup replication and
scripted walk/cutscene staging beyond these audited paths remain limitations.


## Independent learning with shared access (P01)

The conversation owner is also the learner for the audited training entry,
lesson and Back callbacks. Ordinary info conditions and known-info history still
use the host story actor; SELF remains the trainer. Within each training action,
OTHER and HERO are scoped to the learner and restored on return. No Daedalus files
are modified and no guest VM executes scripts. Existing input sequence and
conversation/menu revision checks fence duplicate and stale purchases.

Enabled routes are Carl's strength +1/+5 and Bartok's bow +1/+5 and sneak lesson.
Access unlocks and Carl's original 50-gold access fee remain host story effects.
These lessons have no per-purchase gold fee. LP, cap checks, current stats, cost
counters, learned sneak and combat skill categories belong to the learner.
Bartok's permanent sneak condition is specifically audited: its shared access flag
is combined with the learner's unlearned-sneak check, so one player learning does
not hide the other's option. Other conditions remain host-bound.

The original menu-building and purchase callbacks run in the same learner context,
so displayed costs agree with deductions. Bartok's before-training bow scratch is
held in the conversation and installed/restored around each action. It is not a
personal story global and does not persist as an active transaction. Host saves
remain deferred until the dialogue ends. Existing actor archives persist all
learned outcomes; no checkpoint schema change is required.

The guest participant uses the dialogue output pipe; output waits/skips and owner
loss include that participant. Scope changes do not retarget queued output: the
queue retains the actual NPC handles resolved when the original script calls it.
The latest synchronous training PrintScreen result/refusal is carried in the
existing projected text field when no spoken subtitle is active, alongside the
choice menu. It clears on the next action or conversation close; this adds no
Input fields or general reward-popup replication.

A learning update changes the story archive comparison, transmitting the nine
combat-learning fields per actor together with the menu state. Attribute/LP/talent
and attribute-learning counters retain their existing progression/personal paths.
Trading is enabled for the audited P02 route below. Other trainers and crafting globals
are not enabled by this limited route policy.


## Personal vendor transactions (P02)

The audited guest route is Bosper's `DIA_BOSPER_TRADE_INFO`. Its condition,
known-info state and original stock callback execute with canonical host story
bindings. The open trade belongs to the conversation owner, and both views show
that actor's inventory plus the same vendor stock. Only the owner can operate
or close it. Ordinary host vendor routes continue using their original scripts.
No guest VM runs stock, item-constructor, payment or dialogue callbacks.

Actions 6 (buy) and 7 (sell) use the existing target u64 at offset 86:
upper 32 bits are requested quantity (1–10,000); lower 32 bits are item instance
index + 1 (nonzero). Action 8 closes the trade and requires target zero.
All require the current world, conversation ID and displayed transaction revision.
There are no client-supplied prices or balances. Wire validation rejects invalid
quantities/encodings, mixed movement/use commands and missing leases. The host
requires guest conversation ownership and an open trade, resolves its recorded
vendor and player, then validates the source item, visible stock, available count,
nonnegative original price and bounded arithmetic. Gold cannot itself be traded.
Source-count validation uses the combined equipped/spare stack, since UI rows
can split one instance. Guest sales must leave any equipped unit in place; spare
units can be sold, but scripted equipment removal is not enabled through this route. An equipped vendor item is never purchasable.

Existing `buyItem`/`sellItem` perform the transfer. Buying costs the original item
value; a bulk request exceeding funds buys only the affordable count, while zero
affordability transfers nothing. Selling pays `ceil(value * TRADE_VALUE_MULTIPLIER)`;
the installed NotR multiplier is 0.15. Preserve original unlimited vendor funds:
sales create the player's proceeds and purchases remove payment, without moving
gold into/out of the vendor purse. Requests above available source quantity are
rejected before either items or money change. Every valid-lease attempt advances
the transaction revision, including refused attempts and closing. Replayed input
sequences and fresh inputs with an old revision cannot purchase/sell again.

On trade opening/transaction attempts the existing ordered story archive carries
both players' full inventories and the current/last dialogue NPC inventory.
The companion replacement is queued through closing until projected once; ordinary
pickups/consumables retain their existing incremental paths without replacing the
companion inventory or invalidating unrelated item references. Stock and balances are projected
together, after the existing pickup/equipment/personal validation. This archive
is still tied to the exact executable/data handshake and its existing size caps;
it is not an untrusted arbitrary-save API. No inventory scripts run on the guest.
Inventory cursors remain local; identity/quantity intents refer to items, not rows.

The dialogue lease remains active throughout trade. Save requests wait for exit;
disconnect closes a guest-owned shop and clears dialogue/output state. World load
cancels the lease; fresh session generations reject prior traffic. Original NPC
and actor archives persist stock, gold, items and chapter stock flags, so no save
schema migration is needed. Bosper's original per-visit arrow/bolt refresh and
chapter-based stock/junk cleanup remain shared host effects. Persistence tests
compare saved inventories before reopening, then test another real transaction.
