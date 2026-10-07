# SYNTH protocol v2

## Player caps payment contract foundation (2026-09-08)

The flat client advertises this contract and requires its own acknowledgement
before native dispatch or paired receipt forwarding. The maintained and local-
compatible server branches support it. See the parity ledger for exact installed
artifacts and validation. Native tests and synthetic HTTP checks do not prove game
payment. VR never advertises or executes the payment action.

`action.player_caps_inventory` requires flat v2, `action.take_caps_from_player`
and all six caps/paired-transfer capabilities below. Only the complete, exactly
correlated `init_player_caps_inventory_accepted` acknowledgement grants this
contract. Older caps/transfer acknowledgements cannot authorize player debits.
v1 and VR reject the player-payment marker. Existing clients keep their old ACKs.

`take_caps_from_player` retains the receiving NPC as `action.actor` and requires
`action.target` to be the captured player, Fallout4.esm/Form0x00000014 in the same
playthrough. Arguments contain only required integer `amount` in 1..1000000.
The same strict amount/currency rules as GiveCapsTo apply, but inventory proof
must come from the player. Available NPC funds never substitute for player funds.
The model instruction requires player agreement; this is not an independently
verified consent token or a separate confirmation UI.

The paired receipt's `inventory` is the player donor; `recipient_inventory` is
the NPC receiver. Stored intent performer/target remain NPC/player. Receipt and
ancestry validation map those roles explicitly, preserving per-side availability,
original snapshots, dialogue speaker and recorded amount through follow-up.
Read-only rechat cannot issue payment. Cancelled/retired owners cannot publish
follow-up actions; exact duplicate receipts replay, while changed pairs conflict.
The client clears readiness before every new-generation init and on retirement.
Missing dependencies, an older ACK or a malformed response cannot grant payment;
the fully negotiated payment ACK also grants its lower caps/transfer dependencies.
Manual flat-game acceptance remains required before claiming working game payment.

## Caps transfer contract foundation (2026-09-08)

The flat client advertises caps and requires the exact acknowledgement before
dispatch or paired receipts. Matched client/server builds, cancellation/replay
probes and local installation are verified; manual gameplay acceptance is still
required. VR never advertises or executes caps. See the parity evidence ledger.

`action.caps_inventory` requires flat protocol v2, `action.give_caps_to` and all
four paired-transfer capabilities below. Caps require `init_caps_inventory_accepted`
or the fully negotiated player-payment ACK above; an older server's
transfer/equipment/Consume ACK is insufficient.
The caps marker is invalid in v1 or VR. Existing clients keep their original ACKs.

`give_caps_to` has the exact captured NPC donor and distinct NPC/player target.
Its wire arguments contain only a required JSON integer `amount` in 1..1000000.
The server checks one observed unequipped Fallout4.esm MISC(35) record0x0000000F,
with sufficient count, before intent writes. Labels and legacy NPC metadata cannot
identify currency. Partial inventory may prove this explicitly observed record;
unavailable or ambiguous inventory cannot. Accepted action ancestry, not the
original stale snapshot, supplies subsequent inventory decisions.

The raw model amount must be an actual integer: strings, fractions, booleans,
null, missing amounts, excess quantity and extra arguments are rejected.
The shared speech envelope's exact empty `item` placeholder is removed before
action encoding; nonempty item fallback is forbidden. Internal canonical decimal
command text becomes integer wire data. Native action catalog definitions override
legacy parameter shapes at runtime without rewriting stored v1 definitions.

Caps use `synth.action.transfer-result.v2`, with both owners and capabilities
bound to the recorded GiveCapsTo intent. Duplicate terminal replay must match
both observations. Single-actor receipts cannot substitute for a transfer;
missing legacy observations invalidate both earlier views. Recorded amount and
recipient survive tool/result follow-ups, including a failed or mixed-quality
receipt. Read-only rechat cannot issue caps mutations; an owned action-result
follow-up may do so after a fresh inventory check. This is server/fixture proof,
not proof that Fallout executed or conserved caps.

## Paired transfer receipt foundation (2026-09-08)

The flat client advertises paired transfers and requires the separate acknowledgement
before dispatch. VR never advertises or executes transfers. See the parity evidence
ledger for matched local installation and outstanding manual gameplay acceptance.

