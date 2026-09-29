# Dialogue, quest and trainer baseline (G07)

This is a single-player baseline using unchanged installed Gothic II scripts.
It establishes the outcomes that G08/G09 and P01 must preserve. It does not enable
guest dialogue, personal quest variables, independent factions or trainer use.
Protocol/checkpoint versions remain 8/56.

## Bosper's stolen bow

The isolated old-world scene contains original instances `VLK_413_BOSPER` and
`VLK_440_BARTOK`. Their constructors, routines, perceptions and dialogue are used
unchanged. The fixture positions the player near each NPC and places the original
quest item on nearby ground. All conversations, menu choices and pickups use
ordinary controls. Escape skips spoken lines through the existing UI.

The original prerequisite path is:

1. Bosper's greeting, `DIA_BOSPER_SEEKWORK`, then `DIA_BOSPER_BARTOK`.
2. Bartok's greeting and `DIA_BARTOK_BOSPER`. This sets `MIS_BOSPER_BOGEN` to
   `LOG_RUNNING` unless it is already successful. The fixture does not set it.
3. Return to Bosper and choose `DIA_BOSPER_BOGENRUNNING`. Its condition requires
   the running flag; its action creates the "Bosper's Bow" journal topic/entry.
4. Pick up `ITRW_BOW_L_03_MIS`, then choose `DIA_BOSPER_BOGENSUCCESS`.

The handover condition checks that `OTHER` has at least one quest bow. Its action
transfers one bow from `OTHER` to `SELF` through `B_GiveInvItems`, sets the quest
global successful, awards `XP_BOSPER_BOGEN` (150) through `B_GivePlayerXP`, then
removes that bow from Bosper's inventory. `B_CheckLog` and
`B_CloseTopics_Mattes`/`B_CloseTopic` update the journal's status. The fixture
does not write completion, reward, journal or known-info records.

The info is non-permanent. `GameScript::exec` records the pair
`(PC_HERO, DIA_BOSPER_BOGENSUCCESS)` before executing the action. The test picks
up a second quest bow after completion, then reopens the conversation before
and after a complete process restart. This verifies that history hides the
handover even when the inventory condition would pass. XP stays at 150 and the
spare bow stays with the player. Global, journal, history, inventory and personal
progression state must match the saved values after restart.

## Bartok's archery lesson

A separate fresh runtime uses only `VLK_440_BARTOK`. His greeting followed by
`DIA_BARTOK_WANNALEARN` sets `BARTOK_TEACHPLAYER`, which enables the training
conversation. `DIA_BARTOK_TEACH_INFO` records current bow accuracy in the global
`BOSPER_MERKEBOW` and builds the normal +1/+5/back choice menu.

The +1 choice calls `B_TeachFightTalentPercent(SELF, OTHER, NPC_TALENT_BOW, 1, 60)`.
At the tested starting accuracy of 10%, the displayed and charged cost is 1 LP;
there is no gold/item charge. A zero-LP attempt must leave accuracy and its
learning counter unchanged. With a fixture starting budget of 3 LP, the same
ordinary choice must leave 2 LP, raise bow accuracy to 11%, and increment
`aivar[REAL_TALENT_BOW]` once. The test does not assign the learned result.

Gothic II combat percentages live in `C_NPC.HITCHANCE`, distinct from the generic
talent-value array. The test observes both, plus the four real combat-learning
counters and talent skills. Save/restart preserves the purchase; opening the
trainer again must not repeat it. P01 will later test independent players and
additional costs/routes after personal scripting support. `BOSPER_MERKEBOW` is a
concrete shared script variable to consider in that audit.

## Observations

The development control action `watch_dialogue` accepts up to 64 integer symbol
names. A nonempty list enables read-only state observations of:

- The existing menu's choices, function/info names, selected row, readiness,
  current subtitle and participating persistent NPC IDs. No condition is
  reevaluated by the observer.
- Known-info pairs, watched integer arrays and the quest log.
- The latest 256 actual condition/action calls, recording `HERO`, `SELF` and
  `OTHER` object IDs while the original scope is active. This bounded trace is
  diagnostic and is not serialized. Enable observation again in a fresh session.
- NPC inventories and player combat percentages/counters needed for the outcome
  and restart assertions. Actor air/busy state and controller focus make ordinary
  interaction timing observable.

In these baseline calls, `HERO` and `OTHER` refer to the player and `SELF` to the
talked-to NPC. Menu input executes actions via the existing `DialogMenu::onEntry`
and `GameScript::exec` paths. The test also checks that reading state in a held
simulation changes neither script-call count nor persistent state.

`fixture_dialogue_npc` permits only Bosper and Bartok, rejects existing copies,
and requires held offline gameplay with no active conversation. The quest-bow
fixture has the same offline/held restriction. Neither fixture executes dialogue
actions. These development controls require `-test-control`; normal play does
not collect the scope trace.

## Chapter authorization candidates for G09

Read-only inspection identifies **Garond's ore-report turn-in**,
`DIA_GAROND_SUCCESS`, as a useful test of deferred chapter advancement:

- Its condition requires chapter 2, `ORE_COUNTER >= 3`, and `MIS_SCOUTMINE`
  running. Knowledge of the non-permanent info is a separate engine condition.
- Its action creates/transfers `ITWR_PALADINLETTER_MIS`, sets
  `KNOWSPALADINS_ORE`, adds the Evidence log entry, completes `MIS_SCOUTMINE`,
  awards `XP_SCOUTMINE`, and sets `MIS_READYFORCHAPTER3`. It also calls
  `B_Npc_IsAliveCheck(OLDWORLD_ZEN)`; its old-world chapter-2 branch has no
  corresponding NPC removal branch, but later-chapter branches do.
- On return to Khorinis, `INIT_NEWWORLD` checks the readiness flag and
  `B_CHAPTER3_ONETIME`, calls `B_Kapitelwechsel(3, NEWWORLD_ZEN)`, and latches
  the one-time flag. That helper changes `KAPITEL`, checks the journal, runs
  `B_Enter_NewWorld` and updates ambient XP. Chapter-3 world initialization has
  NPC creation, killing and routine changes. A guest must therefore be denied
  before the entire turn-in action, not just before assignment to `KAPITEL`.

For a direct chapter-changing dialogue, **Lord Hagen's pass conversation**,
`DIA_LORD_HAGEN_PASS_INFO`, calls `B_Kapitelwechsel(2, NEWWORLD_ZEN)` inside the
action. Its condition checks chapter < 3 and `HAGEN_BRINGPROOF`. Surrounding
effects include starting `MIS_OLDWORLD`, issuing `ITKE_PASS_MIS`, creating the
Evidence topic, a conditional Fernando routine change and an NPC insertion.
The chapter helper also changes NPC flags and invokes world initialization.

G09 should compare the full pre/post turn-in effects, including known-info
history, items, XP, quest/global flags and world changes, for a denied guest
attempt followed by a successful host attempt. Checking only the immediate
chapter number would miss Garond's deferred transition. These are inspected
candidates, not a claim that chapter transitions have been playtested in G07.
The broader route/global/world-effect audit remains G09a.

The parent `TESTING.md` records commands, original-script fingerprint, inspection
artifacts, native build, acceptance and regression results. The inspected-symbol
manifest is `../tools/gothic-dialogue-symbols.txt` in the parent repository.
