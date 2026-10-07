# Owned quest reactions

Implemented for negotiated protocol v2 flat clients only. VR remains build/package
covered, without quest-reaction activation or copied flat offsets.

## Capture and admission

Reuse the bounded quest collector at most once per five-second background interval;
bootstrap delays the first refresh. No deep player inventory or nearby-reference scan
is added. Lock contention reports unavailable.

Compare fresh complete/partial journals with the last acknowledged journal for the
same generation, playthrough and canonical player. Initial load establishes a baseline.
React to explicit false-to-true tracking, a newly tracked quest absent from a complete
known-tracking journal, or changed complete displayed objectives on a tracked quest.
Stage-only changes, ordering, unknown tracking, cached scans, untracking alone and
quests without displayed objectives cannot manufacture a reaction.

Choose an already activated NPC: current non-narrator conversation owner, crosshair,
then nearest eligible actor. Require visible line of sight, alive, normal/sitting,
noncombat, nonhostile, not talking to the player, hearing range and no conversation
cooldown. Activation evidence is bounded to 128 canonical actors per client session.

Queue one response through the existing cancellable worker lane. A newer acknowledged
journal or player turn supersedes it. Context publication and trigger are serialized.
Do not automatically activate an actor or start follow-up chatter for a quest comment.

## Server trust boundary

The quest_updated trigger carries its actor and previous_context_sequence. Require
dialogue.quest_reactions plus context.quest_tracking, v2 flat, and a positive previous
sequence smaller than the exact bound current sequence. Client-authored instruction,
text and visual capture are forbidden. Init returns init_quest_reactions_accepted;
older acknowledgements do not enable reactions. This marker also acknowledges the
requested quest tracking/events support.

The server reads immutable snapshots from the same active session, generation,
playthrough and player. The previous snapshot must be the latest earlier fresh journal.
It independently verifies the target is active, present and eligible, recomputes the
change, and allows only the earliest admitted quest trigger per current snapshot.
Suppression returns quest_reaction_suppressed without invoking a model.

Existing NPC RPG enabled-event list, chance and atomic owner-isolated 60-second
cooldown still govern generation. Detection is not a promise of speech.
No setting, schema migration, provider or prompt override is introduced.

## Verification and manual acceptance

Both native DLL builds and all 17 suites pass. Native checks cover journal semantics,
negotiated/older-server behavior, malformed reinitialization and invalid binding.
Mirrored contracts: 26 schemas, 98 fixture cases, 114 canonical files. Existing PHP
suite: 194 tests, 2630 assertions. Disposable SQL: 25 binding/ownership/eligibility/
duplicate checks, plus journal history 33, native quest events 27 and PHP handshake 11.
No test called a paid provider or game.

Manual flat acceptance remains open: load a save, activate a nearby NPC, establish
a fresh journal baseline, then change a tracked quest/objective while the NPC is
visible and idle. Inspect the owned quest_updated request and its RPG policy result.
Repeat with combat, scene dialogue, save/load during a pending response, no eligible
NPC and unchanged journals; verify responsiveness and no stale/duplicate speech.
The user runs the game. Independent VR headset acceptance remains separate.