The distinct `synth.action.transfer-result.v2` receipt requires both `inventory`
(the donor) and `recipient_inventory`, each with an exact actor, whole item list
and independent complete/partial/unavailable quality. Both actors must be distinct
loaded FormIDs in one playthrough; names or different plugin labels cannot make a
self-transfer distinct. Existing single-actor receipt schemas remain unchanged.

`action.transfer_inventory` requires flat protocol v2, `action.give_item_to`,
`action.inventory_observation` and `dialogue.turn_ownership`. Each inventory is
limited to 32 rows, or 512 with `context.inventory_512`. The total encoded request
still has the ingress 1 MiB byte budget; encoding never silently drops rows.
The native encoder and PHP ingress validate both observations independently.

Persistence binds both identities to the recorded donor and target of GiveItemTo,
with the same session, turn, generation, context and capability ownership as its
parent request. Terminal replay must match both inventories. A single-actor v2
receipt cannot stand in for a transfer. A legacy receipt with no inventory evidence
invalidates both actors' previous inventory views instead of reviving the snapshot.
An unavailable observation for one actor does not erase valid evidence for the other.
Accepted ancestry updates the newest inventory per actor without rewriting captured
snapshots. These are wire/database guarantees, not proof of actual game transfer.

`init_transfer_inventory_accepted` acknowledges the four required capabilities
and individually requested lower inventory/event features, including Consume only
when its own markers were requested. Equipment or Consume ACKs never authorize
transfers. A complete, exactly correlated init is required; reinit and hard halt
invalidate readiness. Unready paired receipts are rejected before network traffic.

The server resolves recipients only from the owned captured context. Transfer
emission requires a distinct captured NPC donor and NPC/player recipient, one
playthrough, an item of at most 255 UTF-8 bytes, and an integer amount in 1..1000000.
Omitted legacy quantity means one; canonical decimal command text becomes a JSON
integer. Fractional, excessive, conflicting and extra action arguments fail before
intent writes. The native client further requires the exact observed usable stack
and sufficient count, then repeats live actor/stack/deadline checks on dispatch.

The client prepares the same byte-bounded paired inventory rows for its immutable
continuation snapshot and the wire receipt, preserving independent observation
quality. No mutation is authorized by preparation or acknowledgement alone.
Actual-main mixed transfer/rechat, cancellation, replay, conflicting receipts and
generation retirement pass on maintained and installed-compatible schemas. An
impossible client pair terminates only its owned turn; it never invents a receipt.
Matched local deployment and manual game proof are tracked separately in the ledger.

## Acknowledged consumption inventory (2026-09-08)

Consumption additionally requires `action.consume_inventory` and `action.consume`
alongside `action.inventory_observation` and `dialogue.turn_ownership` on a flat
v2 session. The marker is invalid without these prerequisites, in v1 or in VR.
`init_consume_inventory_accepted` acknowledges this full contract and individually
requested lower inventory/event features. An equipment-only or older ACK never
authorizes consumption. Failed, incomplete, cross-owner, halted or reset sessions
cannot retain consumption readiness.

Consume executes one exactly bound observed ALCH item through the flat native
adapter, then sends the same typed inventory receipt used by equipment. The server
requires Consume's recorded intent and both parent/receipt capability sets.
Terminal replay must be identical; conflicts and wrong actors/timelines fail closed.
Mixed action/rechat ancestry uses the newest whole-inventory observation for that
actor. Complete empty means known empty; unavailable or a missing legacy mutation
observation clears earlier inventory instead of restoring the original snapshot.
Follow-up tool text names the recorded item, never the actor's display name.

The source client dispatch and server contract are connected but Consume remains
unadvertised pending matched integration/cancellation tests and deployment gates.
This contract is not proof of native consumption, magic effects or game stability.

## Acknowledged post-action inventory (2026-09-08)

For v2 flat init requesting both `action.inventory_observation` and
`dialogue.turn_ownership`, `init_action_inventory_accepted` acknowledges the
accepted receipt and mixed action/rechat inventory consumer contract. Like the
existing hierarchical init markers, it also acknowledges individually requested
extended inventory, actor, player and quest features. It never enables a feature
absent from that init's capabilities.

The client requires a complete, exactly correlated three-line start/status/end
reply before allowing a typed inventory receipt. Requested capabilities alone,
older ACKs, v1, VR, failed/truncated/cross-owner responses do not grant readiness.
A new initialization resets readiness; hard halt disables it. Explicit attempts
to send typed inventory before readiness fail locally, without network traffic.
Flat PluginSession advertises the equipment inventory capability; Consume has its
separate, still-disabled advertisement gate. Wire readiness is not gameplay proof.

