# Persistent world object identities (N06)

The registry in [`objectregistry.h`](../common/game/objectregistry.h) gives
world NPCs and world items identities independent of pointers, script classes,
array positions and connection order. `WorldObjects` owns it and registers all
normal and dynamic world-item insertions, player insertions, and NPC spawns.
It is engine code shared with the asset-free tests.

## Identity and lifetime

A world has a nonzero random 64-bit namespace, saved with its registry. Every
object receives a nonzero 64-bit serial from a monotonically increasing counter.
The pair `(world namespace, serial)` identifies an object within that saved
world history. A fresh world gets a new namespace; restoring a checkpoint
restores its namespace and serials. Serial zero means no world binding.

Object kinds are `1 = NPC` and `2 = world item`. Typed lookup requires both the
correct namespace and kind. Unknown, removed, wrong-world and wrong-kind
references cannot resolve. Registration of the same live pointer/kind is
idempotent. Changing its kind is an error. Memory-address reuse after removal
allocates a new serial.

Player roster IDs remain `1 = campaign host` and `2 = companion`. They describe
player roles, not object serials. Each roster entry points to an NPC that also
has an ordinary NPC object identity. Connection authority remains assigned by
the host using the player roster; an object reference grants no authority.

When an object leaves its world collection, its registry record becomes an
explicit tombstone: same serial and kind, no live object. Tombstones remain in
checkpoints and are never returned to the allocator. Removing an item can compact
the runtime array without changing another item's identity. Inserting an object
into a vacated array slot allocates a fresh serial.

Normal pickup retires the **world instance** identity before transferring the
item to inventory. Inventory stacks can merge and do not have world identities;
they are still represented by owner, symbol, count and equipment state. Dropping
or otherwise creating another world item allocates a new identity. Stack identity
and inventory transaction replication are separate future work.

All active and inactive NPCs are registered, including both players. Sorting
NPC arrays or moving an NPC between active and inactive storage preserves its
identity. Explicit removal/extraction, or retirement from inactive storage to
the engine's removed-NPC list, creates a tombstone. Death alone does not remove
a still-present corpse. This establishes the identity lifecycle for later NPC
replication; it does not replicate AI, animation, damage or spawn events yet.

Object namespaces belong to individual regions. A carried hero entering a new
region has that region's NPC identity while retaining its player roster role.
Co-op region transitions remain blocked until their later coordination task.

Every future network object reference must also be fenced by the accepted
connection/world generation. Loading an earlier checkpoint rewinds the entire
registry, including allocations that happened after that checkpoint. Such an
alternate history can issue the same subsequent serials. N05 closes connections
on world replacement; packets from the abandoned history must never resolve in
the new generation. No process-global allocator file or normal-play save is
modified to maintain identities across those separate histories.

## Checkpoint format

The engine save version is now **56**. Ordinary NPC/item records and the version-1
`coop/players` roster are unchanged. Every version-56 world requires:

```text
worlds/<world name>/coop/objects
```

This archive entry follows the engine serializer's native integer representation
(little-endian on the tested arm64 Mac), not the network byte order. Fields are
written individually with no struct padding:

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | Registry format version, exactly 1 |
| 4 | 8 | Nonzero world namespace |
| 12 | 4 | Record count, including tombstones |
| 16 | 14 × count | Records below |

Each record is `{serial: u64, kind: u8, location: u8, index: u32}`. Locations:

| Location | Meaning | Required kind/index |
| --- | --- | --- |
| 0 | Tombstone | Either known kind; index `0xffffffff` |
| 1 | `npc/` checkpoint record | NPC; valid active NPC index |
| 2 | `items` checkpoint record | Item; valid world item index |
| 3 | `npc_invalid/` checkpoint record | NPC; valid inactive NPC index |

Indexes only locate records **inside this checkpoint**. They are resolved to
newly loaded objects and discarded as identifiers. They must never appear in
network object references. The writer obtains them from the same frozen world
collections that it serializes; no simulation runs during the existing save path.

Records contain every issued serial from 1 through `count`, including removed
objects. Thus the next serial is `count + 1` without a separately editable
watermark. Records are written in serial order; loading permits any order but
requires unique, contiguous serials and unique live-object bindings. Every
active/inactive NPC and world item must be covered exactly once. The registry
rejects unknown kinds/locations, zero namespace, invalid locators, duplicate IDs,
multiple IDs naming one object, missing live objects, unsupported format
versions, and truncated or trailing data. The maximum is 1,000,000 records per
world including removal history; exhaustion fails instead of recycling IDs.
Tombstone pruning is not implemented.

World versions through 55 without this entry are legacy saves. Loading one
creates a fresh namespace and assigns IDs once; saving it writes version 56 and
persists the mapping. A missing/empty entry in a version-56 world is corruption,
not permission to silently recreate identities. If an older world does contain
the entry, it is validated normally. Independently importing the same unsaved
legacy checkpoint creates independent namespaces. Guests will receive the
host's mapping with its snapshot, rather than derive IDs from their own arrays.

The binary wire protocol remains version 2: N06 adds no network message. N08
must define snapshot serialization/transfer of these mappings; later removal
messages will name the generation, namespace, serial and kind. These identities
do not themselves transmit state or announce a removal to another process.

## Validation

The standalone suite tests typed/world-scoped lookup, idempotence, address reuse,
new-pointer restoration, allocator persistence after deleting the highest serial,
malformed tables with transactional rejection, size limits and 10,000 removals.
Both Release and ASan/UBSan run the same engine registry source.

The parent runner's `object-identities` scenario tests two identical plants,
real array compaction, wrong-world/wrong-kind/removed lookups, normal companion
pickup, explicit tombstones, reuse of the old array slot, and three full
save/process-restart cycles. It checks both players' persistent NPC identities
and independently parses the archive's mapping. `object-corruption` damages ten
variants of the archive entry, requires failure before gameplay, then checks a
valid restore and migration of the preceding version-55 layout. Commands and
recorded artifact paths are in the parent repository's `TESTING.md`.
