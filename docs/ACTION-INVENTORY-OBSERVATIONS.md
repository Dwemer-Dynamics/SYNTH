# Post-action inventory observation work

Status: native producer, receipt/acceptance, client forwarding, mixed action/rechat
inventory views, explicit readiness and old-server equipment execution gate implemented.
Synthetic full-main/model and mixed cancellation/concurrency/replay checks pass.
Matched flat client 5b81452 and compatibility server 092e5ad are locally deployed.
ENABLED FOR FLAT / NOT GAMEPLAY VERIFIED. Actual model behavior and gameplay remain open.
The compatibility pipeline adds 200 passing checks on the old schema; see the goal ledger
for exact artifacts, preserved pending embedding scope, backups and manual acceptance.

## Reproduced source path

- Client PluginSession::execute_action retains pending.snapshot and passes it unchanged to
  handle_line for every action-result continuation. CheckInventory reads that original view.
- Server synth_protocol_current_context selects the immutable original context_sequence.
  synth_protocol_bind_action_result rebuilds a successful CheckInventory detail from that view.
- Native equipment changes invalidate the runtime capture cache, but neither of those paths
  performs a new capture before following the action. Invalidating the cache alone is insufficient.

## Implemented native producer

RuntimeActionResult now optionally owns a RuntimeInventoryObservation: action ID, generation,
canonical actor/playthrough, copied inventory rows and complete/partial/unavailable quality.
It is engine-free and survives the game-thread/worker completion boundary by value.

The flat equipment adapter captures the entire inventory after entering the equipment operation,
including rejection, failed verification and caught exceptions. This also captures side effects
such as equipping a rifle automatically unequipping a pistol. Already-equipped verified success
also attaches an observation. A capture failure preserves the execution result and supplies an
explicit empty unavailable observation. Cancellation/load epoch changes suppress the read.
This reuses the existing bounded inventory traversal and metadata admission budget.

valid_for checks the exact action, actor, plugin, playthrough and generation plus cancellation.
It does not itself establish network request/turn ownership: the correlated action completion
and eventual validated terminal receipt must provide that authority. No engine pointers escape.

## Remaining integration contract

1. IMPLEMENTED: additive v2 action-result payload supporting a tagged result with an inventory
   observation. Preserve the v1 schemas/old receipt shape. Require the new negotiated capability
   action.inventory_observation; enforce v2, lane and the existing inventory row-size rules.
   Mirror schemas/fixtures/manifests in both repos and update the native codec, Python verifier,
   fake client/server and PHP synth_ingress validator together.
2. IMPLEMENTED: carry the observation from RuntimeActionCompletion into send_action_result. Validate it
   against the dispatched request. Do not truncate it into the existing 1024-character detail.
   Missing metadata after an issued mutation must not revive the original inventory.
3. IMPLEMENTED: synth_protocol_persist_action_result validates the observation against the recorded
   action actor, generation, originating request/context and turn before accepting it. Preserve
   the existing terminal-result idempotence check: conflicting repeated observations are conflicts,
   not last-writer-wins updates. Prefer retaining the observation inside the existing result JSON;
   do not add a database migration unless an actual missing invariant requires one.
4. IMPLEMENTED (source): build an ephemeral response view from the original context plus accepted observations in
   this exact action ancestry. Walk recorded request -> parent action_result -> intent links,
   not latest/global inventory. Enforce bounded depth, no cycles, exact session/generation/turn/
   context, accepted terminal records and canonical actors. Newest observation per actor wins.
   An unavailable newer observation clears earlier rows; never fall back to a stale inventory.
   Malformed/exhausted ancestry must fail closed, not silently expose the original state.
5. IMPLEMENTED (source): use that same response view for prompt inventory, CheckInventory, action continuation
   selection and client continuation handling. Keep original context_snapshots and all
   non-inventory facts immutable. A subsequent independently captured turn remains a new
   observation, not a rewrite of this action chain.
6. Validate the complete chain: equip changes rifle/pistol state -> receipt -> fresh follow-up
   prompt/CheckInventory -> unequip, including same-name actors, duplicate names, split stacks,
   partial/unavailable reads, wrong owner, future/foreign context, missing parent, cycles,
   duplicate conflicting receipts, cancellation and save/load. Verify both original snapshots
   and original inventory rows remain byte-identical.
7. Deploy the matched producer/codec/server consumer only after these gates pass. Use the
   compatibility server branch based on the installed runtime to avoid accidentally deploying
   the pending embedding changes. Then request manual flat equipment/dialogue/save-load tests.
   VR equipment remains disabled until its independent safe instance-access path is implemented.

## Evidence boundary