## Canonical dialogue listener transport (2026-09-07)

An initialized session advertising both `dialogue.listener_identity` and
`dialogue.turn_ownership` may receive an optional `payload.listener` identity on
dialogue lines. It is resolved from the exact request-owned context using the same
route as rechat/history, before the final emitted line hash is stored. Speaker and
listener must be distinct identities in the same playthrough. Narrator, unknown,
ambiguous, self-addressed and uncaptured routes omit the field; omission never
means player or current crosshair. No names or latest actor registry grant authority.

V1 and clients without the capability retain byte-identical response framing.
Clients reject unnegotiated listeners, malformed identities and wrong request,
turn, generation or context owners before response callbacks. This transport alone
does not enable actor rotation; live actor eligibility and exact scene membership
must be checked by the native presentation consumer before any game mutation.

## Negotiated extended inventory transport (2026-09-06)

Optional v2 flat capability `context.inventory_512` permits up to 512 observed
inventory rows per actor. A complete, correlated init start/status/end response
with `init_inventory_512_accepted` acknowledges this extension and the existing
actor/player/quest features individually requested by that same init. It does not
enable unrequested features. VR, v1, missing-capability, malformed/failed init and
older acknowledgements retain the 32-row wire limit. Every new generation resets
negotiation. Server ingress also requires the initialized session's exact capability
manifest; a context cannot add the extension after initialization.

V1 actor-state schema is unchanged. The v2 actor-state schema differs only in its
inventory limit and schema/reference identity. All identity, item, observation and
other actor-state validation remains intact. The core may own up to 512 rows;
transport support does not attest that the native adapter captured that many.

The worker copies actor states before applying the negotiated row limit. Any
truncation is explicitly `partial`. The JSON writer retains its 1 MiB byte bound;
a typed byte-limit failure permits halving the largest remaining inventory and
retrying encoding. Escaped string bytes and the final newline count toward the
HTTP ingress budget. Other JSON errors propagate. If even a context with no inventory
cannot fit, publication fails without sending it. Captured immutable snapshots
are never edited or joined to newer inventory observations.

This is transport preparation, not full inventory parity: native capture remains
32 rows with its existing structural bounds; ordinary prompt selection remains
12 principal / 4 equipped bystander rows. Bounded larger native capture, relevant
inventory retrieval, inventories beyond safety bounds, generated names, and manual
flat/VR acceptance remain separate unfinished requirements.

## Actor-event negotiation and bounded delivery (2026-09-06)

An opted-in v2 flat init receives a complete correlated start/status/end response
with `init_actor_events_accepted`. That marker acknowledges actor source history
and any requested existing player/quest features. It never enables an unrequested
capability. Old, malformed, truncated, wrong-owner or wrong-sequence responses
cannot enable actor delivery; every new generation resets support.

Actor contexts require an explicit `context_accepted` status before queue progress.
The client retains at most four immutable source captures. A worker splits only
between records to retain every subject and resolved secondary within the existing
16-state limit, including the player. Each fragment has its own stable serial;
retries retain its exact scene, records and serial while delivery age advances.
Complete acknowledgement or terminal validation/4xx rejection advances that
fragment; timeout, ambiguous response, 408 and 429 retain it. Stale completions
cannot consume another fragment. Halting the session stops admission and draining.

PluginSession owns this queue, exact retained-scene actor selection and bounded
background scheduling. Ordinary dialogue selection is unchanged. The constructor's
actor-availability gate defaults false; the flat host passes true only when its
exact-runtime native observers remain installed. It arms after negotiation and a
fresh owned scene, drains on the game pump and invalidates immediately on load or
retirement. Event-only captures bypass dialogue caches and nearby discovery; exact
loaded actor states use one nonblocking form-table lease and shallow copying.
Manual flat acceptance is still required; host wiring is not gameplay proof.
VR has no actor-event capability and receives no flat binding.

## Native actor-event source contract (2026-09-06)

Optional v2 flat capability `context.actor_events` permits `payload.actor_events`.
Encoding, admission and persistence now have the negotiated delivery path above.
Flat host capture is wired behind exact native availability and negotiation.
Actor-event reactions remain unimplemented; raw source is not NPC knowledge.

