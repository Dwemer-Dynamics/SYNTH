# Fallout 4 flat semantic-event binding audit

## Flat actor host activation (2026-09-06)

The host now installs the previously audited observers at kPostLoad, advertises
availability only while the exact slots remain installed, and arms only after
the correlated server acknowledgement and a fresh stable owner capture. Immediate
F4SE invalidation also changes the epoch during a reentrant arm capture; the host
checks that stamp before arming, before binding and across session retirement.
World-not-ready and halt paths invalidate capture. Normal Pip-Boy menus retain
shallow capture so equipment callbacks can bind promptly; menu pause defers HTTP.

The new actor_events capture purpose excludes nearby discovery, quest refresh,
picked-reference lookup and inventory/details. It neither consumes nor populates
the dialogue scene cache. Event subjects and secondaries (at most 64 distinct IDs)
resolve with one GetAllForms try-read lease. RE::NiPointer pins actors through
shallow copying after lease release, then releases all references on the game
thread. Loaded dead actors are retained; unavailable/disabled/different-world
actors remain unresolved. No worker receives an engine pointer. Base-item metadata
uses its existing separate nonblocking lease and the exact resulting scene owner.

The partial event scene explicitly clears its optional picked actor. Regression
coverage caught assigning zero instead of clearing the optional; that is fixed.
Current source validation and deployment status are in the retired parity ledger ([handoff](HANDOFF.md#retired-ledgers)).
Actual getter timing, original handler coexistence, save/load and menu capture
remain manual-game gates. NPC witnesses, grounded reactions, complete inventory
events and independent VR native bindings remain required for full parity.

## Scene-binding checkpoint (2026-09-06)

Client 59f1188 extends the inactive observer with original callback monotonic
timestamps and an immutable scene-binding boundary. The callback copies time before
native forwarding; identities/item scalars and the original time survive the mailbox
together. Actual observer probes pass 170 checks per MSVC/GCC compiler.

The binder validates arm-time session, native epoch, runtime generation and player
identity against the later exact scene, maintaining a nondecreasing scene frontier.
It rejects future/stale/unowned events with a two-second original-age limit; retries
must retain the already-bound immutable result, never rebind to a newer scene.
Base metadata belongs to the same scene pointer; duplicate FormIDs are ambiguous.
Nearby/visible-after-event is not witnessed-at-event. Only player self equip/unequip
has player-equipment knowledge; NPC source evidence has unavailable player knowledge.
An unresolved nonzero killer ID is retained distinctly from an originally absent killer.

The flat metadata helper is compiled but not called. It uses the pinned
TESForm::GetAllForms map with TryReadLock instead of introducing the blocking
GetFormByID wrapper. At most 32 deduplicated item base IDs are read under one lease;
form/name/plugin access follows lease release on the safe game pump, with live world,
playthrough and generation checks. No inventory, instance or extra-data traversal.
Actual-helper probes pass 20 checks per compiler. These are synthetic native seams,
not in-game lifetime, compatibility or freeze proof.

Context 275 checks and all 17 native suites per build pass; timestamped mailbox
stress accounts for eight million attempts without torn payloads or stranded heads.
No new vtable/offset/handle-format change. No actor observer, metadata capture,
binding call, capability or transport activation is present in the host.
The goal ledger records built-only hashes, packages and the next delivery/history/
knowledge/reaction boundary. Flat and independent VR gameplay proof remain open.

## Guarded actor observer checkpoint (2026-09-06)

Client e7392d1 compiles concrete death/equipment observers plus the scalar policy
observer; none is installed, armed, drained or advertised. This supersedes the
previous foundation's statement that no runtime adapter includes the collector.
Native availability still requires exact runtime 1.11.240 and all three original
slots before any write:

| Observer | Address Library vtable ID | Slot | Original function RVA |
| --- | --- | --- | --- |
| BasicEventHandler death subobject [25] | 4826686 | 1 | 0x11931A0 |
| InventoryEventHandler equipment subobject [3] | 4826615 | 1 | 0x10C17A0 |
| HandlePolicy [0] | 62648 | 7 | 0x109A4F0 |

The callbacks copy pointer-sized address tokens only while the original event is
owned. They never dereference those actors. Existing native handle-policy returns
populate the thread-local scope; only scalar FormIDs reach the mailbox.
Death requires byte +0x10 equal to 1, victim token +0 and secondary token +8.
Equipment copies actor token +0, base FormID +8, original reference +12,
unique ID uint16 +16 and equip byte +18 before forwarding. Invalid boolean bytes
do not publish evidence. An absent killer is distinct from a present killer whose
type/handle cannot resolve; the latter drops rather than fabricating an identity.

All payload writes and reads use one nonwaiting try-gate. Reservations precede
native forwarding; completion follows native return and epoch recheck. A failed
commit gate counts and discards the ticket through a gate-free serial CAS, so a
pending head cannot strand later completed entries. Original event/policy calls
and return values are unchanged; RAII discards pending evidence on exceptions.
No callback acquires engine locks, creates an event source, performs a new engine
call, retains pointers after return or invokes a provider/filesystem/dispatcher.

The exact binary/address audit still passes. Actual-header synthetic-vtable probes
pass 168 checks per MSVC/GCC compiler. Single- and two-producer mailbox stress
account for eight million total attempts, checking payload coherence/order and
explicit drops; this is not measured gameplay throughput or freeze acceptance.
Both flat/VR builds and 17 suites per lane pass, with 489 lifecycle assertions.
The built-only flat DLL hash changes; VR is unchanged. See the goal ledger for
hashes, audited packages and detailed evidence. No deployment or game launch.

The observer/ordering foundation is now implemented. Canonical identity, safe-pump
ownership, player-knowledge eligibility, negotiated immutable transport/history
and grounded AI reactions remain required before activation. Broader death-source
handling and independent VR native evidence remain full-parity requirements.

## Scalar actor identity bridge foundation (2026-09-06)

FOUNDATION IMPLEMENTED / NO NEW HOOK ACTIVATION. Client74a4ba1 adds
core/flat_event_identity_scope.hpp and28 assertions in the existing lifecycle suite.
Current flat/VR DLL hashes remain identical to the preceding player-reaction build;
the collector is not included or called by either runtime adapter yet.

Read-only disassembly of the exact checksum-verified1.11.240 executable found a
usable source of numeric identities: native death and equipment handlers already call
GameScript::HandlePolicy::GetHandleForObject at vtable slot7. Policy Address Library
ID62648 maps to vtable RVA0x25B8620; slot7 is RVA0x109A4F0. The ordinary-form return
branch0x109A6E1..0x109A6F1 reads the engine object's FormID and ORs
0x0000FFFF00000000. Other branches return inventory-instance or other handle kinds;
blindly truncating any returned handle to32 bits is incorrect.

Existing policy calls are at death victim0x119320E, death killer0x11933EB and equipped
actor0x10C1846. This suggests observing the native policy's original return while a
particular original event callback is on the same thread's stack. It does NOT justify
a new GetHandleForObject call, worker form access, pointer retention after callback
return, pointer-address-to-actor caches, event source construction or trusting callback
thread affinity. Nor does it prove a runtime hook is installed or compatible.

The collector compares temporary address tokens without dereferencing them and
retains only decoded scalar IDs. It accepts actor type0x41 and exact ordinary-form
tag; empty, zero/invalid, instance, high-bit, wrong-type and conflicting results are
rejected. Secondary nonnull actors must resolve; they cannot silently become an
unknown killer. A null secondary actor stays absent. Nested scopes restore the
parent during normal/exceptional unwind; separate engine threads cannot populate
one another. Results require the same armed mailbox epoch after native return.

The expanded scratch semantic-event-binary-audit.py now supports --slot and verifies
the policy mapping plus21 selected instructions and the earlier six event sinks.
Exact executable/address-library hashes still match. Full policy disassembly included
the chained fragments beyond the first unwind entry, not only its tiny prologue.

Next complete boundary:
1. Guard the concrete death/equipment and policy slots before any write; refuse
   foreign hooks. Keep arguments, original call counts, results and exceptions intact.
   Instrumentation must add no allocations, waits, extra engine calls or I/O.
2. Reserve bounded ordered event slots before forwarding; harvest scalar identities
   from the original native policy calls, then commit only after native return and
   epoch recheck. Missing/non-actor/instance identities are explicitly unsupported,
   not guessed actor IDs. Account for dropped evidence without inventing events.
3. Resolve only copied scalar IDs on the safe game pump and bind canonical identity,
   player visibility/knowledge, capture ownership and an immutable scene. Death
   confirmation, dying, equipment and consumption remain distinct.
4. Add negotiated typed transport, immutable dedup/history and truthful witness
   semantics before activation. Preserve manual response priority, cancellation and
   original queues. Both build/seam/SQL checks precede manual game acceptance.
VR needs independent native binding evidence; flat handle encoding is not VR proof.


Date: 2026-09-06. Status: negotiated flat quest adapter activation implemented; actual in-game delivery/acceptance remains unproven.

## Activation checkpoint

The flat host now passes guarded hook availability; explicit init_quest_events_accepted
support is required before arming. The safe pump drains only with delivery capacity,
captures a journal and rechecks native epoch/session/generation after capture.
F4SE load boundaries and retirement invalidate capture immediately. Both builds,
the current exact binary/address-library audit and74 observer seam checks on each
MSVC/GCC pass. No game was launched; manual quest/save/load evidence is still required.

## Client delivery checkpoint

The typed v2 codec, bounded QuestEventDelivery and PluginSession worker publication
now retain original captured journals/batch IDs across ambiguous responses, with
acknowledged same-owner historical witnesses and cancellation/drop accounting.
The host still leaves quest_events_available=false. No game-pump install/arm/take,
observe or delivery-pump call exists. Server-support negotiation and exact epoch/
generation adoption must be connected and tested before activation. Full evidence
and remaining gates are in the retired parity ledger ([handoff](HANDOFF.md#retired-ledgers)); no gameplay proof is claimed.

## Foundation checkpoint

Client8306197 implements a 32-entry ordered QuestEventMailbox plus the guarded flat
stage/start-stop observer. The flat entry includes the header for native compilation,
but there is deliberately no install/arm/drain call or advertised capability yet.
The installed DLL contains no activated new quest observation behavior.

Callbacks reserve scalar payloads before forwarding and commit after native return.
A later completed nested callback cannot overtake an unfinished outer handler.
Reservation/take use one atomic try-gate; commit/discard do not acquire it. Pressure
drops are counted globally. Unique slot tickets survive physical-slot reuse without
allowing an old callback to complete a replacement event; a load invalidates pending
and completed data. Exceptional unwinding discards reservations. No engine pointer,
game lookup, allocation, dispatcher, filesystem or provider access occurs in these
callbacks. Consumers must still verify batch ownership after binding their context.

Both DLL builds/all17 suites pass, including400 lifecycle assertions. Actual observer
header seams pass74 checks in both MSVC and GCC; two million concurrent reservations
per platform were accounted as admitted events or explicit pressure drops. The flood
probe is intentionally lossy under contention, not a game cadence/performance claim.
Both packages/protocol checks pass; local client hash is verified in the goal ledger.
No game-dependent claim or VR binding is added. The remaining complete boundary below
is still required before these hooks can be activated.

## Outcome

Exact quest-stage and quest start/stop payloads provide scalar IDs that can be copied
without dereferencing a game object in an event callback. Implement those first.
Keep them distinct from sampled `quest_observation` history and from stage-item
completion. Do not label all three as quest completion or immediately trigger AI.

Combat, death and equipment sinks are now identified, but their actor identities
are pointers, not raw handles. They need an independently safe identity bridge;
copying a pointer to a worker or dereferencing it on an unknown callback thread is
not authorized by this audit. No flat address is valid evidence for VR.

## Exact local evidence

Read-only inspection, never execution, of these existing files:

- `<Fallout4-install>/Fallout4.exe`, version 1.11.240.0,
  image base 0x140000000, SHA256
  fdcef37ac1230af6d0b0050eb2142b139ef3a867b37b9211fb6edfcc646072f8.
- `<MO2-mods>/Address Library for F4SE Plugins/F4SE/Plugins/version-1-11-240-0.bin`,
  652306 records, SHA256
  65985cc2259384a13cffb538d74776e422e62b0cd3766485a000620242b72b06.
- Pinned CommonLibF4 6266ecc9014b473fc6b6efd04abac324477c63cd,
  `include/RE/IDs_VTABLE.h`. No dependency update or third-party code copied.
- `<scratch>/semantic-event-binary-audit.py` validates both
  hashes, executable version, six concrete runtime type/slot mappings and thirteen
  inspected payload-load instructions. Its mapped-vtable scan checks 10371 unique
  addresses using Complete Object Locators and matching base-subobject offsets.
  GNU objdump supplied instruction-level review of selected native handlers.

RVA means address relative to the executable image base. These are *concrete sink*
vtable ProcessEvent slot 1 entries, not event-source factories or naked call hooks.

| Event | Concrete owner / subobject | Address Library ID | Vtable RVA | Original slot 1 RVA |
| --- | --- | --- | --- | --- |
| TESQuestStageEvent | GameScript::FragmentEventHandler / 0x18 | 4826606 | 0x25B50F0 | 0x10AF860 |
| TESQuestStageItemDoneEvent | GameScript::QuestCallbackMgr / 0x00 | 1420673 | 0x25B70E0 | 0x10D4A90 |
| TESQuestStartStopEvent | GameScript::QuestCallbackMgr / 0x08 | 4826621 | 0x25B70F8 | 0x10D4CF0 |
| TESCombatEvent | GameScript::BasicEventHandler / 0xA0 | 4826681 | 0x25DA2E0 | 0x1192AB0 |
| TESDeathEvent | GameScript::BasicEventHandler / 0xC8 | 4826686 | 0x25DA358 | 0x11931A0 |
| TESEquipEvent | GameScript::InventoryEventHandler / 0x18 | 4826615 | 0x25B61A0 | 0x10C17A0 |

The four corresponding generic `BSTEventSink` base vtables all have slot 1 at
0x22C46A2; they are not the concrete delivery sites. A second death consumer is VATS
(ID 4826558, subobject 0x18, slot 1 0xE5D9E0); do not observe both and double-count.
`GameScript::CombatEventHandler` is a misleading name for combat-state work: its
three sink bases are TESHitEvent, TESMagicEffectApplyEvent and BGSRadiationDamageEvent.

## Payload boundaries

For TESQuestStageEvent, native instructions at 0x10AF8B6/0x10AF8BF/0x10AF8C4 read
the uint8 item at +0x0E, uint16 stage at +0x0C and uint32 quest ID at +0x08.
The +0x00 callback smart pointer is not needed and must not be retained or invoked.
These scalar positions agree with the independently maintained
[event declaration at e337150](https://github.com/LucaDotGit/CommonLibF4/blob/e337150ac993daaa9d089fcddf7a6ce9a6bad42f/include/RE/T/TESQuestStageEvent.hpp).

TESQuestStageItemDoneEvent instead reads quest ID at +0x00 (helper 0x10D9635),
stage at +0x04 (0x10D4B73) and item at +0x06 (0x10D4B6E). The upstream
[declaration](https://github.com/LucaDotGit/CommonLibF4/blob/e337150ac993daaa9d089fcddf7a6ce9a6bad42f/include/RE/T/TESQuestStageItemDoneEvent.hpp)
has a misleading +0x08 comment beside the item member; do not copy that comment as
an offset. The actual native load and normal C++ member layout both put it at +0x06.
The event is not interchangeable with TESQuestStageEvent.

TESQuestStartStopEvent reads uint32 quest ID +0x00 and bytes +0x04/+0x05. Native
code resolves the ID as form type 0x50 and treats +0x04 == 0 as the stop path.
The external [declaration](https://github.com/LucaDotGit/CommonLibF4/blob/e337150ac993daaa9d089fcddf7a6ce9a6bad42f/include/RE/T/TESQuestStartStopEvent.hpp)
names those bytes started and failed. Preserve both; a stop is not proof of success,
and a start/stop event does not establish what the player or an NPC knows.

TESEquipEvent uses the actor pointer at +0, item IDs +0x08/+0x0C and equip state
+0x12. TESCombatEvent likewise starts with an actor pointer; its
[declaration](https://github.com/LucaDotGit/CommonLibF4/blob/e337150ac993daaa9d089fcddf7a6ce9a6bad42f/include/RE/T/TESCombatEvent.hpp)
also contains a target pointer and state. TESDeathEvent's native handler reads the
dying actor pointer at +0, killer at +8, and branches on the +0x10 dead byte.
An equip event alone must not be called consumption; a hit is not combat end.

Windows unwind entries may cover only a fragment of a function. The scratch tool's
optional disassembly labels this limitation. The quest handlers were reviewed over
their complete selected address ranges, not just the first unwind fragment.

## Next complete implementation boundary

1. Connect only the guarded flat quest-stage and quest-start/stop forwarding observers.
   Check runtime and expected original slots; refuse foreign hooks. Preserve native
   arguments, original invocation and return value. Do not construct/register an
   event source during save load. Stage-item-done is a separate later decision.
2. Stamp a load epoch before forwarding and copy only required scalar fields while
   the event is alive. Queue after forwarding only if the owner still matches.
   Use a bounded nonblocking FIFO, not a latest-value mailbox: two stages between
   pumps must remain two events. Callback work cannot resolve forms, allocate,
   perform I/O, log per event, call providers or enter the game dispatcher.
3. Invalidate on load/menu/session retirement/shutdown. Drain on the safe game pump,
   resolve each quest ID there, and bind canonical plugin identity, playthrough,
   generation and an immutable context. Failed resolution is unavailable, not a
   guessed quest. Do not overwrite the captured event stage with the later journal
   stage. Bound overflow and expose process-wide drops without attributing old drops
   to the newly loaded save. Do not claim exact capture time from the drain time.
4. Add a coordinated, negotiated, strict event-batch contract in both repositories.
   Current v2 context has additionalProperties=false and no native-event field;
   neither stuffing fields into it nor disguising transitions as active_quests is
   compatible. Use existing bounded workers/cancellation and explicit retry/dedup
   ownership. Keep ordered pending events until acknowledged or explicitly discarded
   at a lifecycle boundary; do not block the game or claim lossless overflow.
5. Archive accepted events transactionally with their exact context/save frontier.
   A retry must not create duplicate history. The existing sampled journal writer
   stays separate. Global quest scripts can include hidden/non-player quests:
   recording an engine event must not automatically expose hidden content to player
   memory, confer NPC awareness or invoke an AI provider. Define an observed-player-
   journal eligibility gate, including truthful treatment of partial discovery.
6. Implement eligible quest reactions only after that history/ownership path works.
   Frozen DialecticServer 907eb634 main_dialectic_pipeline.php:2106-2112 maps
   quest_updated to the RPG-comment lane, which applies a shared 60-second cooldown;
   prompts/prompts.php:268-274 also calls shouldTriggerRPGComment. Port behavior
   deliberately through SYNTH's current permission/target/capability gates. Native
   history existing is not proof of reaction parity.
7. Validate concrete hook refusal/forwarding, null events, reentrant load, repeated
   stages, interleaving producers, pressure/drop accounting, canonical quest
   identity, missing/hidden quests, cancellation, retry dedup, transactional rollback,
   same-save frontier and old-save isolation. Both native builds, mirrored schemas,
   focused PHP/SQL probes and local deployment precede manual game acceptance.

No observer is installed by this audit. Native source subscription, delivery cadence,
player-visible stage semantics, loaded-save quiet periods, long-session behavior,
coexistence with other hooks, flat gameplay and independent VR delivery are unproven.
Current client8306197 is locally deployed with the new observers inactive;
server runtimef26fcf1 remains unchanged.
