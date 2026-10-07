# SYNTH protocol v1

## Current transfer status (2026-09-08)

Equipment, Consume, paired GiveItemTo and GiveCapsTo are locally installed in
the matched flat client/server candidate. Manual gameplay acceptance remains open.
Older staged notes below are historical. Caps require flat v2, their exact
init_caps_inventory_accepted acknowledgement and all paired-transfer dependencies.
Read-only rechat cannot issue mutations through either catalog fallback or direct
emission. See the retired parity ledger ([handoff](HANDOFF.md#retired-ledgers)) for builds, hashes and outstanding flat/VR gates.

## Staged post-action inventory receipts (2026-09-08)

SOURCE ONLY / NOT ENABLED. A v2 action_result event may carry the tagged
synth.action.result.v2 result with a closed inventory object: canonical actor,
items and complete/partial/unavailable observation. Unavailable requires no rows.
The existing v1 result remains valid and unchanged. The extension requires
action.inventory_observation, dialogue.turn_ownership and a positive original
context_sequence. More than 32 rows additionally requires flat context.inventory_512;
512 rows is the maximum and the existing request-byte limit still applies.

The server binds observations to the stored EquipItem/UnequipItem intent, its
canonical actor/playthrough, parent request, turn, generation, context, lane and
capabilities, with an active runtime/turn and non-retired intent. Exact repeats are
idempotent; conflicting observations cannot overwrite the first terminal result.
The original context snapshot is never rewritten.

The plugin does NOT yet advertise this capability. Its forwarding path and mixed
action/rechat prompt/CheckInventory/client inventory views are source-connected.
Receipt admission reads the original context; consumers overlay only accepted exact
ancestry (64 combined edges / 8 MiB maximum), without changing original snapshots.
Rechat reads verify completed parent/route evidence under the actual request lock.
The current candidate view may precede reservation; historical rechat edges require
reserved/prepared nonterminal selection and matching parent body/line receipts.
Explicit readiness is implemented: only the exact correlated init_action_inventory_accepted
ACK enables typed receipt forwarding. New initialization and halt revoke readiness.
Full-main/model acceptance and advertisement remain pending.
Wire/database tests do not establish end-to-end AI freshness or gameplay acceptance.

## Staged native rechat parent contract (2026-09-07)

SOURCE ONLY / NOT ENABLED. Protocol v2 capability dialogue.rechat.ownership
requires dialogue.turn_ownership. Rechat triggers using it require paired
reply_to_request_id and reply_to_line_id plus a positive context_sequence.
IDs contain 1-128 ASCII characters: first alphanumeric, then alphanumeric or
._:-. Parent fields are rejected on non-rechat or capability-off requests;
the parent request cannot equal the current request. V1 and capability-off
wire shapes are unchanged.

The client retains the actual eligible Line request/line IDs in one
PendingRechat bundle with its copied actor, snapshot and original context,
turn, generation and cancellation binding. Queue dispatch moves that bundle;
halt, supersession and actor suppression clear it together.

The plugin does not advertise this capability. SynthClient's
uses_rechat_parent_contract checks wire selection, not acknowledged server
support. The server validates the shape but rejects owned rechat with
owned_rechat_not_ready before legacy binding. Schema acceptance is not proof
of a completed parent. Enablement still requires explicit server readiness,
durable emitted routes, completed-parent admission and replay-safe decisions.

## Extended inventory transport (2026-09-06)

V2 flat `context.inventory_512` is explicitly acknowledged by
`init_inventory_512_accepted`. It allows 512 rows per actor under the existing
1 MiB ingress budget; old peers and VR keep 32. Trimming stays partial and owned
by the original snapshot. See protocol/v2/README.md for negotiation, byte-budget
and incomplete native/prompt coverage boundaries.

## Native actor-event source contract (2026-09-06)

Optional v2 flat capability `context.actor_events` permits `payload.actor_events`.
The following source contract is extended by the negotiated delivery checkpoint
in protocol/v2/README.md. Flat host capture is wired behind exact native
availability, negotiation and lifecycle ownership; reactions remain unimplemented.

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
Actor states remain bounded to 16 and require player/audience membership; bounded
delivery retains all event identities without inventing witness eligibility.

Schema migration 20260906004 adds `synth_meta.actor_event_batches` for raw source.
Source insertion and the player-only `actor_equipment` history projection share
the context transaction. NPC events never enter that projection or eventlog.
Raw actor batches are removed from the shared prompt snapshot, not from stored
evidence. History is player-only, has no inferred location or NPC witnesses, and
obeys the existing context/save frontier. Duplicate batches retain their original
context and age; changed bodies/player identities conflict and roll back. Inactive
or wrong-generation owners cannot persist or reuse a prior batch receipt.

## Native acknowledged player reactions (2026-09-06)

Optional v2 flat dialogue.player_reactions also requires context.player_events.
Only a complete correlated three-line init_player_reactions_accepted handshake
enables it. That marker also acknowledges requested player-history and existing
quest features; init_player_events_accepted still enables history only. v1 and
VR cannot activate or send player reactions.

trigger kind player_reaction carries only actor and event_id (player:<positive
wire-safe serial>). Client narration, game time, visual capture and previous quest
sequence are forbidden. The server derives level_up/combat_end and text from the
original immutable history, exact session/generation/player/playthrough and current
save frontier. A currently active, visible, available audience NPC is required.
The player must be alive, enabled, out of combat, at least the observed level,
and no earlier on the game clock. The current context must have been received
within ten seconds; original capture age plus server elapsed time must be at most
sixty seconds. Retries never refresh the original age.

The first admitted trigger for that owned event ID is the only reaction candidate,
even across different context sequences. Ineligibility returns
player_reaction_suppressed without a model call. Existing levelup/combat_end profile
settings, chance and atomic RPG cooldown still apply. The bound native cue overrides
legacy/custom victory assumptions after prompt loading, without changing policy
extras: combat ending does not prove victory, kills, enemies, weapons or witnesses.
Level-up does not prove a perk, skill, reward or cause.

Client history ACKs can retain up to sixteen independent reaction candidates.
They expire sixty seconds after the original steady-clock observation and are
discarded on identity/timeline discontinuity. History delivery continues when no
speaker exists or reactions are unsupported. Existing owner/crosshair/nearest
active-NPC selection is shared with quest reactions. Worker context must be no
older than two seconds; an accepted queue submission consumes the candidate once.
Normal turn/generation cancellation applies. One-shot responses cannot enqueue
actions or rechat. Flat dispatch is connected; VR remains explicitly gated off.
Source/build/SQL/HTTP validation is not gameplay acceptance.


## Native sampled player history (2026-09-06)

Optional v2 flat context.player_events permits one player_event object on a context
publication. The client must first receive a complete three-line init response with
init_player_events_accepted. This marker also acknowledges any requested quest-event,
quest-tracking and quest-reaction capabilities implemented by this server; older
markers retain their existing meaning and never enable player events. v1 and VR
neither send nor negotiate this extension.

player_event contains event_id (player:<positive wire-safe serial>), kind
(level_up|combat_end), before and after states, interval_ms and age_ms. A state has
only positive game_time_ticks, level 1..32767 and boolean in_combat. Times/levels
must not reverse; level_up requires a strict increase, combat_end requires true
then false. interval_ms is positive and age_ms is nonnegative; both are wire-safe
integers. No text, action, victory, kills, weapons or witness claims are allowed.
The envelope's exact player identity owns the transition; native encoding checks
both original samples against that identity, generation and lane.

Delivery attaches original evidence to a newly captured scene, never republishes an
old scene to acquire an event sequence. Workers reject scenes older than two seconds;
transient failures retain the same event identity/facts for another fresh capture.
The client requires a complete context_accepted response before consuming the FIFO
head; game-thread receipts carry exact serial ownership. Terminal rejection is
observable. Sampling stays independent of response work, while delivery is bounded
and yields to current dialogue/input. Unsupported servers leave capture disabled.

Server admission validates the current player/time evidence, writes one immutable
player_event history row under the session transaction/lock, and finalizes its
context/save frontier atomically. Schema migration 20260906003 adds the matching
partial unique index. Same session/generation/playthrough/event ID plus identical
player and facts is a retry; conflicting evidence fails the transaction. age_ms is
delivery metadata rather than event identity, and retries never rewrite the first
insertion age or original save frontier. Event time comes from the original after
sample; location is unspecified and only the player is marked as an observer.
Passive history does not activate NPCs, create model jobs or replace dialogue owners.

Automatic level-up/combat-end reactions are a separate unfinished gate. They must
bind this durable event to a fresh eligible active NPC, deduplicate reaction admission,
apply existing RPG settings/chance/cooldown, and avoid legacy combat prompts that
assume defeated enemies. History admission alone is not gameplay or response proof.

## Native owned quest reactions (2026-09-06)

Optional v2 flat quest_updated requires dialogue.quest_reactions plus context.quest_tracking
and an explicitly negotiated init_quest_reactions_accepted or init_player_events_accepted response. It binds a positive
previous_context_sequence strictly older than the current owned context. No client prose
or visual binding is accepted; the server recomputes the change and speaker eligibility.
v1 and VR do not enable it. See QUEST-REACTIONS.md and protocol/v2 for the full contract.

## Native quest tracking evidence (2026-09-06)

Protocol v2 context quests may carry an optional boolean `tracked`. True and
false are observed player tracking states; omission means unavailable. Explicit
null is invalid on the wire. The field requires the flat lane and negotiated
`context.quest_tracking`; v1 quest schemas remain unchanged. Fallout 4 can track
multiple quests, so this is per quest, not a guessed single selected entry.

The flat adapter copies the pinned CommonLibF4 `QUEST_DATA::flags` kActive bit
inside the existing bounded game-thread journal capture. It does not call
Papyrus, add a hook, spin on a lock, or perform network work. VR leaves the
optional value unset. Tracking participates in immutable snapshot equality, so
toggling it is a journal change even when stage and objective text are unchanged.

For an admitted v2 flat init, the server acknowledges requested support:

| Requested supported capabilities | Status code |
| --- | --- |
| Quest events only | `init_quest_events_accepted` |
| Quest tracking only | `init_quest_tracking_accepted` |
| Both | `init_quest_events_tracking_accepted` |

No opt-in, legacy and VR retain `init_accepted`. The client requires the complete
correlated start/status/end acknowledgement and its requested capability before
sending tracking. An older server's quest-events acknowledgement still enables
quest capture, but tracking is stripped from contexts and read-quests results.
Failed initialization clears both acknowledgements.

Server-owned prompt/history projections normalize absent tracking to null and
label it unavailable. History comparison preserves the distinction between null
and false while ignoring JSON object-key order. The original context stays
immutable. Tracking is journal knowledge, not proof that an NPC heard an event.
This does not yet enable automatic quest-reaction dispatch.


## Passive world observations and native history (2026-09-06)

The existing v2 context message can be published without a target, activation or
model request. Both native entry pumps consider changed location/cell/worldspace/
weather/interior labels on their existing two-second background cadence. Clock-only
changes do not trigger passive publication. Admission is single-flight, idle-only,
menu/cancellation/generation-aware and rechecked against newer acknowledged frames.
Ordinary successful context publication acknowledges the same observed state.
Failure does not acknowledge it; later eligible observations may retry. This samples
world facts, not every intervening movement or a proven entry/exit event.

V2 derived world, nearby-item and point-of-interest observations now enter eventlog
and immutable native history in one database statement within context admission's
transaction. Their owner is payload.snapshot_sequence, not the envelope's zero
context_sequence. The referenced snapshot/playthrough and active runtime generation
must exist. World observations belong to the captured player; no Narrator speaker
or NPC visibility is invented. Existing exact-reader and saved-frontier rules apply.
Before admission commits, the new context frontier includes its own derived
observations as well as preceding history. Finalization selects only this exact
session/generation/playthrough/snapshot/request's observation rows while the existing
native-writer lock remains held. Failure rolls back admission; later dialogue,
workers and future observations cannot advance an acknowledged checkpoint.
Loading an older anchor retains its own observations but excludes later branches.
Mutable log edits cannot rewrite that history.
Legacy v1 writes remain unchanged; no old records are backfilled or given guessed
ownership. There is no new wire field, schema migration or product version.

## Captured audience completeness (2026-09-06)

Both context payload versions accept optional `audience_observation`: `complete`,
`partial`, or `unavailable`. Unavailable requires an empty audience. This describes
the exported captured actor set, not every actor in the world and not the separate
player/target state. Missing quality on an old snapshot means partial when its
audience is nonempty, otherwise unavailable; stored history is never rewritten.

The native adapter reports partial when the nearest-64 retention limit omits
actors, handles/identities/positions cannot be resolved, or snapshot construction
rejects a candidate. No process lists/bootstrap means unavailable. An exhaustive
successful empty traversal is complete. Supplemental exact-actor capture and
hearing/participant filtering downgrade completeness when they add/omit identities.
This change does not cap source traversal, change targeting, create departure
events or prove bounded frame time. No actor pointers are retained across frames.

The server preserves the field with its exact context sequence and supplies it
to text/audio/trigger context and the native main prompt. An omission must not be
interpreted as departure, death or world absence. Complete attests only this
capture's discovery set. Deploy server first: older strict servers reject the new
optional field until their coordinated schema/ingress update is installed.

## Health and action-point observation availability (2026-09-05)

Actor states accept optional booleans `health_percent_available` and
`action_points_percent_available`, independently. The numeric fields retain
their0..100 wire shape for older payloads; a number is authoritative only when
its corresponding availability flag is explicitly true. Missing flags on older
snapshots are unknown, not proof of a successful read. No history is backfilled.

Native capture emits false with a zero placeholder for a missing definition,
non-finite current/permanent actor value or non-positive denominator. Observed
zero remains true/0. The existing ratio to permanent actor value and0..100 clamping
are unchanged; temporary-effect/full engine percentage semantics are not newly
established by these flags. No additional actor-value engine calls were added.

Saved snapshots retain original numbers/flags. AI prompt projections emit null
for unavailable or legacy-unattested percentages and preserve each independent
flag. Inspect-actor action detail likewise emits null instead of a numeric default
when that snapshot did not attest availability. Selected-NPC detail enrichment
preserves the originally frozen scalar values and flags rather than mixing reads.
Roll out the accepting server before the emitting client; strict older servers
reject unknown optional fields. Both protocol lanes share this additive contract.


## Faction-list completeness (2026-09-05)

Actor states accept optional `faction_completeness`: `complete`, `partial` or
`unavailable`. This is separate from `faction_observation`, which identifies
the source as effective runtime membership, base-only membership or unavailable.
Complete base-only data does not attest current runtime memberships.
Only complete effective data can support absence from the current faction list.
An unavailable list must be empty and use unavailable observation mode; conversely,
complete/partial requires effective or base_only mode. Both protocol lanes accept
older payloads without this field. Their completeness is interpreted conservatively
as partial when a source was observed, otherwise unavailable; never rewritten in
historical snapshots. Server-first coordinated local deployment is required before
the new client emits this optional field to strict older servers.

Native capture downgrades completeness when invalid entries are omitted or the
32-result cap applies. A failed/nonblocking capture remains unavailable. Current
actor and linked NPC metadata carry completeness alongside factions. Prompt caps
(16 for player/selected NPC,8 for bystanders) also downgrade complete to partial.
Neither incomplete membership nor a base-only empty list means an NPC has no
runtime affiliations. This field is not companion affinity.


## Selected NPC detail capture (2026-09-05)

Native text, voice, activation, bored/combat triggers and NPC PipVision requests
resolve their subject from the shallow immutable snapshot, then enrich only that
selected NPC before worker admission. Process-list discovery no longer chooses a
nearest NPC for deep inventory/faction/package capture. Background context-only
publication and diary/profile refresh snapshots remain shallow for NPCs.

The adapter requires the current runtime lane, generation and capture frame. It
checks live reference/plugin/playthrough and base identity, then merges inventory,
faction and package evidence only. Original positions, names, player/world facts,
other actors and pose timestamps remain unchanged. Missing actors, changed identity
or invalidated frames reject the request; lock contention preserves unavailable
observation quality instead of selecting another NPC. No worker performs enrichment.
Replies, actions and rechat retain the original enriched snapshot; an action aimed
at another NPC still cannot claim unobserved inventory knowledge.

Flat player deep capture remains disabled. Existing VR player capture is unchanged.
This closes selected-NPC enrichment ownership in source, not actual game timing or
runtime acceptance. Process-list scan bounds and per-property availability still
need separate work. No protocol schema/version or server change is required.

## Nearby reference observation quality (2026-09-05)

Both context lanes accept optional nearby_items_observation and
points_of_interest_observation: complete, partial, cached or unavailable. Omitted
legacy quality means partial for nonempty rows and unavailable for empty rows.
Unavailable requires an empty array, but never means there are no nearby objects.

Native completeness covers supported object types in the player's current parent
cell within 4096 game units of the flat camera or VR HMD observer. It is not a scan
of adjacent exterior cells or a line-of-sight guarantee. Each scan visits at most
512 cell-reference entries under a nonblocking try-lock, then processes copied
references on the game thread. Output retains at most 32 items and 16 points of
interest. Scan/output caps or unrepresentable candidates mark the affected lists
partial. Missing cell/identity, bootstrap or lock failure is unavailable. Flat
non-dialogue snapshots explicitly mark retained observations cached, including old
distances/looking-at values; moving cells invalidates that cache.

Saved source payloads remain immutable. Both dialogue and trigger prompt paths
carry quality; their 16-item/12-point caps downgrade complete lists to partial.
Derived event metadata and human-readable summaries retain quality. No historical
event backfill is performed. inspect_surroundings carries the two quality fields
and downgrades completeness at its row/byte budget; successful actor information
does not imply fresh or complete object knowledge. Cached rows remain labeled
cached, not current. Existing per-object weight/value/count/lock estimates and
actual flat/VR capture timing still need separate semantic/gameplay acceptance.


## Active quest observation quality (2026-09-05)

Both context lanes accept optional active_quests_observation: complete, partial,
cached or unavailable. Unavailable requires an empty array but means unknown, not
no quests. Legacy nonempty arrays are partial; legacy empty arrays are unavailable.
Flat dialogue captures use a nonblocking try-lock, at most 128 target entries and
256 objective instances, retaining at most 16 quests. Other flat snapshots label
previously captured quest data cached. Bootstrap and failed lock acquisition are unavailable.

Each quest optionally declares objectives_observation: complete, partial or unavailable.
Unavailable requires zero/empty wire placeholders; prompts expose its count as null.
Objective capture retains at most eight texts per quest and marks bounded or missing
text partial. VR's pinned headers cannot attest displayed objective state/text or
complete non-target discovery: quest lists are partial and objectives unavailable.

Only a complete fresh list replaces the playthrough quest projection. Partial lists
update observed quest identities only; cached, unavailable and legacy empty lists do
not modify it. Unobserved projection rows can therefore remain historical. Request
prompts use the exact immutable saved snapshot and its quality, never that projection.
read_quests refuses cached/unavailable observations as a fresh read. Its 1024-byte
result budget downgrades truncated lists to partial. These limits do not establish
measured game-thread timing or flat/VR gameplay acceptance.


## Inventory observation quality (2026-09-05)

Both protocol lanes accept optional actor-state `inventory_observation`:
`complete`, `partial`, or `unavailable`. An explicit unavailable observation requires
an empty inventory array. New clients always send the field. Older payloads remain
valid and retain their original saved bytes: during prompt construction, missing
quality means partial for nonempty data and unavailable for empty data, never complete.
The server downgrades complete to partial whenever prompt filtering removes an item.

Capture is still game-thread-only with a nonblocking inventory try-lock. Skipped
capture, a missing inventory list or lock contention is unavailable, not observed empty.
A full successful scan may attest complete, including an actually observed empty list.
The scan accepts at most32 items, inspects at most128 entries and64 linked stacks per
entry; truncation, an incomplete stack count or omitted unrepresentable data is partial.
It does not enable deep player capture or add blocking retries.

`check_inventory` returns unavailable when capture is unavailable. Otherwise its
JSON detail carries inventory_observation and truncated. The1024-byte response budget
also downgrades completeness, including when no item fits. Complete-empty and
partial-empty are distinct. Correlated actor, generation, request and result handling
are unchanged. Install the paired server support before the updated client.
This quality contract does not prove gameplay observation, physical visibility or
safety of untested game/runtime combinations.


This is the human-readable companion to the closed schemas, golden fixtures, and synchronized
SHA-256 manifest under `protocol/v1` in SYNTH and Synthserver.

## Transport

- Default base: `http://127.0.0.1:8087/Synthserver/`.
- Primary request endpoint: `main.php` until a versioned router replaces it.
- JSON request/response; NDJSON for streamed response events.
- Loopback only by default. Remote hosts require explicit opt-in, HTTPS, and a documented trust
  model.
- Connect, first-byte, idle, and overall deadlines; cancellation closes the request.
- Maximum request/response/line sizes and JSON depth/string/array limits.

## Common envelope

```json
{
  "schema": "synth.event.v1",
  "protocol_version": 1,
  "session_id": "runtime:vr:1234:5678:1",
  "request_id": "uuid",
  "turn_id": "uuid",
  "generation": 1,
  "game": "fo4",
  "runtime_variant": "vr",
  "client_version": "0.1.0",
  "runtime_version": "1.2.72",
  "capabilities": ["dialogue.text", "runtime.hmd_pose"],
  "type": "init",
  "payload": {"locale": "en-US", "hmd_pose_fresh": true}
}
```

Required rules:

- Session, request, turn, action and utterance IDs are bounded canonical strings; generation is an integer from 1 through 9007199254740991.
- A fresh `session_id` identifies one plugin-process lane. Request replay is scoped to it, and a restarted DLL cannot collide with an older lane.
- `game` remains `fo4`; `runtime_variant` is exactly `flat` or `vr` and is echoed on every response.
- Server echoes request, turn, generation, and protocol version.
- Unknown schemas/types/actions are rejected with a structured error.
- The client accepts only responses for an outstanding request in the current generation.
- Actor authority uses the current reference's full form ID plus originating plugin and current
  snapshot verification. Each actor state also carries the base actor/NPC full form ID and its
  originating plugin, allowing Synthserver to apply stable Fallout 4 creature/profile mappings
  without trusting display names or load-order-dependent local IDs.
- Text fields are untrusted data, bounded, UTF-8 validated, and never interpreted as code/path.
- Init advertises exact executable/script-extender/adapter versions plus typed input, presentation,
  context, action, and optional VR capabilities. The server filters behavior from the manifest; it
  never infers that VR supports a flat-only action/hook.
- Optional compatibility evidence uses a sanitized mod-profile fingerprint and bounded plugin
  identity list. It never includes filesystem paths, usernames, saves, or arbitrary local files.

## Event catalog

- `init` negotiates the exact runtime lane and capabilities and carries the bounded active plugin
  manifest with full/light indices and form-ID prefixes, never plugin filesystem paths.
- `context` commits canonical player, target and bounded audience identity plus bounded nearby
  actor position/live/combat/hostility/sneaking/level, inventory, faction and activity state before
  a turn. Activity includes life/posture, weapon, movement/sprint, player-dialogue, power-armor and
  canonical current-package evidence without exposing engine pointers.
  Faction entries carry full form ID, originating plugin, name/editor ID and rank. The required
  `faction_observation` value is `effective` when runtime changes were enumerated, `base_only` when
  only active base memberships were observable, or `unavailable`; incomplete observation is never
  presented as companion affinity. Its `world` object carries native Fallout location, cell,
  worldspace, weather, interior state and game time.
- `activate` persists a manual or automatic active-agent registration.
- `trigger` exposes canonical-actor `auto_greeting`, `bored`, `combat_bark` and `rechat`. Auto greetings carry
  native Fallout game-time ticks and are re-authorized against persisted NPC/profile settings,
  last interaction and cooldown on the server. Bored events use the configured wall-clock idle
  deadline; rechat is dispatched after response, speech, prepared media, playback and action
  queues drain.
- `input_text` and `input_audio` enter the real dialogue pipeline.
- `action_result` commits one idempotent terminal result and may drive a result-aware continuation.
- `cancel` cancels one request; `halt` closes the lane and cancels dialogue-related background work.
  After the halt is acknowledged and all local queues drain, the plugin creates a fresh opaque
  runtime session before accepting new dialogue.

## Response stream

NDJSON lines are one of:

- `response_start`: accepted request metadata;
- `dialogue`: speaker, listener/audience, text, emotion, optional audio reference;
- `action`: allowlisted typed action and parameters;
- `status`: non-terminal progress/diagnostic;
- `response_end`: terminal success/cancelled/error and unfinished/rechat state.

Ordering is per request. Duplicate line IDs are idempotent. An action is never inferred from prose.

## Media transport

JSON carries only closed `synth.media.v1` references. Audio, microphone recordings, and images are
bounded native HTTP request or response bodies and are never Base64-encoded in JSON. Media IDs are
opaque content-addressed keys; server filesystem paths never cross the wire. The client verifies
content type, byte count, SHA-256, same-origin routing, deadlines, cancellation and generation
before dispatching playback or further processing.

## Versioning

Backward-compatible additions may use optional fields inside v1. Breaking semantics require v2.
Both sides advertise supported schemas/capabilities during init. Flat and VR share v1 event
semantics but carry their runtime variant and actual capabilities. A runtime-specific capability
addition can remain optional inside v1; changing shared action meaning still requires v2. No
long-lived dialectic-schema alias is planned; migration fixtures are implementation aids only.

## Native PipVision capture ownership (2026-09-05)

Native v2 sessions use `synth.visual_context.capture.v2` with the acknowledged
`context_sequence` and the same `turn_id` as the capture's immutable context.
The client publishes that snapshot before image processing and passes the returned
`capture_id` in its v2 `external_reaction` payload. `synth.visual_context.response.v1`
remains the closed capture-result envelope; its request/turn/generation/capture ID
must all match. Ordinary triggers omit `capture_id`; v1 triggers reject it.

Ingress checks initialized protocol/lane/generation, completed non-cancelled context,
frozen subject and nearby actors, matching observed world values, and supersession.
The v2 world FormID fields stay empty because the snapshot does not attest those IDs.
Completion locks the session and exact capture claim, rechecks ownership, then commits
the description or portrait metadata without holding a transaction during image/model work.

An explicit reaction reads only its successful capture with matching actor, session,
generation, turn and sequence; missing/mismatched captures never select a gallery fallback.
Current-generation ambient descriptions require the same session/generation/actor and
a non-future v2 snapshot. Accepted descriptions are also recorded as typed visual results
in the ordered native history frontier. Ancestral observations require exact saved-context
visibility, actor/playthrough/lane ownership and matching accepted result bytes. They read
the original description, not a later gallery edit. Legacy captures are never backfilled.

After a successful image/reaction chain, the client acknowledges another immutable context
to advance the co-save frontier past completion. Pending/failed acknowledgement is reported
without undoing the accepted reply. Capture work registers its context with the independent
turn-cancel lane before image processing. A cancelled anchor blocks late server completion.

Gallery active/delete/lock controls remain respected; locking only extends scene TTL, never
ancestry visibility. Current-generation edited descriptions remain supported. Versioned
manual visual-knowledge edits across saved timelines are still unfinished parity work.