A batch has `batch_id=actor:<positive wire-safe serial>`, nonnegative integer
`age_ms` and 1-32 ordered events. Age measures delivery delay since the bound
scene; each event's immutable `capture_delay_ms` measures original callback to
scene capture (0-2000 ms). Retrying never replaces the scene or refreshes source
time. Batch IDs are scoped to session, generation and playthrough.

Each event carries kind (death/equipped/unequipped), canonical actor identity,
original secondary FormID, optional resolved secondary identity, optional base
item metadata, original reference, uint16 unique ID, capture delay and explicit
player knowledge. An unresolved nonzero killer ID stays nonzero with null identity.
Death carries no item/reference/unique ID. Equipment carries no secondary actor;
base item labels do not establish stack counts, consumption or instance identity.

Admission resolves every named identity against exactly one enabled actor state
in the owned context. Only the player's own equip/unequip can claim
`player_equipment`; all other source records remain `unavailable`. Nearby
presence or later line-of-sight is not evidence of witnessing the earlier event.
Actor states remain bounded to 16 and require player/audience membership; future
delivery must retain all event identities without inventing witness eligibility.

Schema migration 20260906004 adds `synth_meta.actor_event_batches` for raw source.
Source insertion and the player-only `actor_equipment` history projection share
the context transaction. NPC events never enter that projection or eventlog.
Raw actor batches are removed from the shared prompt snapshot, not from stored
evidence. History is player-only, has no inferred location or NPC witnesses, and
obeys the existing context/save frontier. Duplicate batches retain their original
context and age; changed bodies/player identities conflict and roll back. Inactive
or wrong-generation owners cannot persist or reuse a prior batch receipt.

Optional flat dialogue.quest_reactions plus context.quest_tracking enables the closed
quest_updated trigger with positive previous_context_sequence less than context_sequence.
Only the exact latest earlier fresh journal of the same active player owner is eligible.
No instruction/text/capture_id is accepted. The server independently verifies tracked
objective change, active visible speaker and earliest admission per current journal.
Explicit init_quest_reactions_accepted also acknowledges requested quest events/tracking;
older ACKs never enable reactions. Existing RPG policy/chance/cooldown remain authoritative.
See ../../docs/QUEST-REACTIONS.md for eligibility and acceptance boundaries.

The event and streamed-response envelopes use v2. Context has a dedicated v2 payload
schema for the optional saved observation anchor. Other payloads, identities, actions,
speech and opaque media retain their v1 schemas in ../v1/schemas/.

Every v2 event requires context_sequence. It is a positive, previously acknowledged
snapshot_sequence for input_text, input_audio, activate, trigger, profile_refresh and
action_result; init, context, cancel and halt require zero. Context publication has its
own positive payload.snapshot_sequence. Sequences are publication order, not frame IDs.
They advance within one session/generation, whose first context fixes the playthrough.

A bound request selects exactly (session_id, generation, context_sequence), scoped to
the active playthrough. Missing, stale or mismatched bindings fail; there is no latest
snapshot fallback. The stored payload supplies immutable identities and display names.
Rechat must retain its originating binding. Action results must match the binding of
the request that emitted the action. Every response line echoes protocol_version and
context_sequence, including start/end/status lines; clients reject mismatched echoes.

Init pins the protocol version for that generation. Changing versions requires a new
init generation; v1 sessions keep their existing contract and latest-context behavior.
Visual capture and media transfer keep their independent v1 contracts; a describe
trigger still needs a v2 context binding. The base v2 binding uses the existing
immutable context_snapshots and canonical protocol_events keys.

An ambiguous display-name response is rejected rather than selecting a different
same-name actor from historical registry state. Canonical actor-profile isolation is
a separate remaining parity milestone.

Optional capability dialogue.turn_ownership pins a logical response chain to the
same turn_id and context_sequence through text/audio, rechat and action results.
Manual activation and non-rechat triggers may start a root too. A new root must use
a higher acknowledged context sequence; publishing context alone does not interrupt
the existing owner. An obsolete continuation fails with superseded_turn (HTTP 409).
Init resets ownership, and capabilities remain fixed within the generation. Clients
without this capability retain their prior behavior; there is no new envelope field.

