# Guest authority (N07–G01)

The campaign is simulated and saved by the host. `-coop-join 127.0.0.1:PORT`
selects guest mode for the process, including before a world exists and after
connection refusal, timeout or disconnect. The guest does not become a host if
its connection fails. G01 uses wire version 5 and checkpoint version 56.

## Loading

A guest launched without `-save` waits with no `GameSession`, world, hero or
new-game startup. Accepting a connection and receiving `Welcome` does not create
these objects. The window displays “Waiting for the host world”.
It honors the configured frame cap, including the test runner's 30-FPS setting.

The checkpoint constructor accepts `GameSession::Authority::Guest`. Its immutable
authority member is initialized before the script VM and world are constructed;
loading must not depend on `Gothic::world()` because the new session is owned by
the background loader at that point. NPC/item records are allocated and restored
from the checkpoint without running their instance constructors. Campaign
definition constructors (spells, focus, currency, guild values and dialogue
instances), SVM constructors and DirectMemory initialization are skipped. The guest restores read-only focus definitions from `coop/focus`; other skipped
definitions retain empty defaults.
The separate visual/resource VMs still load rendering, camera and sound data.

Saved variables, quests, perceptions, NPCs, inventories, world items, triggers
and identity tables are restored. Startup/world initialization and a legacy
perception fallback cannot execute campaign functions. N08 constructs its
received snapshot with **Guest** authority explicitly; the default checkpoint
constructor remains authoritative for ordinary single-player/host loads.

The normal join path receives a checksum-verified host checkpoint over TCP,
loads it directly from memory, selects companion ID 2, and acknowledges readiness.
The host holds simulation during this barrier. Incomplete transfers never reach
the loader, and a load finishing after cancellation is discarded before display.
See [snapshot protocol](coop-protocol.md#initial-snapshot-and-readiness-barrier-n08).
`-save 99 -coop-join ...` may still load a local development fixture with guest
authority, but only the received host snapshot can acknowledge a network revision.

## Simulation and presentation

`GameSession::tick`, `World::tick` and NPC simulation/routine/event processing
return without changing a guest campaign. Campaign time stays at the snapshot
tick. Guest bone animation advances on a separate presentation clock initialized
to that same tick. `MdlVisual::updateAnimation` updates the animation solver,
bone pose and mesh attachments, while skipping sound/particle animation tags and
effect ticks. Gameplay animation events and root-motion integration remain in
the authoritative NPC tick.

The main window keeps rendering and camera mouse/zoom handling active. It skips
dialogue and player simulation. G01 permits read-only inventory navigation,
with item-use, drop and equipment actions blocked. Explicit nonlocal
controllers, Marvin commands and mutating UI shortcuts are blocked. N09 routes
normal movement/turning keys and mouse intent to the assigned host controller
after snapshot readiness; guest input never drives local gameplay. N10 receives
both players' typed poses and animation layers, validates them together, and
interpolates displayed transforms over 100 ms without predicting movement.
The pose importer changes position/orientation and animation descriptors only.
It does not call normal animation-start functions, gameplay tags, root-motion
integration or the VM. The presentation clock follows received host ticks;
the campaign clock stays at the snapshot tick. The guest acknowledges a pose
revision only when both displayed transforms reach its exact endpoints.

Additional authority checks protect damage/attribute changes, pickup/use,
world item/NPC creation and removal, queued/immediate/startup triggers, region
changes and quest-log mutations. Campaign calls go through
`GameScript::runFunction`; bound script externals also check authority. The raw
VM is retained for checkpoint allocation/restoration, not runtime execution.
New execution paths must use the guarded entry point. This is an engine
ownership boundary, not a security sandbox for arbitrary native code or mods.

Normal guest saves return `denied_guest`; the session serializer also refuses
them. A guest cannot overwrite a host checkpoint or start a fresh campaign.
Host saving and gameplay retain their existing paths.

After connection loss the guest remains read-only. Press **R** when the reconnect
prompt appears to retry the original host without restarting the window. The
old view can remain visible during transfer; controls return only after loading
and acknowledging a fresh host snapshot. Held keys/mouse impulses, pose blending
and old transport queues are discarded. The existing companion and inventory
come from the current host world. See [rejoin protocol](coop-protocol.md#rejoining-the-same-companion-n12).

## Acceptance test

From the parent tools repository:

```sh
python3 tools/run-gothic-tests.py --scenario guest-isolation
```

The test connects an empty guest, verifies that requesting a new game still
leaves it empty, prepares two host actors, a plant, distinct companion HP and
quest/global markers, holds host simulation, then transfers the host snapshot
into the guest. A local checkpoint is retained only to detect unauthorized saves. It compares saved quest/perception data, every mutable
script variable and every resolvable instance reference. Existing loader
normalization of unresolved pointers and ZenKit's synthetic argument scratch
string is handled explicitly; no actual script globals are omitted.

Before and after guest movement, attack/pickup, inventory, explicit host-player
input, damage, quest/script, item removal/creation, trigger, new-game, console
and save attempts, it hashes **every World::save archive entry except NPC
`/visual` entries**, plus the VM variables, quest log and perceptions. These
visual entries contain advancing bone animation state. The test also requires
the simulation tick and campaign function-call count to remain fixed, while
rendered frame count, presentation time, head-bone poses and camera angles
change. The same damage, pickup, quest, script and trigger calls must work on the
host. Disconnect must leave the guest equally read-only. Diagnostic archives
are not loadable guest checkpoints.

This verifies the fixture and the engine's current runtime entry points. Full
campaign event replication, dialogue ownership and joint region
travel remain later tasks. Screenshots, detailed
states, per-entry hashes, logs and cleanup evidence are retained by the runner;
the parent `TESTING.md` records completed runs.

## Replication acceptance (N10)

`python3 tools/run-gothic-tests.py --scenario pose-replication` holds ordinary
keys in both windows concurrently, observes movement and changed animation
layers in both replicas, and exercises opposite turns. It then holds host
simulation and requires the same acknowledged pose revision, typed identities,
HP/inventories and animation descriptors. Settled tolerances are 0.01 game units
and 0.01 degrees; camera targets must follow their own actors within 2 units.
Both viewpoints and moving captures are retained. All other nonvisual campaign
archive entries remain byte-for-byte unchanged; only the two player data records
are excluded because they contain replicated transforms. Campaign tick/function
counts stay fixed. A fixture host then sends unknown world/object/animation
references after a real snapshot; the guest must reject them before changing
either actor. This supplements the N07 effect-attempt and save-denial regression.

## World-item pickup (G01)

The guest's generic-action key sends the focused typed world-item identity to
the host. Host normal targeting and pickup checks decide the outcome. Successful
host or companion pickups are journaled into ordered pose frames. The guest
validates the whole frame and all transfer preconditions before moving the
existing snapshot item into the specified inventory and tombstoning its world
identity. This explicit replica path calls no campaign scripts or pickup effects.
The normal mutating guest APIs remain blocked. Tab shows companion inventory;
use/equip/drop is unavailable. Save/restart/rejoin gets the result from the host's
checkpoint. See [wire format and validation](coop-protocol.md#pickup-transfers-g01).