Full-main checkpoint (2026-09-08), server 3c50c06: 45 checks against the actual
main/profile/prompt/connector/receipt/rechat code with an isolated PostgreSQL DB
and deterministic loopback model. This exposed and fixed equipment tool history
substituting the NPC name for the item and CheckInventory resolving filters as
NPC targets. Stored item/filter semantics now survive the model round trip.
Captured model requests prove fresh equipment, per-actor reads through rechat and
unavailable clearing; original snapshots are unchanged. Follow-up opt-outs remain
unchanged; only the fixture enables equipment follow-ups. TTS is disabled in this
probe. No game/installed runtime acceptance follows from it. See the goal ledger
for the retained fixture paths, exact run and remaining gates.

Readiness checkpoint (2026-09-08), client 6abe6c4 / server 4fdfe96: opted-in v2
flat init receives init_action_inventory_accepted. Only a complete correlated
three-line ACK grants client readiness; new initialization clears it and halt
disables it. PluginSession forwarding uses readiness, not wire selection alone.
727 transport checks and PHP 298/4974 pass; both builds/17 suites and fresh
scratch package audits pass. The capability is still not advertised or deployed.

Mixed ancestry checkpoint (2026-09-08), server 5e04d9c: completed rechat evidence
is read under the real current owner lock. Historical nodes additionally require
reserved/prepared selections, matching parent/line/body receipts and historical
speaker-role proof. The root's read-only candidate view works before reservation;
it does not authorize generation. A shared 64-edge / 8 MiB limit includes action
and rechat edges; body evidence is charged conservatively when read twice.
Real PostgreSQL mixed probe: 41 checks; ordinary inventory: 63; completed-parent:
37, all rolled back. PHP 298/4958 and both native builds/17 suites pass. Actual
coordinator, prompt and CheckInventory paths preserve independent NPC observations
and unavailable tombstones, including after budget exhaustion. No provider/game
acceptance or deployment. Client product cb87acd and its DLL hashes are unchanged.

Consumer checkpoint (2026-09-08): PluginSession forwards only exact validated native
observations when the unadvertised wire capability is selected. Missing/mismatched
or unencodable observations become empty/unavailable without changing action status.
The receipt and copied continuation use identical rows, including negotiated trimming.
Only inventory/quality changes; actor names, bases, poses, world facts and capture time
remain the original scene. CheckInventory and subsequent equipment selectors use it.

Server admission now explicitly reads synth_protocol_original_context. Response
consumers use synth_protocol_current_context, applying only accepted terminal
observations from exact request/intent edges. The walk is bounded to 64 actions and
8 MiB of decoded ancestry; cycles, corrupt/missing/cancelled owners fail closed.
Newest per-actor observations win, including an unavailable tombstone for a legacy
mutation receipt missing inventory. SQL reads do not update original snapshots.

Checks: 91 native action checks; 298 selected PHPUnit tests / 4954 assertions.
The rollback-only action-inventory-view-probe.php passes 63 real PostgreSQL checks:
equip -> actual prompt snapshot / CheckInventory binder -> unequip -> unavailable,
with missing/cancelled/cyclic/foreign ancestry rejection and byte-identical originals.
Three-edge reads averaged 7.267 ms over ten reads in this synthetic database only;
this is not large-history, full-main/provider, concurrency or game timing proof.

The previously traced rechat gap is source-fixed above. PluginSession's queue_rechat
retains the response view and server consumers now cross proven parent edges. No
latest inventory lookup or request-global substitution is used. The plugin advertises
dialogue.rechat.ownership; old prose elsewhere saying rechat is disabled is historical.
Explicit action-inventory readiness is implemented above. Complete full-main/model,
old-server equipment and concurrency tests before advertising action.inventory_observation
or deploying the matched compatibility delta.

Wire/acceptance checks (2026-09-08): all 17 native suites in flat and VR builds;
31 schemas, 147 fixture cases and 166 identical peer protocol files; 297 selected
PHPUnit tests / 4921 assertions. A rollback-only PostgreSQL probe passes 45 checks
against synth_parity_context_20260904, including wrong actor/plugin/playthrough,
turn/context/generation/key/request, parent capabilities, retired intents, runtime
retirement/generation/turn/context/lane, duplicate conflicts and byte-identical
original context JSON. Scratch evidence: `<scratch>/`
action-inventory-receipt-probe.php. No live game database or provider was used.

Producer unit/build proof does not establish fresh AI replies, actual native capture timing,
gameplay equipment behavior, long-session stability or VR headset acceptance. Do not advertise
the new capability or deploy this producer alone. Full Dialectic parity remains the objective.