Server migration synth_protocol_schema 20260904001 adds runtime owner fields and a
nullable session_id to worker_jobs. Cancellation is session/request/generation scoped;
late completion cannot replace cancellation. Halt and reload invalidate only their
session's work. Legacy jobs without session ownership are not guessed or cancelled
by another session's Halt. Guards check durable ownership at response, provider,
action-intent and deferred-speech boundaries. They do not yet abort an in-flight
provider connection or make all legacy memory/cache writes transactional.

Optional v2 capability dialogue.turn_cancel (alongside dialogue.turn_ownership) makes
cancel_request_id naming an acknowledged context publication cancel that logical
turn. Its exact session/generation/turn_id/context_sequence identifies the response
chain; ordinary request IDs and clients without the capability keep request-scoped
cancellation. The context execution becomes a durable cancelled anchor, so a delayed
root or continuation cannot start after the cancel wins the claim lock. Cancelling an
older anchor cannot invalidate a newer sequence, even with reused turn text. Worker
rows require matching payload.protocol_event.context_sequence for chain cancellation;
unattributable jobs are not guessed. No payload field or database column is added.

Optional v2 capability runtime.retirement permits Halt to create a halted session
record before its init claim has arrived. The normal session advisory lock and
monotonic generation rule then reject that delayed same-generation init. An explicit
higher-generation init may resume the session. This covers an init acknowledgement
lost during client retirement; clients without the capability retain the old unknown-
session rejection. No schema column or payload field changes. Retirement is bounded
best-effort signalling, not a guarantee when the server is unreachable or the process
is forcibly terminated.

Optional context.saved_anchor allows context payload.loaded_context containing
playthrough_id, session_id, generation and context_sequence. It identifies the last
acknowledged observation carried by the loaded F4SE co-save, not the current session
or the latest server row. All IDs/numbers are strictly bounded; the playthrough must
match the new context. The first context fixes this optional value for the generation;
later publications cannot add, remove or change it. v1 rejects the new field.

Absence means no usable saved anchor (including legacy/missing/invalid/unsupported
records), not permission to infer one from game time. The server preserves this
metadata in the immutable context payload. Its explicit resolver requires the exact
session/generation/sequence, same playthrough and runtime lane; old retired sessions
may supply an observation, but missing or incompatible records return unavailable.
The resolver is groundwork for scoped history and is not yet used to select narrative
or memory history. No fallback, history deletion or legacy data reassignment is added.

This is not a unique save ID, complete dialogue frontier, delivery acknowledgement or
timeline lineage. Multiple saves may share one observation. Unfinished AI work and
later background writes are not proven saved by this pointer. Separate durable history
watermarks, branch ancestry and scoped reader/writer migration remain required.

Diary completion: POST the exact originally admitted diary_request event
to /diary_status.php, not /main.php. This is a read of that canonical owner, not a
new claim, replay, provider dispatch or save acknowledgement. Only v2 with the
original role/reason capabilities is accepted. Unknown/changed/retired owners
return HTTP409 diary_owner_unavailable; temporary read failures return HTTP503.
The response is exactly response_start, one status and response_end, retaining the
original request_id, turn_id, generation, runtime lane and context_sequence.
Status codes: diary_queued, diary_running, diary_ready, diary_failed,
diary_cancelled or diary_disabled. Ready requires every admitted job's validated
published result and source provenance, not just a ready worker flag. It does not
mean saved. A later acknowledged fresh context must include the completed result
before the client may advertise next-save inclusion. Earlier saved frontiers
remain unchanged. Polls never overwrite the original queued/disabled admission.

Batch status waits while any admitted job is running or queued, even if another job
has failed or been cancelled. Once every admitted job is terminal, an owner whose
initialized capabilities include diary.batch.status receives diary_partial when at
least one validated published result coexists with a failed/cancelled job. All-success
remains diary_ready; no-success remains diary_failed/diary_cancelled. Disabled roles
are not failed jobs. Owners without this capability retain the old terminal codes.
The capability is immutable session state, not an option added only when polling.
Native clients require it before sending a multi-role/actor batch and reject partial
status for a single selected role or an admission response. Partial success requires
the same fresh acknowledged context checkpoint as ready, but the completion notice
must say only successful entries are included in the next save. A status read itself
never advances the saved frontier or retries failed generation.

Native quest batches (optional v2 flat capability context.quest_events): a context may
carry quest_events with one batch_id and 1-32 ordered scalar transitions. Repeated
identical records are meaningful and are not collapsed. Stage retains its original
16-bit stage and 8-bit item; started/stopped carry only the native failed flag.
Stopped is not evidence of success. This transport is not an AI-reaction trigger.

visibility_sequence=0 binds visibility to this context. A positive sequence must
precede snapshot_sequence and resolve an immutable context in the exact same
session, generation, playthrough and player identity. Only a fresh complete/partial
quest journal with observed displayed objectives witnesses visibility. Cached,
unavailable, absent or unseen quests are rejected, not invented. Historical witnesses
can support stop events after disappearance; they do not replace transition stages.
No automatic fallback to another session, loaded ancestor or latest context exists.

History stores one quest_events row per batch in capture order, separately from
sampled quest_observation history. The current context's save frontier includes it.
An identical batch retry in the same owner is a no-op before witness resolution;
a changed batch or player under the same ID conflicts and rolls back admission.
The ID namespace resets with session/generation/playthrough. New-generation retries
must not replay old captures. Visibility failure or persistence failure rolls back
the entire context. Native delivery, witness retention and capability activation
remain client milestones; publishing this contract does not enable game hooks.

Quest-support handshake: an admitted v2 flat init advertising context.quest_events
receives exactly start/status/end with status code init_quest_events_accepted and
the init's unchanged correlation/sequence-zero owner. Other init requests retain
init_accepted. A native client must receive this complete, correctly correlated
three-line acknowledgement before arming quest capture or publishing batches.
Ordinary dialogue remains available when an older server omits the support marker.
Malformed/failed/stale initialization never enables capture. Every new generation
clears the negotiated state; a previous session's marker grants no later authority.

Fresh-scene continuation contract (staged, not runtime-activated):
dialogue.rechat.scene requires dialogue.rechat.ownership and dialogue.turn_ownership.
A rechat using that capability must include payload.scene_context_sequence; without
the capability this field is forbidden. It is a positive safe integer strictly
greater than the immutable chain context_sequence. Parent request/line, turn,
generation and context_sequence remain unchanged. JSON schemas cover shape and
capability requirements; native/PHP validation additionally checks the cross-field
sequence ordering. Existing parent-only clients retain their existing contract.

Activation requires an explicit correlated server acknowledgement, a fresh
game-thread capture at dispatch, and an acknowledged immutable scene publication.
The server must resolve that exact scene in the same live session, generation,
playthrough and player identity, after the preceding scene and within a bounded
admission age. It must validate scene identity and the previous speaker before
profile selection or any provider work. Fresh audience/state must feed selection,
prompt context, listener routing, actions and witnessed history consistently;
history ancestry and one-child reservation still use the immutable parent chain.
No latest-context lookup, same-name substitution or silent stale-scene fallback.

The codec and ingress shape checks are published ahead of runtime activation.
Client defaults do not advertise dialogue.rechat.scene. Server preparation rejects
scene-bearing requests with rechat_scene_not_activated until the owned scene reader,
history projection and negotiated client handoff are implemented and tested together.

Physical scene capture uses optional payload.world.scene under dialogue.rechat.scene.
It contains exactly cell and worldspace. Each reference contains a nonzero form_id
and origin_plugin; interior worldspace is null, exterior worldspace is required.
The enclosing context supplies session, generation and playthrough ownership.
Display labels remain prompt context only. Missing file/reference metadata means
scene identity is unavailable, not an inferred cell or worldspace. Unnegotiated
peers receive the original world fields without scene. This data contract does not
remove the staged continuation activation gate above.

Fresh-scene initialization uses an optional boolean rechat_scene on the existing
v2 status payload. Only true on a completed, exactly correlated init acknowledgement
grants support. This supplements the existing init code so flat inventory/quest
acknowledgements retain their meaning; flat and VR scene readiness are independent.
A client offering dialogue.rechat.scene must refuse initialization without that
acknowledgement, not change its initialized capability manifest or silently use an
older scene. A new generation clears readiness. The v1 status payload is unchanged.

The client capture handoff is staged behind that readiness: the runtime host copies
the retained actor's scene outside PluginSession, then verifies session/generation
ownership before submitting it. Null, cancelled, replaced, changed-scene and older
than two-second captures are discarded. Publication remains on the network worker,
and only scene_context_sequence advances; the original turn and parent stay bound.
The server does not emit this acknowledgement and client defaults do not request it
until combined activation checks pass.
