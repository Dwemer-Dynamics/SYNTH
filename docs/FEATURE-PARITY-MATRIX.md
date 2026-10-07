# SYNTH feature parity matrix

## CHIM queue parity (2026-09-13)

IMPLEMENTED / MANUAL ACCEPTANCE OPEN: one bounded speculative WAV is prepared on
the existing speech lane before parent delivery, then reused only after complete
response and fresh scene admission. Menu capture handoffs survive pause; stale
pre-dispatch captures have two retries. Terminal rechat policy reasons are visible
in bounded diagnostics. Ordered playback, action ownership and save-load guards
are retained, not replaced. See [the current contract and acceptance matrix](CONVERSATION-PARITY-GOAL.md#chim-queue-parity-checkpoint-2026-09-13).
No in-game latency improvement or full behavioral parity is claimed from builds.

## Flat package capture lifetime (2026-09-08)

Client `f2886c1` pins the effective package under its nonblocking holder lock,
copies metadata after unlock, then releases with verified flat native lifetime
rules. The old source reproduces heap-use-after-free in the isolated retirement
probe; fixed source passes245 ASan/UBSan checks. Exact executable anchors,
flat17/VR17 suites, build37, packaging11, protocol32/188/200 and both package
audits pass. Flat client deployed with matching SHA256
`6AC34B2D415B328B45D4023203D70580306E125C09C6A99AD31D5294769A55F3`.
Both INIs and Papyrus files preserved; no server deployment, healthHTTP200.
VR lifetime behavior is not verified by flat offsets or its7 baseline probe
checks. PickupItem remains disabled; native approach/transfer/restoration and
matched receipts still need completion. No gameplay freeze-cause or stability
claim. See [evidence and next integration](PICKUP-ACTION.md) and
[deployment ledger](HANDOFF.md#retired-ledgers). Full goal remains active.

## PickupItem exact-reference preparation (2026-09-08)

Source-only client435e5b7 binds the exact NPC, world reference and full observed
stack to its immutable turn. The postcondition requires positive reference
retirement and exact named inventory conservation. No capability or dispatch
enabled. Actions415 including ASan/UBSan, flat17/VR17, build36, packaging11,
both package audits and unchanged protocol32/188/200 pass. Exact flat binary
audit confirms native transfer, possible weapon auto-equip and SetDelete(true);
package ownership/restoration and native execution remain unproven.
See [pickup integration requirements](PICKUP-ACTION.md). No deploy: installed
b6d29a6 remains unchanged. Full parity and all manual acceptance remain open;
five agreed TTS providers only.

## Flat world-item metadata locally installed (2026-09-08)

Client `b6d29a6` captures actual stack counts and instance metadata using one
non-blocking metadata lease, skipping the irrelevant item map-marker getter.
Invalid/busy metadata produces partial observations. Flat 17/VR 17 suites,
build gates 36, packaging 11, exact-runtime audit and ASan/UBSan probe 3,592 pass.
Both packages audited; installed DLL matches
`C49BB67206EEBA6634F03827B50165E5BF108578D4D1258E139E265ECD823BC3`.
Custom INI/PEX unchanged; server untouched and read-only health HTTP 200.
See [capture evidence](WORLD-ITEM-CAPTURE.md) and [deployment ledger](HANDOFF.md#retired-ledgers).
This is a PickupItem prerequisite, not action implementation or gameplay proof.
VR count/instance capture, other blocking nearby getters and manual flat/VR
acceptance remain open. Full parity stays active; five TTS providers only.

## Player caps payment locally installed (2026-09-08)

Client08f5d2d and compatible servercd59050/c020de9 now provide the matched flat
payment path: distinct ACK, guarded dispatch, player-donor/NPC-receiver receipt
and immutable follow-up. Flat17/VR17 suites, fake-server1306, compatible PHP304/
5744, payment41,12 invalid modes23 each and cancel/reinit/replay races pass.
Installed DLL0F722E88CCCB260FCEA4CB192CE1B24F2D6F9CDC034A74D23B7776040DD9D86E;
19 server files and mirrored protocol32/188/200 verified. Custom INI/PEX unchanged.
See [deployment and backup evidence](HANDOFF.md#retired-ledgers). Manual payment,
save/reload, sustained stability and independent VR acceptance remain open.
Full parity stays active; only the five agreed TTS providers are in scope.

## Player caps payment server contract (2026-09-08)

Maintained source only: server378c7e1 and client protocol bb37542 implement the
separate payment contract. NPC performer/player target map to player donor/NPC
receipt recipient; strict player funds, immutable ancestry and recorded amount
survive follow-up. PHP308/5831, actual-main payment41,12 invalid modes23 each,
cancel/reinit35 each and duplicate/conflict40 pass. Flat17/VR17 suites and mirrored
protocol32/188/200 pass. See [full evidence](HANDOFF.md#retired-ledgers).
Client payment readiness/dispatch and compatible server port remain pending;
nothing new deployed or game-tested. Full parity stays active. Five TTS providers only.

## TakeCapsFromPlayer native producer only (2026-09-08)

Client4ccb308 on codex/dialectic-parity-core adds exact player-to-NPC caps
preparation and paired continuations. The dialogue performer remains the NPC;
the native donor and first inventory receipt are the exact captured player.
The receiving NPC owns the second receipt. Strict amount/currency identity,
same-scene actor ownership, original snapshot immutability, conservation,
cancellation and independent observation quality are preserved.

Source/build/package proof: actions341, all17 flat suites, VR build/CTest17/17,
build gates36, packaging11 and unchanged protocol32/180/194 pass. Read-only
base currency/EXE/Address Library audit passes. Native engine execution is unproven.
No capability advertisement, PluginSession dispatch, server contract or deployment
for TakeCapsFromPlayer yet; those are the next matched milestone. The installed
GiveCapsTo candidate remains unchanged. Manual payment, save-load, voice and
independent VR acceptance remain open. No game control or additional TTS work.

## Matched caps and read-only rechat update locally installed (2026-09-08)

Client fdbffe4 on codex/dialectic-parity-core advertises caps only in flat and
requires exact init_caps_inventory_accepted before dispatch/paired receipts.
Maintained server6ecf670 closes the item-transfer rechat catalog bypass at lookup,
mapping and emission. Compatible codex/equipment-local-20260908 selectively carries
ee834a4/82a8a16 (checkpoint7c54684), without pending embedding changes.

Flat/all17 and VR/CTest17/17, build gates36, packaging11, maintained PHP306/5641,
compatible302/5554 and both release audits pass. Real main.php with isolated DB
and synthetic model passes item/caps NPC52/player51, twelve invalid caps modes23
each, and cancellation/reinit35 plus duplicate/conflict40 for each recipient.
Protocol32/180/194 matches maintained, compatible and installed trees.

Matched flat DLL7175F1AC883CBF8E5B3AD6B58A7A092220D0719D151152436B57F95B0410F6ED
and21 selectively checked server PHP/protocol files are locally installed.
Health200 and exact correlated caps init ACK pass. Custom INI, original PEX,
configuration, live schema, saves, voices and unrelated work are preserved.
Backups and detailed proof are in the retired parity ledger ([handoff](HANDOFF.md#retired-ledgers)).

Manual save-load, caps conservation, audible responses, cancellation/recovery
and independent VR acceptance remain open. VR caps stay disabled. No game was
controlled, no real provider was called, and no push/PR/release occurred.
Only XTTS, Chatterbox, PocketTTS, Cartesia and Inworld are in TTS scope.
Full parity remains active; older checkpoints below describe historical states.

## Caps server contract verified; client gate pending (2026-09-08)

Maintained server PHP95cc124 and client protocol513059f on codex/dialectic-parity-core.
Strict typed caps issuance, exact observed currency, separate ACK, paired receipts
and recorded amount/recipient follow-ups pass305 PHP tests/5607 assertions and
isolated actual-main NPC51/player50 checks plus twelve invalid modes23 each.
Flat/VR builds and17 suites pass; protocol32 schemas/180 cases/194 files,
build/packaging gates and both package audits pass. Caps remains unadvertised:
client readiness/dispatch and compatibility/matched deployment proof are pending.
Existing GiveItemTo read-only rechat catalog bypass is the next scoped fix.
Installed client2a20aaa and compatible server0150b04 remain unchanged.
See [goal ledger](HANDOFF.md#retired-ledgers); no gameplay or VR acceptance claimed.

## Flat caps transfer producer (2026-09-08)

Client c620ba5 adds typed GiveCapsTo preparation, exact observed currency selection
and guarded native two-inventory transfer reuse. The exact executable audit corrected
stale CommonLib singleton signature and initialization-array assumptions before use.
Flat/VR builds, all17 suites, actions274, protocol and package gates pass.
Source only: no wire/server negotiation or PluginSession dispatch yet, no capability
advertised, no deployment. VR remains unsupported. Installed speech-cache/transfer
builds are unchanged. See [goal ledger](HANDOFF.md#retired-ledgers) for next steps.

## Speech cache recovery locally deployed (2026-09-08)

Compatible server0150b04 (codex/equipment-local-20260908), maintained producta69be38
(codex/dialectic-parity-core). Invalid WAV caches no longer suppress generation in
the durable worker or the five supported adapters; valid audio remains reusable.
PHP suites302/5397 and298/5310 pass, plus25 isolated actual-adapter/real-ffmpeg
recovery checks with synthetic transports only. Seven server files hash-verified,
healthHTTP200; no cache/config/database cleanup or native DLL change.
This is not evidence that the earlier game freezes are resolved. Manual flat/VR
acceptance and full parity remain open. See [goal ledger](HANDOFF.md#retired-ledgers).

## Matched transfers locally deployed; manual acceptance open (2026-09-08)

Flat client 2a20aaa on codex/dialectic-parity-core and compatible Synthserver
product dd9a22c / deployed source 0c43aac on codex/equipment-local-20260908
are installed locally. Maintained server product189c6f4 remains on
codex/dialectic-parity-core; its pending embedding work was not deployed.

- Flat transfer advertisement requires a separate owned transfer acknowledgement.
  Impossible paired results cancel only their own turn/context, never a replacement.
- Flat native suites and VR CTest17/17 pass; build gates36, packaging11,
  protocol32 schemas/172 fixtures/188 mirrored files and both package audits pass.
- Installed DLL SHA256:
  2092EEFD9E747DDAFD035C0496D7EC0C553C9C0631F995C8173D0907EB05FDE1.
  All28 selectively deployed server hashes match; healthHTTP200 and actual runtime
  init_transfer_inventory_accepted verified. Configuration and bridge unchanged.
- No game was launched or controlled. Transfer conservation, inventory follow-up,
  sustained dialogue and save/load responsiveness still require the user's manual
  flat test. VR transfer stays unsupported; independent headset acceptance remains.
- Full evidence, backups and remaining work: [goal ledger](HANDOFF.md#retired-ledgers).
  Full parity is not complete. TTS only: XTTS, Chatterbox, PocketTTS, Cartesia, Inworld.

## Transfer request selection and paired local continuation (2026-09-08)

SOURCE VERIFIED / UNADVERTISED / NOT DEPLOYED.
Client product 5decb24da343f244d9d75266eb91cd94df3132c9 on codex/dialectic-parity-core.
Server product remains 5398ba4; no server code, schema or runtime change this milestone.

- Added transfer_actors, prepare_transfer_action and prepare_transfer_inventory.
  Only a flat, uncancelled captured scene with the exact generation, NPC donor and
  distinct captured NPC/player recipient is accepted. Duplicate FormIDs, conflicting
  plugin/save identity, absent actors and name-only substitution fail closed.
- Request preparation accepts only item plus optional integer amount (default1).
  Exact existing item selection rejects ambiguous names/stacks, equipped items,
  unknown inventory and quantities outside1..1000000 or above the observed count.
  Partial inventory requires an explicit observed FormID. No rounding of doubles,
  strings, booleans or null. Action ID, token and deadline are carried to the native
  request; live actor/stack/deadline checks remain in the native execution guard.
- Paired preparation uses the existing NPC/player inventory-only copy APIs.
  Original scene identity, pose, timestamps, generation and other actors remain
  unchanged. Both local views contain exactly the serialized receipt rows and
  independent quality. Mismatched pair ownership makes both unknown; invalid wire
  values or malformed native UTF-8 clear only the affected side to unavailable.
- Shared action_inventory_value validation prevents divergent row interpretation.
  Per-list32/512 limits remain; the paired JSON gets960KiB, reserving64KiB for the
  bounded envelope. Byte overflow halves the larger list, at most20 reductions;
  affected quality becomes partial. No rows are silently removed after preparation.
- Both builds and all17 suites pass: actions243, adapters411, fake-server832;
  protocol32 schemas/168 cases/186 matching peer files, build gates36, packaging11.
  Tests include exact whole-row equality, NPC/player recipients, immutable originals,
  duplicate identities, VR/stale/cancelled owners, one-million quantity boundary,
  malformed strings and two worst-case escaped512-row observations with a large envelope.
  Audited transfer-preparation-flat.zip and transfer-preparation-vr.zip in scratch.
  Uninstalled flat SHA57CC038868FBC124D4F5E0BE698A9E55657018ADBD0C285CC50F4E1E1FDFBD0D;
  uninstalled VR SHA0E2C6271E3A68E4488B8738D6AA83D384F00D137AD50FA4DCD77C9E4EE173808.
  Installed client SHA remains8052167338934999DB4A20443819F0553CEFCB0FAB85024843D767AD2DAA9881.
- NEXT: wire these prepared requests/continuations into PluginSession and SynthClient,
  add separately acknowledged transfer readiness and strict server item/amount emission.
  Then prove actual-main mixed transfer/rechat cancellation/replay/save-generation on
  maintained and compatible schemas before advertisement/deployment. The new helpers
  are not yet called by live dispatch. Prior server/database proof is in the next entry;
  PHP and PostgreSQL tests were not rerun for this client-only source milestone.
  Full roadmap, manual game transfer/stability and independent headset acceptance stay
  open. Five TTS providers only. No game/editor/headset, shared-service, MiniMe, deploy,
  push, PR, release, version bump or goal-completion action.

## Paired transfer receipt and owned server view (2026-09-08)

SOURCE VERIFIED / UNADVERTISED / NOT DEPLOYED.
Client product 68eebf5f3c24a4c1cbd017f6d9fc02dc6ccd9e25 and maintained server product
5398ba4e08d9633678339508c8ca48ffd0f731af, both on codex/dialectic-parity-core.

- Added synth.action.transfer-result.v2 with required donor inventory and
  recipient_inventory; independent quality and row validation, distinct FormIDs,
  one playthrough, flat v2 action.transfer_inventory + action.give_item_to +
  action.inventory_observation + dialogue.turn_ownership. Each list is at most32,
  or512 with context.inventory_512. Encoder preserves the 1MiB ingress byte limit
  including its newline; no silent post-preparation truncation.
- Server binds both actors to existing action_intents actor/target IDs, parent
  request, context, turn, generation and capabilities. No migration. Identical
  terminal replay is idempotent; recipient changes conflict. Single-actor v2
  receipts cannot substitute for transfers. Missing legacy evidence invalidates
  both inventory views; independent unavailable evidence clears only that actor.
  Newest accepted observations win per actor without rewriting original snapshots.
- Both builds and all17 flat/VR suites pass (actions154, fake-server832,
  adapters411); native paired receipt tests cover3 quality cases, two512-row
  lists and20 negative cases. PHP300 tests/5282 assertions. Build gates36,
  packaging11. Protocol32 schemas/168 cases/186 matching peer files.
- Rollback-only PostgreSQL probe transfer-receipt-probe.php passes87 checks
  independently for player and NPC recipients in synth_parity_context_20260904:
  exact intent/target ownership, stale/cancelled owners, duplicate/conflict,
  both prompt/context views, donor CheckInventory, independent failed/missing
  observations and immutable snapshots. Fixture sessions rolled back.
  Existing equipment view63 and mixed action/rechat41 checks also pass.
  These are synthetic receipts; no native game action or transfer full-main proof.
- Server release audit:781 files/429 PHP/180 JSON/12 Herika-style directories/
  3 catalog checksums. Retained17-driver inventory does not expand TTS scope.
  Audited transfer-receipt-flat.zip and transfer-receipt-vr.zip in scratch.
  Uninstalled flat SHA F6734A610975CFFDFCB50DFC3293E79049193E07B21D91AC53A2F8655F7D403B;
  uninstalled VR SHA 84D555490F1CC9FF63691E738249F836F415C6AF6F9248A352EE69ECB7CF5116.
- Installed client ee8a192 SHA remains
  8052167338934999DB4A20443819F0553CEFCB0FAB85024843D767AD2DAA9881.
  Installed server's ingress/state/inventory-view hashes still match compatible
  ddda7d4 source; compatible worktree bf718d3 is clean. Pending embeddings remain
  undeployed. No runtime, game/editor/headset, shared-service or MiniMe changes.
- NEXT: separately acknowledged transfer readiness, strict canonical recipient/
  item/amount emission and dispatch, paired local snapshot preparation (including
  player recipient) with identical wire rows and byte bounding, then actual-main
  mixed transfer/rechat cancellation/replay/save-generation tests on both maintained
  and installed-compatible schemas before advertisement and local deployment.
  Real transfer/attachment/ownership behavior, same-save stability, independent VR
  acceptance and the full parity roadmap remain open. Only XTTS, Chatterbox,
  PocketTTS, Cartesia and Inworld. No push/PR/release/version bump/goal completion.

## Native paired inventory transfer foundation (2026-09-08)

Client b0c4ee4 on codex/dialectic-parity-core adds guarded flat give_item with distinct
canonical actors, exact selected-stack deltas and paired whole-inventory observations.
SOURCE ONLY: not advertised, no wire receipt/server acceptance, not deployed; VR disabled.
Both builds/all17 suites pass; actions154, build gates36, packaging11, protocol31/157/174
and both release archives pass. Production execution with fake engine passes31 checks;
a bypassed postcondition fails as expected. Binary dispatch audit is not gameplay proof.
Installed client ee8a192/server ddda7d4 unchanged. Next: paired receipt contract and
mixed follow-up ownership for both actors. Full parity/gameplay acceptance remains open.
Only XTTS, Chatterbox, PocketTTS, Cartesia and Inworld are in TTS scope.


## Matched Consume deployed; gameplay acceptance remains open (2026-09-08)

Installed flat client ee8a192 (codex/dialectic-parity-core) and compatibility server
ddda7d4 (codex/equipment-local-20260908). Flat advertises both Consume capabilities;
execution still requires the explicit owned-inventory acknowledgement. VR stays disabled.
All 17 server files match source; installed init returns init_consume_inventory_accepted.
Installed DLL: 8052167338934999DB4A20443819F0553CEFCB0FAB85024843D767AD2DAA9881.
Both builds/17 suites, protocol 31/157/174, PHP 294/5029, 245 compatible full-main
pipeline checks and 33 installed-source rollback wire checks pass. Both ZIP audits pass.
Custom/default INIs and PSC/PEX unchanged; schema remains 20260907012. Pending embeddings,
unrelated runtime drift and shared MiniMe were not deployed/restarted.
Manual Consume effects, same-save/message stability and independent VR proof remain open.
See the retired parity ledger ([handoff](HANDOFF.md#retired-ledgers)) for artifacts, backup paths and evidence limits.
Only XTTS, Chatterbox, PocketTTS, Cartesia and Inworld remain in TTS scope.


## Consume client/server contract connected (2026-09-08)

Client 522a528 + maintained server de773ed, codex/dialectic-parity-core.
Source-only: distinct flat v2 Consume acknowledgement now gates native dispatch,
exact owned typed receipts and fresh per-actor inventory across action/rechat chains.
Fixed the real pipeline's item-as-actor-target drop; conflicting selectors and amount
fields are rejected. Actual client still advertises neither Consume capability.

200 deterministic full-main Consume checks pass across ordinary/cancel/reset/replay,
plus 45 existing equipment-chain checks and 33 rollback-only PostgreSQL emission checks.
Both builds/17 suites; 119 action and 832 fake-server checks; PHP 298/5116;
protocol 31/157/174; build/package/release audits pass. No game/provider acceptance.

NEXT: compatible server port and matched local deploy, then user-controlled gameplay.
Installed 5b81452/092e5ad unchanged; pending embeddings must not be full-synced.
Full parity and independent VR acceptance remain open. Five approved TTS providers only.

## Native flat Consume prepared, still disabled (2026-09-08)

Client 0ba9098 adds an unadvertised native consumption producer with exact ALCH
binding, synchronous engine-thread gating and verified one-item removal. It rejects
quest-alias/unsupported instances and retains truthful post-action inventory.
No new PluginSession route, capability, server contract or deployment.

Both builds/17 suites pass (113 action checks), with 35 build gates, 11 package tests,
protocol 31/150/167 and both ZIP audits. Exact binary audit: 22 instruction spans;
actual execution against fake engine: 23 checks plus two caught negative controls.

NEXT: Consume-specific acknowledged receipt/ownership and fresh-context support on
both client and server, then integration/replay/cancellation checks before enabling.
Existing inventory ACK alone is insufficient. Installed client 5b81452 and server
092e5ad remain unchanged. Gameplay and independent VR acceptance remain open.
Only XTTS, Chatterbox, PocketTTS, Cartesia and Inworld remain in TTS scope.

## Effective flat activity package captured (2026-09-08)

SOURCE / NOT DEPLOYED: client df6c6b5 now gives a non-null run-once AI package
precedence over the regular package, matching the exact flat 1.11.240 selector.
Busy or unidentifiable overrides report unavailable, never a guessed regular activity.
At most two nonblocking try-locks; no engine mutation or new runtime offsets.
VR behavior stays unchanged pending independent runtime proof.

Eight binary instruction spans plus the actual extracted capture function's 13 flat/
7 VR checks pass. Both builds/17 suites, 34 build gates, 11 package tests, protocol
31/150/167 and both package audits pass. Installed client remains 5b81452 unchanged.
Gameplay/package transitions and all movement/follow/action restoration gates remain open.

## Inventory observations enabled and deployed locally (2026-09-08)

Flat client 5b81452 (codex/dialectic-parity-core) + compatibility server 092e5ad
(codex/equipment-local-20260908) are installed together. All 18 changed runtime/
protocol files and the client DLL were hash-verified; actual installed init ACK
and health HTTP 200 pass. Full-server sync was avoided to preserve unrelated
differences and pending embeddings. Schema stays 20260907012; configs/PEX preserved.

Compatibility pipeline: 200 synthetic full-main checks across ordinary inventory,
cancel, generation reset and duplicate/replay. PHP 294/4948; both builds/17 suites;
765 fake-server checks; protocol 31/150/167; 34 build gates/11 package tests; audits pass.
Full details, exact artifacts and backups are in the goal ledger's current checkpoint.

ENABLED / LOCALLY DEPLOYED / NOT GAMEPLAY VERIFIED. User manual save-load/dialogue/
equipment test is next. Actual model judgment and independent VR acceptance remain
unproven. No game was launched or controlled. Five approved TTS providers only.

## Inventory cancellation, replay and compatibility gate (2026-09-08)

Client ff196b1 gates opted-in equip/unequip before native dispatch on the server's
inventory ACK. Old ACKs cannot allow a mutation with an unsupported observation
receipt. Legacy sessions and sheathing are unchanged; 765 fake-server checks pass.
Server product 3c50c06 is unchanged.

Full-main synthetic mixed inventory chain: cancellation 51 checks, generation reset
51, concurrent duplicate/completed replay 53. Cancel/reset returns promptly and
suppresses stale dialogue/actions; duplicate makes one model call and replays exact
completed lines. Original context remains unchanged. Both builds/17 suites, protocol
31/150/167, 34 build checks, 11 package tests and both package audits pass.

NOT ADVERTISED / NOT DEPLOYED. Next gate: matched compatibility-server delta without
pending embedding changes. Real model and manual flat/VR acceptance remain open.
No game operation; only the five approved TTS providers remain in scope.

## Full pipeline inventory item semantics (2026-09-08)

Server 3c50c06 fixes equipment item names in tool history and native CheckInventory
filters being mistaken for actor targets. Client 6abe6c4 is unchanged.
45 actual-main/profile/connector/receipt/rechat checks pass with a deterministic
loopback model: fresh per-actor inventory reaches emitted prompts; unavailable
does not revive stale rows. Equipment follow-up remains a configurable policy,
enabled only in the synthetic fixture for this test.

PHP 298/5035; both builds/17 suites; protocol 31/150/167; SQL 41 and 63; audits pass.
NOT ADVERTISED / NOT DEPLOYED. Mixed cancellation/concurrency, old-server equipment,
matched compatibility deployment, actual model behavior and manual flat/VR proof
remain open. No TTS expansion or shared-service changes.

## Acknowledged inventory readiness (2026-09-08)

Client 6abe6c4 / server 4fdfe96 require init_action_inventory_accepted before typed
inventory receipts. Old/failed/mismatched ACKs do not grant readiness; halt and
new initialization revoke it. The marker preserves individually requested older
capabilities. The plugin still does not advertise action.inventory_observation.

Both builds/17 suites, 727 fake transport checks, PHP 298/4974, protocol 31/150/167,
41 mixed SQL checks, build/package/server audits pass. Fresh flat/VR packages are
staged in scratch only. NOT ADVERTISED / NOT DEPLOYED. Full-main/model, old-server
equipment behavior, concurrency and manual flat/VR acceptance remain open.

## Mixed action/rechat inventory freshness (2026-09-08)

Server 5e04d9c crosses completed automatic-reply edges without resetting inventory.
Client product cb87acd is unchanged. Newest per-actor observations and unavailable
tombstones survive mixed ancestry; original snapshots are immutable. Current
candidate reads precede reservation, but historical rechat requires durable
prepared selection and exact completed parent/route proof under the real owner.

Proof: PHP 298/4958; mixed PostgreSQL 41 checks, ordinary inventory 63, completed
parent 37 (all rollback-only); both builds/17 suites, protocol 31/147/166,
build/package gates and audits pass. SOURCE ONLY / NOT ENABLED / NOT DEPLOYED.
Explicit readiness, matched full-main/model/concurrency and manual flat/VR
acceptance remain open. Five TTS providers only; pending embedding not deployed.

## Direct action inventory freshness (2026-09-08)

Client cb87acd / server 5dbaf95 connect native completion observations to copied
client continuations and accepted server action ancestry. Prompts/CheckInventory
see updated equipment; newest unavailable observations clear stale rows.
Original snapshots and unrelated scene facts stay unchanged.

Both native builds and 17-suite runs pass (91 action checks); protocol 31/147/166;
PHP 298/4954; real rollback-only SQL chain 63 checks; release/package audits pass.
NOT ENABLED / NOT DEPLOYED. Cross-rechat ancestry, explicit readiness, full-main/
provider/concurrency and manual gameplay proof remain open. The plugin currently
advertises rechat ownership but not action.inventory_observation. Five TTS providers only.

## Owned inventory receipts (2026-09-08)

Client 73ea72f / server 9999bcc add the typed inventory receipt, capability/lane
validation and exact durable equipment-action owner binding. Duplicate conflicts
cannot replace a terminal observation. Original snapshots are unchanged.
Both native builds and 17-suite runs pass; protocol 31/147/166; PHP 297/4921;
real rollback-only PostgreSQL 45 checks; release/package audits pass.

NOT ENABLED / NOT DEPLOYED / NOT END-TO-END FRESH: native completion forwarding,
bounded ancestry overlays, prompt/CheckInventory/client continuation consumption
and manual gameplay gates remain open. See ACTION-INVENTORY-OBSERVATIONS.md.
Only XTTS, Chatterbox, PocketTTS, Cartesia and Inworld remain in TTS scope.

## Post-action inventory producer (2026-09-07 local / 2026-09-08 UTC)

Client 32cacc6 adds copied, exact-action-owned post-equipment inventory to native
completion results. Both native builds/17-suite runs passed (68 action checks),
as did protocol/build/package audits. NOT DEPLOYED; no wire consumer/capability yet.

Confirmed remaining defect: client continuations and server CheckInventory still
read their original pre-action snapshot. This producer alone does not fix AI
freshness. ACTION-INVENTORY-OBSERVATIONS.md records the required matched codec,
ingress, idempotent receipt and ancestry-scoped ephemeral view work. Original
context snapshots must remain immutable, and unavailable newer reads must not
fall back to older equipment facts.

Installed client 27c8515 and compatibility server response patch remain unchanged;
server health ok. Server product code unchanged this turn; no game control or new
gameplay evidence. VR equipment remains disabled. Full parity remains incomplete.

## Flat NPC equipment actions (2026-09-07 local / 2026-09-08 UTC)

Flat EquipItem/UnequipItem are source-connected and locally deployed in client
27c8515: response-owned exact inventory binding, bounded nonblocking native reads,
positional stack revalidation, no forced/queued equip, final cancellation/load/
deadline gates and observed equipment/count postconditions. Ambiguous instances
and name-only partial-list selections are rejected. Eight flat capabilities;
VR equipment is explicitly unavailable and its six prior actions are unchanged.

Server 33c3a6c fixes equipment arguments being mistaken for an actor target.
Only that response.php fix was deployed from compatibility checkout 44ac03a
(codex/equipment-local-20260908, based on installed 587ef34). Pending embedding
source/migrations remain undeployed; no shared MiniMe changes.

Both native builds and both 17-suite runs passed, including 51 action checks;
protocol/build/package audits passed. Maintained PHP: 295 tests / 4831 assertions;
compatibility PHP: 16 tests / 179 assertions; both release audits passed.
The 19-check actual PostgreSQL equipment emission probe passed for source,
compatibility and installed code with rollback cleanup. Installed DLL/server-file
hashes and backups are in the retired parity ledger ([handoff](HANDOFF.md#retired-ledgers)). Health HTTP 200 / ok.

Not in-game proven. Visible equipment, locked/outfit restrictions, no item loss,
cancellation/save-load stability and actual dialogue follow-up remain manual
flat acceptance. Safe VR instance access and separate headset acceptance are
still required. Full gameplay/behavioral parity is not complete. Five TTS systems only.

## Receipt-relative action deadline (2026-09-07 local / 2026-09-08 UTC)

Client efa6493 on codex/dialectic-parity-core retains action receipt deadlines
through dialogue/pause/worker queues and checks expiry inside the final native
callback lock. Expired work returns timed_out without starting; terminal delivery
has its own bounded budget. Already-started engine calls are not preempted.
No equipment action/capability, protocol or version change.

Flat and VR DLL builds and both 17-suite runs passed (28 action checks), plus
29 schemas / 140 fixtures / 157 mirrored files, 34 build-gate tests, 11 packaging
tests and both package audits. Flat deployed to the isolated SYNTH_dev mod with
matching DLL hash and unchanged custom INI/PEX; previous mod hash-backed up.
VR packaged only. Exact hashes and backup are in the retired parity ledger ([handoff](HANDOFF.md#retired-ledgers)).

Server source 5421c8d and runtime unchanged; HTTP 200 / ok. Pending embedding
deployment and shared MiniMe scope remain open. No game control or new gameplay
proof. Equipment/action coverage, broader stability and separate flat/VR manual
acceptance are still incomplete. Five approved TTS providers only.

## Fair NPC journal backfill (2026-09-07 local / 2026-09-08 UTC)

Server 5421c8d on codex/dialectic-parity-core fixes a reproduced multi-author
backlog delay: new NPC journal candidates prefer least-recently-admitted author,
then oldest source. Existing durable job history supplies ordering only, not
permission; source visibility, private recall, retry priority and capacity remain.
No schema/configuration/protocol or native product change.

703 multi-author PostgreSQL checks passed: all three same-named NPCs served in
the first rotation, complete 14-entry backfill, private semantic recall, three
fresh automatic processes and busy-author exclusion. The changed selector also
passed the 257-entry / 2661-check regression, 294 PHP tests / 4805 assertions,
CLI/HTTP cancellation regression and release audit. See server embedding contract.

NOT DEPLOYED. Shared MiniMe metadata and coordinated deployment remain gated;
large multi-author production-scale and broader gameplay/flat/VR acceptance are
not proven. Client product beec766 is unchanged. Five approved TTS providers only.

## Linked journal scale checkpoint (2026-09-07 local / 2026-09-08 UTC)

Unchanged server ac487e9 passed a 257-entry genuinely published NPC diary chain:
2661 combined PostgreSQL checks, exact saved prior links, complete incremental
backfill, original preservation, corrupt-cache due retry and ancestor
archive/restore across all cached descendants. At 257 entries, lexical/semantic
recall measured 150/160 ms; maximum discovery was 180 ms. See the goal ledger
and server embedding contract for fixture scope, measurements and reproduction.
Current regression: 294 PHP tests / 4801 assertions; server release audit and
both repository diff checks passed. Native code/artifacts were not rebuilt.

Validation/docs only; no product edit or deployment. Both repositories remain
on codex/dialectic-parity-core; client product beec766 is unchanged. This is not
multi-author production-scale, actual-provider, live-daemon or gameplay proof.
Production's 30-second reservation was not changed. Shared MiniMe metadata,
coordinated deployment, remaining gameplay features and separate flat/VR manual
acceptance remain open. Only the five approved TTS providers remain in scope.

## Saved NPC journal backfill (2026-09-07 local / 2026-09-08 UTC)

Server ac487e9 on codex/dialectic-parity-core adds npc_journal maintenance using
the existing integrity-checked saved author/ancestry view. Journals no longer
need their NPC to be the current target for repair. Ordinary diary remains
target-bound; all NPC/Player/Narrator recall permissions are unchanged.
Maintenance and target-bound aliases share admission/cache identity.
Non-target due recovery and per-author capacity filtering are implemented.

294 PHP tests / 4801 assertions, 541 PostgreSQL checks, exact author/revision/
frontier and private-role denial, alias deduplication, targetless journal HTTP,
CLI/cancellation regression, guarded rollback and release audit passed.
Schema marker 20260908006 adds no table/column. See goal ledger and contract.

NOT DEPLOYED. Live provider metadata, large-history/backfill acceptance and
coordinated deployment remain; full action/presentation/stability parity and
manual flat/VR acceptance are still open. Shared MiniMe and deployed artifacts
unchanged. Five approved TTS providers only.

## Player and Narrator embedding repair (2026-09-07 local / 2026-09-08 UTC)

Server 0f8a5bd on codex/dialectic-parity-core adds exact Player/Narrator source
kinds, saved-player identity checks, targetless execution and a four-way
scheduler cursor (schema 20260908005). Both player-owned roles share the existing
actor capacity. Original diaries/vectors are preserved; recall permissions are
unchanged. Narrator semantic recall uses its repaired entries; private Player
entries remain excluded from NPC and Narrator recall.

294 PHP tests / 4799 assertions, 505 PostgreSQL checks, actual role diary
capture/publication, four-kind scheduling, targetless Player/Narrator repair,
cross-role/private-read denial, in-flight Player edit, guarded rollback,
CLI/HTTP regression and release audit passed. See goal ledger and server
docs/NATIVE-EMBEDDING-SPACE.md.

NOT DEPLOYED. Provider metadata, broader non-target NPC journal discovery,
large-history/backfill acceptance and coordinated deployment remain open.
No production bootstrap, real provider or gameplay proof from these tests.
Client product and current deployment unchanged; shared MiniMe untouched.
Five approved TTS providers only. Full parity remains open.

## Automatic embedding repair scheduling (2026-09-07 local / 2026-09-08 UTC)

Server efc1924 on codex/dialectic-parity-core connects repair jobs to the
background manager through a fresh CLI process. Schema 20260908004 adds
30-second per-session reservations and rotating summary/NPC-diary preference.
Exact completed owners, saved visibility, response-owned model identity,
due-job recovery, immutable source/model matching and existing leases remain
mandatory. Original narratives/vectors are preserved; USE_TEXT2VEC is respected.

294 PHP tests / 4791 assertions, 446 PostgreSQL checks, two-process scheduler
reservation, model-switch rejection, alternating source kinds, guarded rollback,
actual CLI plus loopback HTTP with isolated bootstrap, and release audit passed.
See the goal ledger and server docs/NATIVE-EMBEDDING-SPACE.md.

NOT DEPLOYED. NPC-source discovery/dispatch is now implemented; remaining
Player/Narrator roles, provider metadata, broader backfill acceptance and
coordinated deployment remain open. Production bootstrap/daemon and real
provider/gameplay proof were not obtained. Client product/local deployment
unchanged. Shared MiniMe untouched. Five approved TTS providers only.

## Durable embedding repair jobs (2026-09-07 local / 2026-09-08 UTC)

Server 2abd7fd on codex/dialectic-parity-core adds deduplicated admission,
canonical-owner execution, two-active-per-actor capacity, expiring leases,
bounded retries and crash recovery for exact-source memory/NPC-diary repairs.
Schema 20260908003 adds indexes, not new narrative storage or generation.
Original text/vectors remain unchanged.

293 PHP tests / 4783 assertions, 337 PostgreSQL checks, real child-process
concurrency/crash probes, guarded rollback, actual loopback HTTP job execution
and request/lease cancellation, and release audit passed. See goal ledger and
server docs/NATIVE-EMBEDDING-SPACE.md for exact evidence and limits.

NOT DEPLOYED. Admission/lease/retry gap is closed for callers using the new APIs;
automatic fair discovery/dispatch, provider metadata, remaining roles and
backfill are still open. Direct primitive callers are not admission-deduplicated.
Shared MiniMe untouched pending scope decision. Client product and current local
deployment unchanged; gameplay/full parity not complete. Five TTS providers only.

## Immutable per-record vector repair (2026-09-07 local / 2026-09-08 UTC)

Server 2768e15 on codex/dialectic-parity-core adds append-only exact-source
embedding repairs and connects them to semantic recall. Original text/vectors
remain unchanged; corrected summaries and NPC diaries can receive independent
vectors. Canonical owner, saved visibility, source/model checksums, changed
generation, archive/edit and pending diary-control guards passed real SQL tests.

292 PHP tests / 4765 assertions, 151 PostgreSQL checks, fresh-process reuse,
concurrent single-record publication, guarded rollback, real repair HTTP and
cancellation, and release audit passed. See goal ledger and server
docs/NATIVE-EMBEDDING-SPACE.md for exact scope and evidence.

NOT DEPLOYED. This is a per-record primitive, not automatic backfill:
concurrent calls can still duplicate provider work before publication. Durable
admission/leases/retries, fair discovery, remaining role coverage and provider
metadata remain open. Shared MiniMe untouched pending scope decision. Client
product unchanged; gameplay acceptance and full parity remain open. Five TTS
providers only; no publication.

## Native embedding-space prerequisite (2026-09-07 local / 2026-09-08 UTC)

Server 61720f1 on codex/dialectic-parity-core implements response-owned vector
model/revision/preprocessing identity for native summaries and diary checkpoints/
results. Semantic comparisons require an equal space; unknown/mismatched vectors
retain text recall. Actor/save/correction/lease fences remain mandatory.
291 PHP tests / 4747 assertions, 59 PostgreSQL checks, guarded rollback/re-upgrade,
real loopback provider/public recall and cancellation probes, and release audit
passed. See the goal ledger and server docs/NATIVE-EMBEDDING-SPACE.md.

NOT DEPLOYED; provider support and resumable repair/backfill remain OPEN.
The current shared MiniMe /embed response lacks model metadata. Its checkout is
dirty and untouched; shared-service scope was asked, not assumed. Text-only
fallback is not the requested final semantic-memory behavior. Client product
unchanged; previous deployed facing milestone remains healthy. Manual flat/VR
and broader feature acceptance remain open. Five approved TTS providers only.

## Owned speech facing deployed (2026-09-07 local / 2026-09-08 UTC)

Client beec766 and server 587ef34 on codex/dialectic-parity-core are deployed
together to the isolated flat SYNTH_dev mod and WSL Synthserver runtime.
This supersedes the historical prerequisite status below. Both native lanes
advertise exact listener transport; active speech carries immutable ownership
to a guarded game-thread one-shot yaw turn. No unresolved-listener fallback.
VR has separately verified relocation coverage and a minimum library guard.

Fresh flat/VR builds and 17 suites each, 34 build guards, 11 packaging tests,
both package audits, 140 protocol fixtures and server release audit passed.
Installed flat DLL hash matches its build; server health and checked state
preservation passed. VR is packaged, NOT installed or tested in-headset.
See the retired parity ledger ([handoff](HANDOFF.md#retired-ledgers)) for exact hashes, backups and evidence boundaries.

Manual facing, animation interaction, interruption, save-load and long-session
acceptance remain OPEN. Full feature parity is NOT complete. No game/editor was
controlled. Only XTTS, Chatterbox, PocketTTS, Cartesia and Inworld are in TTS scope;
other retained adapters are untouched. No sibling server changes or publication.

## Facing prerequisite: canonical listener wire (2026-09-07)

Client 69bbe3d and server 587ef34 on codex/dialectic-parity-core add negotiated
canonical listener transport using the same immutable context and final wire
hash as rechat/history. Both native builds/17-suite runs and audited packages
pass; server 290 tests/4718 assertions; actual synthetic flat/VR dialogue,
replay, cancellation and narrative recall checks pass. See the goal ledger.

NOT DEPLOYED and NOT facing parity: PluginSession does not advertise the
capability yet. Native playback identity/utterance propagation, exact live actor
eligibility, separate VR relocation verification and one-shot game-thread
rotation are next. No player/crosshair fallback on unresolved group dialogue.
No game control or game acceptance. Only the five approved TTS providers remain
in scope. HerikaServer, StobeServer and DialecticServer are unchanged: this is
SYNTH's own versioned native-wire contract, not a shared adapter modification.

## Ordered speech and shared-cache fixes deployed (2026-09-07, historical)

_Historical receipt, retained as dated evidence. Workstation paths are replaced with
placeholders; see [HANDOFF.md](HANDOFF.md#historical-receipts-and-placeholders)._

The paired local flat deployment now includes client 843f848 (ordered independent
speech delivery) and server db0b59a (shared-cache cancellation/readiness). Both are
on codex/dialectic-parity-core. See the retired parity ledger ([handoff](HANDOFF.md#retired-ledgers)) for exact hashes,
backups and passing build/test/deployment evidence. No game was controlled;
manual flat and independent VR acceptance remain open. Five TTS providers only.

## Paired local deployment: owned replies and truthful action results (2026-09-07, historical)

_Historical receipt, retained as dated evidence. Workstation paths are replaced with
placeholders; see [HANDOFF.md](HANDOFF.md#historical-receipts-and-placeholders)._

DEPLOYED LOCALLY / MANUAL GAME ACCEPTANCE OPEN / FULL GOAL ACTIVE.

Used synth-full-deploy with explicit clean worktrees on codex/dialectic-parity-core:
- SYNTH 3fde817e3cb4c14b527f435d15685e30a8bb2b65 (client product 9d2ec76).
- Synthserver 950fc5298b286267f8b350df52ed25739c039df0 (latest product d02ec3b,
  including 29b6740 cancellation, 322b301 capability catalog and 2b4431f actor binding).
The client's owned automatic replies and all coordinated server fixes are now
installed together. TTS scope stays XTTS, Chatterbox, PocketTTS, Cartesia, Inworld.

Read-only process/MO2 checks found no Fallout4/Fallout4VR process. Default profile
uses `<Fallout4-install>`, executable 1.11.240.0.
No game, VR or editor was launched, closed or controlled.

Verification:
- Fresh flat and VR x64 builds pass, all 17 native suites each, including 539
  fake-server checks. 34 build guards, 11 packaging tests and 140 protocol
  fixture cases pass. Both lane package audits pass; VR packaged, not installed.
- Skill reports SUCCESS - Synthserver to WSL, SUCCESS - SYNTH flat client to MO2,
  and Deployment completed successfully. Bootstrap/schema and Apache syntax pass.
- Flat installed `<MO2-mods>/SYNTH_dev/F4SE/Plugins/SYNTH.dll` SHA256:
  837CF1FB20237922437D37AED6017F636E863FC1A262D1CEF5D01B19F95A24DF.
  Built VR SHA256:
  ACB769088E184464289384CA2758635B47B12979F8A81079CC6A7ED26391476C.
  Installed original SYNTHNative.pex SHA256 unchanged:
  56D6D12F82FFC41F1CFA91A2AA465B67466022F7C550B0B4CEBA3E56D11FEF9E.
  Custom INI SHA256 unchanged:
  51972C4A66F1EF89B067C6C26747CFB5CFC425E6C667F52180296F433CDF82F3.
- Server `<wsl-distro>:/var/www/html/Synthserver`; ten changed product files
  compared byte-for-byte with source: request, provider_stream, funcret,
  synth_protocol_state, functions catalog and five JSON connectors.
- HTTP http://127.0.0.1:8087/Synthserver/health.php is 200 with database/schema/
  background_processor true. Explicitly restarted only the verified SYNTH
  supervisor after stopping its exact old parent/children before synchronization.
  New supervisor 1899318, nested supervisor 1899330 and tts_dispatch.php --loop
  1899332 remain alive on a subsequent process check. Port 12348 stays isolated.
- Private job endpoint returns 403; invalid tts_status request returns 400.
  Server conf/conf.php, data/voices, soundcache and uploads compare unchanged
  against the pre-deploy archive. No production dialogue/test requests inserted.
- Last source server suite, before this documentation-only deployment checkpoint:
  282 tests / 4571 assertions plus full synthetic action/reply/outcome checks.

Recoverable pre-deploy backups retained:
- Windows `<private-backup>/deploy-20260907-2218/SYNTH_dev`.
- WSL `<server-backup>/synth-parity-20260907-2218`, mode 0700.
  synth-before.dump SHA256
  4c0b310061ba21c5b6d8e9aa966111d4b403a7d4ff970378f4faac6d6006f120;
  pg_restore --list succeeds (747 output lines); no restore test performed.
  Synthserver-before.tar.gz SHA256
  087c30b27511c21230149918e8c1232de67bd24ff43c148411b43c6f8d5446ac.
No push, PR, release or version bump. No sibling mod/service files were targeted.

Manual handoff: user launches Fallout, loads a save, sends a message and
interrupts an automatic reply with another message; then correlate fresh logs.
This is deployed readiness, not in-game acceptance. Separate VR headset proof,
real five-provider voice/cloning/model behavior, private-mode controls, native
actions and remaining gameplay/persistent AI/management/public API parity remain
open. Full goal stays active.

## Owned reply-chain HTTP acceptance and client enablement (2026-09-07, historical)

_Historical receipt, retained as dated evidence. Workstation paths are replaced with
placeholders; see [HANDOFF.md](HANDOFF.md#historical-receipts-and-placeholders)._

SOURCE / BUILT / PACKAGED / NOT DEPLOYED THIS TURN / FULL GOAL ACTIVE.
Client product 9d2ec76 enables dialogue.rechat.ownership in the shared flat/VR
capability set. Existing scheduling now sends the exact completed parent
request and line, original turn/snapshot and cancellation token. No new
setting, legacy fallback, schema/version change or runtime adapter change.

Server product 29b6740 fixes a defect found in the actual child-reply path:
cancelling a completed root request only cancelled that row, leaving the child
provider running. It later failed the prepared-parent proof and left its own
request claimed. Negotiated v2 cancellation now resolves either a context or
an owned response as the exact turn/snapshot anchor. It retires matching
requests and the active turn, and cancels workers with either nested legacy
or top-level durable context_sequence. Other sessions, generations, turn IDs
and snapshot sequences remain excluded. Unnegotiated, v1, unowned and
non-manual activation requests keep request-only cancellation.

Evidence:
- Full-main synthetic HTTP: two captured NPCs alternate for two rounds, use
  the selected canonical profile/speaker, include verified original-input
  ancestry in actual model requests, synthesize/register/download PocketTTS
  WAV, replay exact response bytes without another provider call, then stop
  at the two-round budget without another model request.
- rechat-finalflat and rechat-finalvr: 73 checks each. VR is wire/HTTP proof,
  not headset/game proof. rechat-samename1: 73 checks with distinct FormIDs
  sharing the same label; conversational selection preserved each identity.
- rechat-cancel3: ancestor-request cancellation unwound the child in 0.055 s;
  88 checks including next-message speech recovery. rechat-ctxcancel1:
  context-anchor cancellation 0.034 s, 105 checks with next-message recovery.
  Counts include polling assertions and vary with scheduling.
- Existing full-main model/TTS cancel and generation-advance suite:
  ownercancel-http1, 237 checks; recovery passed. All fixture listeners/workers
  stopped; ports 18971/18972/18973/18976 verified unbound afterward.
- Server PHPUnit: 279 tests / 4478 assertions; release audit 722 tracked,
  414 PHP, 152 JSON, 12 Herika-style directories, 3 catalog hashes.
- Both Windows x64 DLL builds, all 17 native suites in each lane, 539 fake-server
  checks, 34 build guards, 11 packaging tests and 140 protocol cases passed.
  Both generated package manifests/release audits passed.
  Flat DLL: 837CF1FB20237922437D37AED6017F636E863FC1A262D1CEF5D01B19F95A24DF.
  VR DLL: ACB769088E184464289384CA2758635B47B12979F8A81079CC6A7ED26391476C.
- Fixture script `<scratch>/rechat-http-probe.php`,
  SHA256 6AAB6DD4D234EDE7D302692B0BF6E248F3098F71E3106F98880E1FBE73EE8726.
  Synthetic provider script cancel-pipeline-provider.php SHA256
  132C38CB92C7446DE11B98411AF87A046E34EB29CE2AAE7A42A84E823F060F47.
  Runtime /tmp/synth-cancel-pipeline-PmHVx1Rk and DB
  synth_parity_cancel_20260907 contain only retained synthetic fixtures.

Installed client remains f1ff7ce / DLL
0CBA561B8D45EC0A43C8EC462A47094A11DF3583BC4C8414DB01FE13D9B4D514;
installed server remains 240e3b3 / schema 20260907007, HTTP health/background
true. No deployment/service mutation, game control, production test data,
real provider call, push or PR this turn. Preserve the user's manual-test
baseline; the next paired deployment must include BOTH product commits above.

Still open: real voice quality/provider lifecycles for the five allowed systems,
narrator/action/private-mode full-path variants beyond the existing narrower
proofs, client game scheduling and independent headset acceptance, deadline
origin/cache-owner edge cases, and the remaining full gameplay, persistent AI,
management and public API parity matrix. This enables a proven source path;
it is not full parity or in-game completion.


## Local paired deployment and legacy speech cutover (2026-09-07, historical)

_Historical receipt, retained as dated evidence. Workstation paths are replaced with
placeholders; see [HANDOFF.md](HANDOFF.md#historical-receipts-and-placeholders)._

DEPLOYED LOCALLY / GAME ACCEPTANCE OPEN / FULL GOAL ACTIVE.
Scope remains XTTS, Chatterbox, PocketTTS, Cartesia and Inworld only.

Server product 240e3b3 closes the old file-worker bypass: native turn-owned
five-provider envelopes exit before runtime bootstrap/provider work instead
of executing without a durable job lease. Both captured-plan and older
tts_function shapes are recognized. Old envelopes are retained, not adopted,
deleted or replayed. Ten real worker subprocess cases verify the refusal and
unchanged files. Out-of-scope providers and non-native worker paths are unchanged.

Verification:
- Full server PHPUnit: 279 tests / 4414 assertions.
- Full-main synthetic PocketTTS pipeline: cutover-http1, 242 checks. Model
  cancel 0.183 s, speech cancel 0.098 s, model generation advance 0.183 s,
  speech generation advance 0.201 s; next-message recovery passed. Actual
  status/media HTTP checks verify registered audio bytes. All fixture servers
  and workers stopped. No real provider credentials or production test jobs.
- Release audit: 722 tracked files, 414 PHP, 152 JSON, 12 Herika-style
  directories, 3 knowledge checksums. Inventory still includes 17 adapters;
  that is packaging preservation, not an expansion of the five-provider goal.
- Actual installed Apache invoked a PHP CLI child as www-data (UID 33).
  Synthetic private-input bytes matched, outside the document root; directory
  mode 0700 and file mode 0600. Endpoint and input were removed after the probe.
  Probe SHA256: 67295719D34602784C0420C75B2F58A502F7C3ED0AEBF5CFF5CB29CC52F8E2C6.
  Script: `<scratch>/synth-private-access-probe.php`.

Deployment:
- Sources: SYNTH fa45fd4 (product f1ff7ce), Synthserver 240e3b3;
  both codex/dialectic-parity-core, clean before deployment.
- Used synth-full-deploy with explicit worktrees and -SkipBuild after the
  preceding flat/VR full build/package checkpoint. Both required success
  summaries and overall completion were returned.
- Flat target `<MO2-mods>/SYNTH_dev`; verified portable MO2 Default
  uses `<Fallout4-install>`, executable 1.11.240.0.
  Installed DLL SHA256:
  0CBA561B8D45EC0A43C8EC462A47094A11DF3583BC4C8414DB01FE13D9B4D514.
  Installed original bridge PEX SHA256:
  56D6D12F82FFC41F1CFA91A2AA465B67466022F7C550B0B4CEBA3E56D11FEF9E.
- Server: `<wsl-distro>:/var/www/html/Synthserver`. PostgreSQL synth migrated
  from 20260906005 to 20260907007; bootstrap/seed verification passed.
  Selected worker, durable-job and supervisor source/runtime SHA256 pairs match.
- HTTP 200 at http://127.0.0.1:8087/Synthserver/health.php, with database/schema
  true and background_processor true after explicit service restart.
  Version/build strings remain 0.7.2 / 2026081007; no release/version bump.
- Old supervisor PID 417146 was verified idle and stopped before synchronization.
  New parent 1861418, nested supervisor 1861438, durable speech dispatcher
  1861440 were confirmed alive; dispatcher remained stable across observations.
  Real manager children completed their tasks. No Fallout process was present.
- Important deployment-helper limit: HTTP 200 did not start/verify the background
  service. Explicitly called the installed synthEnsureBackgroundProcessorRunning
  as www-data after synchronization and verified both heartbeat and dispatcher.

Preservation and rollback evidence:
- WSL backup directory `<server-backup>/synth-parity-20260907-2130`, mode 0700.
  synth-before.dump SHA256:
  e24e542b763a0ef4a43287f249aa49e817b7b748145382070cbeff78e7ba2a4b.
  Archive index readable, 702 entries; no restore into production was performed.
  Synthserver-before.tar.gz SHA256:
  e7099fcdb2e5fc236fe1d92d3991728444599f9722f94e0096d584806841ee54.
- Client backup: `<private-backup>/deploy-20260907-2130/SYNTH_dev`.
- Client custom INI hash unchanged:
  51972C4A66F1EF89B067C6C26747CFB5CFC425E6C667F52180296F433CDF82F3.
  Server conf/conf.php hash unchanged; archive comparison also confirmed
  conf/conf.php, data/voices, soundcache and uploads unchanged.
- No save/profile/other-mod/game/F4SE-root writes, game control, push or PR.
  VR artifact is built/audited but not deployed or headset-accepted.

Next: user manually loads a save, messages an NPC, cancels, and sends the next
message; correlate new client/server logs. Real five-provider cloning/lifecycle
acceptance, deadline-origin alignment, overlapping cache-owner/status behavior,
remaining full gameplay/persistent AI/management/public API parity, and separate
flat/VR sustained acceptance remain open. Do not equate deployment with parity.


## Current TTS scope and speech deadline checkpoint (2026-09-07)

Only XTTS (xtts-fastapi), Chatterbox, PocketTTS, Cartesia and Inworld are
in scope for ongoing TTS implementation and acceptance. Existing other
adapters/settings remain intact; their provider-specific completion is not
a parity gate. The rest of the approved Dialectic parity goal remains active.

Client product f1ff7ce: SOURCE / BUILT / PACKAGED / NOT DEPLOYED.
- Speech status and media HTTP now share the utterance's absolute steady-clock
  deadline. Acquiring the existing serial request lock polls cancellation and
  deadline every 10 ms; no removal of context-publication serialization.
- Remaining HTTP time is capped after lock acquisition. The transport gets a
  deadline-aware cancellation predicate, and late replies are rejected even
  if the transport returns success. Ordinary dialogue timeout is unchanged.
- Pending-status sleeps cannot extend the voice deadline. The session checks
  expiry before media fetch and after validation/spatial preparation before
  enqueue. Cancellation remains quiet rather than generating a voice error.
- Existing fake-server suite covers already-expired requests, cancelled or
  expired waits behind an actual held context lock, cooperative delayed HTTP,
  ignored cancellation/late success, timeout caps and a successful next request.
  Full suite passed 539 checks. All 17 native suites passed in flat and VR;
  140 protocol cases / 29 schemas, 34 build guards and 11 packaging tests pass.
- Both Windows x64 DLLs built. Both package manifests and release audits pass.
  Flat SHA256: 0CBA561B8D45EC0A43C8EC462A47094A11DF3583BC4C8414DB01FE13D9B4D514.
  VR SHA256: C860E92783D0135E0953DEFA7026D5D847084837A681A5DB6770491035CF88DD.
- Installed flat DLL remains
  CB517DE21A2CB8F27DD859C352439B3E0E404D4C632086340397F87975C20364.
  No game launch/control, provider call, runtime sync or production DB mutation.

Limits: these are deterministic transport tests and native build proof, not
fresh real WinHTTP/provider or game playback acceptance. This bounds retrieval
and admission, not the duration of audio already admitted to playback.
Server/client deadline origin alignment, old-job cutover, same-cache owner
overlap, real Apache private-input access, five-provider lifecycle acceptance,
safe paired deployment and separate manual flat/VR acceptance remain open.
Server implementation/evidence through 3b7b62a is in its current parity ledgers.


## Latest checkpoint: staged rechat parent contract (2026-09-07)

IMPLEMENTED FOUNDATION / DISABLED / NOT DEPLOYED. Client product 6c42a7b and
server product 5c6471b carry and strictly validate the exact parent request/line
pair. The client retains parent and original ownership in one pending bundle.
Mirrored v2 schemas and golden fixtures cover flat, VR and invalid contracts.
The plugin does not advertise dialogue.rechat.ownership; the server rejects
that staged path with owned_rechat_not_ready before legacy fallback.
No durable parent completion, route selection or working continuation is
claimed. Earlier defect-detection checkpoints remain historical evidence.

Validation completed for these product commits:
- Both flat and VR DLL builds; all 17 native suites and VR CTest passed.
- Fake-client checks: 510. Build gates: 34 tests. Packaging: 11 tests.
- Protocol: 29 schemas, 140 fixtures, 157 mirrored peer files.
- PHP: 211 tests / 3170 assertions.
- Server release audit: 707 tracked files / 399 PHP / 152 JSON /
  12 canonical Herika-style runtime directories.
- Real scratch SQL: 91 profile/action ownership checks and 17 legacy rechat
  defect-detection checks. Synthetic fixtures rolled back.
- Both internal flat/VR validation ZIPs passed manifest and lane audits.

Artifact SHA256:
- Built flat DLL: BE428041A03033AC34E7F92A3876D30E7075FDE11C24D3D81541B31D449954A9
- Built VR DLL: D2E3C298800730871B1D130982300F4257B7B14B321BC14A1D13A3808091658F
- Internal flat ZIP: 29AE4CD28B94EEAF3F4C887FCD9687EACF78B02D18029607ACD4E3AC2C7BB467
- Internal VR ZIP: DB740D71AB2F34837C52F187962D60886D1DD58DCB141EB1B1771801617E8F62
- Installed flat DLL unchanged:
  CB517DE21A2CB8F27DD859C352439B3E0E404D4C632086340397F87975C20364

Installed products remain client fc2a484 and server 0ade54e (schema
20260906005). Canonical profile schema 20260906006 remains source-only.
No deployment, provider call, game control, push, PR or release occurred.

Next: independent canonical emitted listener routes, exact completed-parent
admission, durable one-child/replay/budget decisions, selection before profile
loading, owned chain prompt/history evidence and full dispatcher integration.
Keep the staging guard until these coupled gates pass. Full parity remains
ACTIVE; flat manual gameplay and independent VR headset acceptance remain open.
The separate Herika/Lorkhan folder-alignment request remains complete.

## Native rechat ownership contract (2026-09-07)

REPRODUCED / NOT FIXED: real SQL and actual trigger/parser functions pass
17 defect-detection checks: lost speaker/audience/identity, stale profile-name
fallback, rejection of the non-target last speaker and shared name-budget keys
across sessions/turns/generations/runtime lanes. All synthetic rows rolled back.
Client already has request/line IDs but does not send them in rechat; emitted
response bodies omit listener routing. Optional relationship capture is not
an unconditional route source.

NATIVE-RECHAT-OWNERSHIP.md defines the coordinated parent contract, durable
routes/decisions/budget, selected profile/speech/history and chain-prompt gates.
No product fix/deployment this turn; source action milestone d9fd4cd remains.
Installed server 0ade54e and client fc2a484 unchanged. Full goal stays active.

## Action reply ownership across profile, speech and history (2026-09-06)

PARTIAL SOURCE / NOT DEPLOYED: server d9fd4cd derives the response actor from
the exact persisted action and captured identity, sharing that ownership with
profile readers/writers, speech and the saved history reader. Early native
funcret binding now loads that actor's profile. Exact target speech works with
same-name audience actors; ambiguous non-owner names remain rejected.
Missing action provenance cannot fall back to the original target.

PHP 210/3135; real-SQL profile probe 91, cooldown 58, ancestry 42; inventory
binder 512 stacks/1024 units; audit 697/399/143 and protocol 29/131/148 pass.
Synthetic fixtures rolled back. No full main/provider/voice/game proof.
Rechat's server-selected responder, complete profile workers, management/TTS
callers, browser/upgrade/concurrency and end-to-end gates remain before deploy.
Runtime server 0ade54e/schema20260906005 and client fc2a484 unchanged.

## Canonical profile writer boundaries (2026-09-06)

PARTIAL SOURCE / NOT DEPLOYED: server b9f2cc5 fences ID-based profile writes
to freshly read canonical ownership under the native request lock. Stale
caller rows cannot replace protected tokens/settings; partial updates retain
native profile slots. Voice/metadata/delete/backup writers share the guard.
Legacy bulk history no longer touches native rows; per-row history inserts
retain npc_id without the nonexistent actor_identity_id history column.

PHP 210/3134, real-SQL owned-profile probe 66 checks, release audit 697/399/143
with 12 canonical directories, and protocol 29/131/148 pass. SQL fixtures
rolled back. Token-collision, wrong-owner, legacy and cancelled-generation
checks are included, not full provider/worker/browser/game proof.
Installed server 0ade54e/schema20260906005 and client fc2a484 unchanged.
Remaining integration/deployment gates are in NATIVE-NPC-PROFILE-OWNERSHIP.md.
Full goal remains active; separate Herika/Lorkhan folder placement is complete.

## Canonical NPC profile storage and queued ownership (2026-09-06)

PARTIAL SOURCE / NOT DEPLOYED: server 330f8ff separates native actor-keyed
profiles from unbound legacy name rows. Captured actor metadata, stable
native row/tokens, rename/manual voice preservation, scoped reads/writes
and two-same-name durable profile job transfer are implemented. Schema
20260906006 is source-only. Existing linked rows are preserved, not reassigned
or retroactively repaired by guessing historical ownership.

PHP 210/3122, real-SQL owned-profile probe 37 checks, release audit 697/399/143,
and unchanged protocol 29/131/148 pass. Management identity labels have syntax
proof only; Claude Opus 5 authentication expired and Codex applied the bounded
fallback. Full main/voice/worker completion, remaining name/token/ID callers,
token collisions, browser/upgrade/concurrency/cancellation gates remain before
deployment. See server docs/NATIVE-NPC-PROFILE-OWNERSHIP.md for exact next work.
Installed server 0ade54e and client fc2a484 unchanged. Full goal remains active.

## Native NPC profile identity cutover (2026-09-06)

REPRODUCED / NOT FIXED. Real SQL using the actual input binding block confirms
same-name and cross-playthrough profile reassignment, private personality/
voice/metadata inheritance, original actor link loss, and rename collision.
Source and installed diagnostic: 21 defect-detection checks each; all fixture
rows/DDL rolled back. This is not evidence of a fixed system or freeze cause.

The server's docs/NATIVE-NPC-PROFILE-OWNERSHIP.md now defines the required
coordinated storage/runtime/worker/import/management cutover and preservation/
acceptance gates. No product changes or deployment this checkpoint. Deployed
server 0ade54e and client fc2a484 remain unchanged. Full goal stays active.

## Saved native action cooldowns (2026-09-06)

IMPLEMENTED / SERVER-ONLY DEPLOYED: 0ade54e records native action issuance
atomically with its intent, using the captured game clock and empty witness
audience. Cooldowns read exact actor/save history plus only the current owned
turn's later issuance, not legacy NPC-name/wildcard logs or a stale name cache.
Save reload excludes post-frontier future actions; same-name actors remain
separate. Native issuance no longer commits a caller's transaction. V1 behavior,
durations and clock conversion remain. Old ambiguous logs are not backfilled.
Schema 20260906005 adds one partial lookup index; no deletion/protocol change.

PHP 209/3110, source and installed real-SQL cooldown/writer probes 58 checks,
installed follow-up 42 checks, audit and protocol 29/131/148 pass. Old installed
code fails the unrelated wildcard test. Installed hashes/index/schema/HTTP200
verified; config/14 voices/client unchanged. No provider/game or latency proof.
Full pipeline/profile ownership, model execution, broader roadmap and manual
flat/VR acceptance remain open; see the retired parity ledger ([handoff](HANDOFF.md#retired-ledgers)).

## Owned native action follow-up depth (2026-09-06)

IMPLEMENTED / SERVER-ONLY DEPLOYED: 7abe237 replaces native v2's latest legacy
action-log depth with exact persisted action/request ancestry. Session, turn,
generation, snapshot, runtime lane, action/idempotency and first action code
stay bound across parents. Mixed action codes cannot reset the one-step limit.
Cycles/unknown parents disable extra tools without dropping textual results;
existing settings and legacy v1 behavior remain. No schema/protocol changes.

PHP 207/3088, source and installed SQL/catalog/processor probe 42 checks,
installed tool-pair 27 checks and full 512-row inventory regression pass.
Old installed code fails the native depth check against conflicting legacy
wildcard entries. Audit and protocol 29/131/148 pass. Server-only deploy,
backup, installed hashes and HTTP200 verified; config/14 voices/client unchanged.
No model/game proof. Other legacy cooldown/profile name readers, full request
composition, context budgets, broader roadmap and manual flat/VR acceptance
remain open; see the retired parity ledger ([handoff](HANDOFF.md#retired-ledgers)).

## Real SQL history-memory-provider integration (2026-09-06)

SYNTHETIC SOURCE AND INSTALLED INTEGRATION VERIFIED: deployed server 832c2f3
passes production PostgreSQL event writes, owned memory batch/job completion,
history-window/summary selection and OpenRouter native HTTP/JSON parsing
together. Current and synthetic loaded-context generation each pass plain and
compact history: 25 events, exact floor 1075, one strictly older summary,
two identical replies retaining their listener, no hidden/aborted/future/mutable
log contamination. Wrong reader/lane reads are empty; retirement cancels the
stale provider request. Four positive HTTP cases per root, eight total.
Synthetic rows and session-local audit table are verified rolled back; fixture
listener stopped/socket closed. No product change or redeploy; HTTP200 and
server/client hashes reverified. Actual context ingress/main pipeline, real
models, maximum context sizes and gameplay are not proved by this fixture.
Broader roadmap and manual flat/VR acceptance remain open. Details and scratch
reproduction paths are in the retired parity ledger ([handoff](HANDOFF.md#retired-ledgers)).

## Native history window fidelity (2026-09-06)

LOCALLY DEPLOYED / SQL-PROVIDER AND GAME ACCEPTANCE OPEN: server 832c2f3 stops
upstream native history from collapsing equal-text utterances, merging replies
across listeners or dropping source timestamps. Native event boundaries reach
the configured history window and optional compact formatter intact; internal
timestamp metadata is removed afterward. Existing legacy grouping remains.
No retention change, database deletion or ownership-SQL change.
PHP 206/3036 plus compact-history 8/90, release audit and protocol 29/131/148 pass.
Source/installed actual builder/window/formatter fixtures cover limits
1/25/150/200 with exact row counts, listener text and memory floors. SQL rows
are mocked; short fixture timing is not a database/model/game benchmark.
Runtime PHP hash/schema/HTTP200 verified; config/14 voices and client fc2a484
unchanged. Actual SQL-to-provider integration, model context budgets, broader
roadmap and manual flat/VR gameplay acceptance remain open.

## Dialogue history and JSON prefill (2026-09-06)

LOCALLY DEPLOYED / REAL MODEL AND GAME ACCEPTANCE OPEN: server 42bc1a2 keeps
native plain replies separate with exact listeners/order across five JSON
connectors. Active dialogue JSON/prefill encoding handles quoted names/text,
backslashes, newlines and Unicode; structured/compact history stays intact.
OpenRouter no longer erases its first chunk when prefill is enabled; OpenAI
and Groq now seed their parser prefix. Five Kobold JSON templates use safe
history encoding; ChatML-C no longer drops assistant lines.
PHP 206/3036, release audit and protocol 29/131/148 pass. Eight five-connector
and 76 expanded Kobold fake-provider HTTP cases pass against both source and
installed runtime. Native cURL/response parsing is real; ownership/audit DB is
a fixture, not a model/context-size or gameplay proof.
Thirteen runtime hashes and HTTP200 verified; config/14 voice files and client
fc2a484 unchanged. No game launched or publication. Long-history/context,
owned continuation, broader roadmap and manual flat/VR acceptance remain open.

## Native Kobold template action evidence (2026-09-06)

LOCALLY DEPLOYED / REAL MODEL AND GAME ACCEPTANCE OPEN: server f9e345f preserves
native action/result evidence across all 18 shipped Kobold templates using
ID-paired narrated context. Full result/custom return wording survives null or
empty assistant content and final-tool histories; input history is immutable.
Duplicate/unpaired tool evidence is rejected. Alpaca examples/prefill encode
quoted names safely. Background chat path and native DLL are unchanged.
PHP 205/3018, audit and protocol 29/131/148 pass. 76 source plus 76 installed
fake-provider HTTP/cURL/stream-parser cases retain all 512 rows and exact text;
old installed llama3 failed the negative control. Ownership DB reads are mocked,
not real-model/context-budget or gameplay proof. Four runtime hashes and
HTTP200 verified; config/14 voices preserved. No game launched.
Ordinary assistant-history formatting, real model/context acceptance, broader
roadmap and manual flat/VR gates remain open. See the retired parity ledger ([handoff](HANDOFF.md#retired-ledgers)).

## Connector action JSON and result preservation (2026-09-06)

LOCALLY DEPLOYED / PROVIDER AND GAME ACCEPTANCE PENDING: server aa2404c fixes
five JSON connectors (OpenRouter, OpenAI-compatible, Google-compatible, Groq,
Player2). Action history/audit use safe shared JSON encoding with current-action
listener data. Native v2 results survive custom return templates without
#RESULT#; legacy template semantics remain unchanged. PHP 204/2994, release
audit, protocol 29/131/148 and 10 source plus 10 installed serialization cases
pass. Each outgoing fixture retains the entire 512-row result and exact quoted
action values (38,178-38,421 bytes). Transport is mocked: no actual provider
conversation or maximum-context proof. Six deployed hashes/health200 verified;
config/14 voices and client fc2a484 unchanged. No game launched.
Template-based connectors, real provider/context budgets, broader roadmap and
manual flat/VR acceptance remain open. See the retired parity ledger ([handoff](HANDOFF.md#retired-ledgers)).

## Owned action/tool follow-up pairing (2026-09-06)

LOCALLY DEPLOYED / PROVIDER AND GAME ACCEPTANCE PENDING: server f43adaf uses
stable session/generation/turn/context/action-owned native tool IDs, never the
shared last-call file. Assistant/tool IDs match; owner mismatches fail closed.
Arguments are JSON encoded, and CheckInventory reconstructs its saved item
filter rather than the NPC name. Legacy shared-file behavior is preserved.
PHP 203/2977, actual processor 27 checks (full 512 rows, interleaved owners,
retry, escaped values, mismatch, legacy), real SQL/binder, audit and protocol
29/131/148 pass. Installed PHP hashes and health200 verified; config/14 voices
and client fc2a484 unchanged. No game or real provider conversation invoked.
Connector serialization, provider budgets/conversations and manual gameplay/VR
acceptance remain open. See the retired parity ledger ([handoff](HANDOFF.md#retired-ledgers)).

## Full captured inventory action follow-up (2026-09-06)

LOCALLY DEPLOYED / PROVIDER AND GAME ACCEPTANCE PENDING: server 6c9b95d expands
successful CheckInventory results from the issued action's exact snapshot and
canonical actor. Saved literal name/FormID filter (or all captured rows), separate
stacks and observed quantity totals are retained through 512 rows. Partial means
lower bound; unavailable is not zero. Failed/cancelled actions remain failures.
No native/wire change: this replaces the 1 KB client preview only inside the
owned server follow-up. Normal prompt and bystander restrictions remain.
PHP 202/2959, real PostgreSQL/binder with later conflicting snapshot, release
audit and protocol 29/131/148 pass. Installed PHP hashes/health200 verified;
config/14 voices and client fc2a484 unchanged. Fixtures rolled back; no game or
provider conversation invoked. Actual action choice/follow-up, large provider
context limits, arbitrary actor/category queries and gameplay/VR acceptance
remain open. See the retired parity ledger ([handoff](HANDOFF.md#retired-ledgers)).

## Turn-owned inventory query selection (2026-09-06)

LOCALLY DEPLOYED / GAME ACCEPTANCE PENDING: server 4b7b1d0 selects up to 12
query-relevant principal rows from the same captured inventory (maximum 512).
Typed text and completed voice transcripts use the same exact-context binder.
Stable ranking preserves separate stacks/counts and capture order for ties.
Actor/playthrough and equipped-only bystander boundaries remain; omitted rows
do not imply absence. Native v2 legacy biography inventory is suppressed.
PHP 200/2910 plus inventory 5/11, release audit and unchanged protocol 29/131/148
pass. Actual helper->encoder->installed PHP selects row 512 intact; the old
installed implementation failed the same probe. Runtime PHP hashes and health
200 verified; config/14 voices preserved. Client fc2a484 unchanged; no game launch.
Broad/category/count questions, more than 12 relevant rows, capture limits and
flat/VR gameplay acceptance remain open. See the retired parity ledger ([handoff](HANDOFF.md#retired-ledgers)).

## Bounded native extended inventory (2026-09-06)

LOCALLY DEPLOYED / GAME ACCEPTANCE PENDING: flat capture512 rows,1024 entries,64 stacks/item; shared500us
structure and2ms metadata work-admission deadlines. One inventory try-lease;
metadata/sorting outside it; failed/incomplete observations remain truthful.
No cross-frame pointer cache or mixed-frame inventory assembly. VR remains32 rows.
Both DLLs/all17 suites, actual helper183415 checks in MSVC/GCCASAN/UBSAN,
old-helper33-row negative control and512-distinct-form capture->encoder->installed
PHP probe passed. Static34/packaging11 and protocol29/131/148 pass.
Clientfc2a484 deployed to SYNTH_dev with verified DLL/INI/PEX hashes and prior-build
backup; server runtime317f212 unchanged, health200. Both audited packages passed.
Game/editor process count0 before/after; nothing launched. Real native game timing
and save/load/message/headset proof remain unverified.
The principal prompt still exposes only12 rows: relevance and broader inventory
coverage remain open. See the retired parity ledger ([handoff](HANDOFF.md#retired-ledgers)) for limits and evidence.

## Extended inventory transport foundation (2026-09-06)

LOCALLY DEPLOYED / GAME ACCEPTANCE PENDING: v2 flat `context.inventory_512` and generation-owned init
acknowledgement support512 rows per actor; legacy and VR fall back to32, explicitly
partial when trimmed. The worker observes the actual1MiB encoded-byte budget without
altering immutable captures. Server ingress requires the initialized capability.
Both DLLs/all17 suites, fake-server500 with GCCASAN/UBSAN, PHP198/2855 and protocol
29 schemas/131 fixture cases/148 mirrored files pass. Native capture is STILL32;
prompt relevance/full inventory coverage and actual game acceptance are NOT complete.
Client5a1a1e9/server317f212 deployed; hashes, health200, config/voice/INI/PEX preservation
and runtime ingress/ACK probe verified. Large encoding no longer holds the readiness
state lock; synthetic worst-readiness delay215045us before versus46us after.
No game/editor launched. Full evidence and next steps:
the retired parity ledger ([handoff](HANDOFF.md#retired-ledgers)).

## Native player container-change notification (2026-09-06)

LOCALLY DEPLOYED / GAME ACCEPTANCE PENDING: client `a5917c3` adds an exact-runtime,
scalar-only TESContainerChangedEvent observer independent of actor-history settings.
Player-linked container notifications feed bounded dirty/debounce/ACK refresh;
they are not consumption, trade-history or witness claims. Original native handling
is forwarded once; old callback ownership is rejected across invalidation. Both
builds/all17 suites, lifecycle1534 plus sanitizers, actual callback probe21 and
static/protocol/package checks pass. Installed flat hash
158DC39520E98332539F83A6982D38036D581210C1744FCD0C561C553CC930F8; INI/PEX/server unchanged.
Manual notification coverage, full inventory beyond32 rows, generated names and
sustained flat/independent VR acceptance remain open. See native audit and goal ledger.

## Automatic request player inventory (2026-09-06)

LOCALLY DEPLOYED / GAME ACCEPTANCE PENDING: client `bd3f90b` enriches missing flat
player inventory only after selecting an actual request target. Automatic activation,
boredom, combat and observed event reactions now use this owned inventory path.
All other player/scene facts stay frozen. Native observation stamps fence capture,
cache reuse and enrichment; normal pick changes can establish a fresh capture.
Both DLL builds/all 17 suites, adapter 411 plus GCC sanitizers, static/protocol and
package audits pass. Installed flat hash ED5518C48BF94CC12225395550E91D549AAAF952076A165D79DAFDBF82B06CF9;
INI/PEX/server unchanged. Full inventory coverage, generated names, remaining native
dirty sources and manual flat/independent VR acceptance are still open.

## Flat player inventory refresh activated locally (2026-09-06)

LOCALLY DEPLOYED / GAME ACCEPTANCE PENDING: client `a0bfbf2` captures fresh player
inventory for dialogue plus an isolated menu-free, generation-owned background
refresh. Initial 2s / dirty 200ms / reconcile 30s / retry 2s; only actual context
and save-frontier ACKs satisfy delivery, and newer changes survive older ACKs.
Bootstrap/deep player factions/packages stay disabled. VR rejects the flat-only
refresh purpose and retains its existing capture path. Both builds/all 17 suites,
lifecycle 511 (also GCC sanitizers), adapter 380, exact gate 14 and static/protocol/
package checks pass. Installed flat hash FE811F0A5821DC807EED85FD13099DCD9201C0DA2745FE1A8278790730FD8411;
INI/PEX unchanged. Native add/drop/sell triggers, generated names, full inventory
beyond 32 rows, automatic AI inventory context and manual flat/VR proof remain open.

## Effective flat inventory statistics (2026-09-06)

BUILT / NOT DEPLOYED: client `9cb6380` replaces fabricated missing-instance zeros
with audited effective native weight/value fallback, preserving Survival rules.
Recursive COBJ valuation is excluded; supported magic costs require bounded,
validated effect lists. Invalid stats omit rows as partial. Both DLL builds/all
17 suites, 3,454 actual-helper/capture checks on MSVC and GCC sanitizers, old-capture
negative control and static/protocol/package audits pass. Runtime is unchanged.
Automatic player refresh, unresolved generated names, full inventory coverage and
manual flat/VR proof remain open. See the goal ledger for exact binary evidence.

## Borrowed inventory data and bounded read ownership (2026-09-06)

BUILT / NOT DEPLOYED: client1756761 corrects the previous built-only helper's
base-instance ownership (weapon/armor base data is embedded), bounds flat read
leases to one CAS attempt and rejects capture across native/runtime load epochs.
Both builds/all17 suites pass (adapter375),3198 metadata checks and126 lease
checks plus200000 threaded attempts pass on MSVC/GCC sanitizers. The old helper
fails the improved embedded-base probe. Protocol/static/package audits pass.
Installed actor-host build is unchanged; do not deploy the superseded919549b
archive. Ordinary-item weight/value semantics, generated names, automatic player
refresh, full inventory and manual flat/VR proof remain open. See the goal ledger.

## Flat inventory metadata lock correction (2026-09-06)

BUILT / NOT DEPLOYED: client919549b replaces two confirmed blocking native
inventory getters with a bounded extra-data try-read, copied cached names and
retained instance data. Busy, malformed or unresolved rows are partial rather
than guessed. Mixed-stack behavior survives the change. Both DLLs/all17 suites,
3201 exact-function probe checks on MSVC and GCC non-PIE address/undefined
sanitizers, static/protocol/package audits pass. Default PIE sanitizer execution
failed without a useful report; see the goal ledger. Installed build unchanged.
Strict worst-case try-lease/getter timing, unresolved generated names, automatic
player dirty/reconcile capture, complete inventory and gameplay acceptance remain
open. VR's separate native getters are unchanged and need independent validation.

## Inventory stack correctness (2026-09-06)

BUILT / NOT DEPLOYED: client 055ca63 keeps each stack's count, equipped state,
name, weight and value together. Different stacks sharing a base form no longer
borrow the first stack's metadata. Source bounds and try-lock scope remain;
equipped stacks have priority before the 32-row wire cap. Server 674f774 verifies
strict ingress/principal preservation and equipped-only bystander selection.
Both DLLs/all 17 suites, 2915 instrumented checks per compiler with GCC sanitizers,
old-helper negative control, real capture-to-PHP prompt probe, PHP 197/2821 and
protocol/package audits pass. Installed actor-host test build is unchanged.
Full flat-player inventory and dirty/reconcile delivery remain unimplemented;
the frozen Dialectic equip callbacks request inventory refresh, not generic AI
reactions. See the paired goal ledger for the evidence and next boundary.

## Flat actor host deployment (2026-09-06)

LOCALLY DEPLOYED / GAME ACCEPTANCE PENDING: client e247632 and server ed9fa08
connect exact guarded native capture, negotiated delivery and owned source history.
Fresh event-only captures, nonblocking exact actor resolution and immediate load
epochs protect scene ownership. Pip-Boy capture is separate from paused HTTP.
Both builds/all 17 suites, adapter 369, PHP 197/2816, 22 database checks,
protocol 28/129/145 and audited packages pass. Server schema 20260906004 and
HTTP 200 are verified; no game events received yet. Full deployment preserved
config/voices/job state and fixed dangerous operational-state/Git copy exclusions.
No in-game freeze, audible response or observer coexistence proof is implied.
Grounded reactions, NPC witnesses, complete inventory/action coverage and separate
VR bindings/acceptance remain open. See the paired goal ledger for exact artifacts.

## Negotiated actor-event delivery (2026-09-06)

SOURCE VALIDATED / HOST INACTIVE: explicit actor-support ACK, four-capture queue,
ordered fragments within the 16-state limit, immutable retry ownership, exact
retained actor resolution and PluginSession background delivery are implemented.
Neither proximity nor later visibility establishes witnesses. Actor contexts
require context_accepted before advancing; stale/ambiguous completions preserve
the correct fragment. Flat/VR builds and all 17 suites, context 418, fake-server
435, PHP 197/2815 and real queued C++-to-PHP binding pass. Protocol 28/129/145.
The host still uses the false default availability gate. No native observer
activation, local deployment or in-game acceptance is claimed. Broader gameplay,
inventory, reactions, NPC knowledge and separate VR runtime parity remain open.

## Actor-event contract and source history (2026-09-06)

SOURCE VALIDATED / INACTIVE: typed v2 actor batches, strict ingress and transactional
raw source retention now accompany timestamped scene binding. Only player self-
equipment becomes owned history. Raw NPC evidence is excluded from prompt
snapshots and is not witnessed memory. Strict retry comparison, conflict rollback,
save-frontier and active-generation isolation pass real PostgreSQL probes.
Both DLLs/all 17 native suites, context 287, PHP 197/2807, 22 database checks and
real C++-to-PHP binding pass. Protocol 28/125/143 remains mirrored; v1 unchanged.
No deployment or native activation. Negotiation, bounded delivery, exact actor
selection, gameplay reactions, inventory refresh, NPC knowledge and independent
flat/VR gameplay acceptance remain open; see the paired goal checkpoint.

## Timestamped actor-event scene binding (2026-09-06)

SOURCE VALIDATED / INACTIVE: client 59f1188 adds original callback timestamps,
immutable owned-scene binding and bounded nonblocking base-form metadata capture.
Session/generation/native epoch, player/playthrough, advancing capture frontier,
original two-second age and exact metadata scene are checked. Source evidence is
separate from player knowledge; nearby or later-visible NPCs are not event witnesses.
Missing secondary identity does not erase a nonzero native killer ID.
Both builds/all 17 suites, context 275, observer probes 170 per compiler, metadata
probes 20 per compiler, eight-million-attempt timestamped stress, static 34,
packaging 11 and protocol 27/116/133 pass. No host activation or deployment.
Negotiated delivery/history, inventory refresh, grounded reactions, NPC knowledge,
broader death sources, independent VR bindings and full manual acceptance remain.

## Guarded actor-event observer foundation (2026-09-06)

SOURCE VALIDATED / INACTIVE / NOT DEPLOYED: client e7392d1 compiles exact-flat
death/equipment/policy observers and a bounded deferred-payload mailbox. Original
native forwarding, nested order, contended completion, exceptional unwind and
load invalidation pass actual-header probes on MSVC/GCC (168 checks each).
Eight million pressure attempts are accounted; no torn payloads or stranded queue.
Both builds/all 17 suites pass (lifecycle 489), static 34, packaging 11 and protocol
27/116/133. The host never installs/arms/drains or advertises these observers.
Typed delivery/history, canonical identity, player-knowledge eligibility and
grounded reactions are still required before activation. Non-actor secondary
death sources and independent VR native bindings remain unresolved. No runtime
deployment or game acceptance claim; full parity remains open.

## Actor-event scalar identity foundation (2026-09-06)

FOUNDATION ONLY / NOT ACTIVE: client74a4ba1 adds an engine-free callback-scoped
identity collector using numeric results from existing native handle-policy calls.
Exact1.11.240 binary audit identifies policy slot7 and plain actor handle encoding;
instance/non-actor/invalid handles are rejected. New lifecycle coverage checks
nested callbacks, exception unwind, load epoch, conflicts and thread isolation.
Both builds/all17 suites pass (lifecycle428). DLL hashes are unchanged.
Death/equipment observers, typed delivery/history, knowledge eligibility and
activation are still required. No new deployment or gameplay claim this turn.


## Acknowledged player reactions (2026-09-06)

IMPLEMENTED / PAIRED LOCAL DEPLOYMENT / GAME ACCEPTANCE OPEN: client b75b45f and
server f74d8d0 connect scalar event history ACKs to fresh one-shot level-up/combat-end
responses. Explicit flat-only negotiation, independent bounded queue, original-age
expiry, current active speaker/player checks, original history/frontier ownership and
cross-context duplicate suppression are enforced. Native combat cues cannot assume
defeated enemies or witnesses. Existing RPG settings/chance/cooldown remain authoritative.
Both DLL builds/all17 suites, PHP196/2749, source+installed SQL51 plus cleanup,
quest25 and combat20, protocol27/116/133, static34 and package11 checks pass.
Server HTTP200 and schema20260906003 verified; flat installed hash and preservation
evidence are in the goal ledger. No game was launched. Manual flat and independent
VR gameplay gates remain open; this is not full feature parity.


## Native player-event delivery/history (2026-09-06)

SOURCE VALIDATED / NOT DEPLOYED: client 7849223 and server fe63680 connect
negotiated scalar events to fresh-context delivery and immutable owned history.
Strict ACK/serial receipts, retry identity, transactional duplicate/conflict handling,
save frontiers and no invented NPC hearing are covered by native/PHP/SQL checks.
Both builds/all17 suites, PHP195/2681, SQL23 plus cleanup, protocol27/107/124,
static33 and package audits pass. Schema20260906003 is prepared but not live.
Automatic reactions remain unfinished; sampled combat-end must not inherit legacy
defeated-enemy prompt assumptions. No in-game/VR proof or deployment is claimed.

## Native player transition capture (2026-09-06)

CAPTURE ONLY / NOT DEPLOYED: client 31af694 adds one-second scalar level/combat
sampling and a bounded 16-item FIFO independent of response work. Quiet baseline,
ordered immutable evidence, exact ACK identity, overflow and discontinuity handling
are covered by 154 context checks and 32 static gates. Both builds/all 17 suites and
audited packages pass. The queue has no network consumer yet; negotiated transport,
owned history/deduplication and AI reaction dispatch remain mandatory before deploying
this stage. Installed runtime remains the preceding combat-cancellation fix.
VR capture activation and all game acceptance remain open. See the goal ledger.

## Combat-entry lifecycle checkpoint (2026-09-06)

IMPLEMENTED / FLAT LOCAL DEPLOYED / GAME ACCEPTANCE OPEN: client 2d55b61
cancels only the current dialogue on combat entry, independently of combat dialogue
enablement. It preserves the session, active NPCs and observations; manual Hard Halt
is unchanged. Flat/VR builds and 17 suites per lane, fake-server 365, static 31,
packaging 11, protocol 26/98/114 and source/installed SQL cancellation 20 plus cleanup
pass. See the goal ledger for artifact hashes and manual tests. Combat-end/level-up
delivery is not yet implemented; this is not full gameplay parity.

## Previous regression-gate checkpoint (2026-09-06)

The three stale static failures reported by the quest-reaction checkpoint are now
repaired in the client test suite. All30 build gates pass and13 in-memory safety
mutations are rejected. Native flat17/VR17 suites, packaging11 and both archive
audits pass; no runtime behavior or installed artifacts changed. See the paired
goal ledger for exact scope, Claude Opus5 chatbox review and remaining acceptance.

## Owned automatic quest reactions (2026-09-06)

IMPLEMENTED / PAIRED LOCAL DEPLOYED / GAME ACCEPTANCE OPEN: v2 flat
negotiated reactions compare fresh owned tracked objectives, select an active eligible
NPC, and use the existing cancellable dialogue pipeline. Server rechecks both journals,
actor ownership/availability and duplicate admission before existing RPG policy/chance
and atomic cooldown. No new settings or schema migration. See QUEST-REACTIONS.md.
Both DLLs/all17 suites, PHP194/2630, protocol26/98/114, SQL25, history33, events27
and real PHP handshake11 pass. VR reaction activation remains disabled.

## Quest tracking evidence (2026-09-06, superseded by reaction checkpoint)

PAIRED LOCAL DEPLOYED / GAME ACCEPTANCE OPEN / REACTIONS INCOMPLETE: negotiated v2 flat
quest tracking now distinguishes true, false and unavailable throughout capture,
context, immutable history, prompt context and read_quests. Legacy/older-server
fallback and VR omission remain truthful. Both DLL builds/all17 suites,
PHP192/2580, protocol26/90/106, SQL33 plus native quest-events27 and init7 pass.
See the goal ledger for paired deployment and the remaining reaction dispatch.


## Negotiated flat quest capture (2026-09-06)

IMPLEMENTED / GAME ACCEPTANCE OPEN: exact1.11.240 scalar hooks are now connected
through guarded game-pump capture to owned history delivery, only after explicit
server init support. Older-server dialogue continues with capture off. Load/session
epoch checks, bounded drain/backpressure and separate health counters are wired.
Both builds/all17 suites, PHP189/2526, observer74 on MSVC/GCC, protocol25/85/100 and
packages pass. Actual in-game callback delivery/save-load stability and quest AI
reactions remain unproven/incomplete. See the paired goal ledger for deployment.

## Bounded client quest delivery (2026-09-06)

PARTIAL / ADAPTER ACTIVATION OFF: typed v2 client batches, four captured-batch
slots,128 owned acknowledged visibility witnesses, immutable retry retention and
background cancellation-aware publication are implemented. Both native builds/
all17 suites, context65, fake-server311, C++-to-deployed-PHP/SQL27 and Windows/Linux
200K-attempt stress checks pass. No current host enables the capability or invokes
the observer/delivery pump. Epoch/session binding, server-support negotiation,
native adapter activation, reactions and gameplay acceptance remain open.

## Native quest event server contract (2026-09-06)

PARTIAL: strict optional v2 flat batches now persist ordered native transitions,
with fresh displayed-objective visibility witnesses, exact save/player ownership,
batch dedup and transactional history/frontier updates. Source SQL27 plus existing
journal23/world53 checks, paired protocols25/82/99, both builds/all17 suites and
packages pass. Client native observers remain inactive; encoding/delivery/witness
retention/negotiation/reactions and gameplay acceptance are not implemented/proven
by this contract. See the latest paired goal ledger for deployment evidence.

## Ordered quest observer foundation (2026-09-06)

PARTIAL / CLIENT DEPLOYED / INACTIVE:8306197 adds guarded scalar quest-stage/start-stop
forwarders and a bounded FIFO preserving reservation order across nested completion.
Epoch/slot reuse and exception handling are validated; pressure loss is explicit.
No install/arm/drain call or native quest capability yet. Eligibility, owned transport,
save-frontier history and reactions remain open. Both DLLs/all17 suites, lifecycle400,
MSVC/GCC observer74 and2M concurrent attempts each, protocol and package audits pass.
Local deployment/hash verified after repairing the used helper's NTFS /IM copy omission;
INI/PEX unchanged, server HTTP200. No gameplay proof. See the goal ledger and audit.

## Native semantic-event binding audit (2026-09-06)

AUDITED / NOT IMPLEMENTED: exact flat1.11.240 concrete quest/combat/death/equipment
sink mapping is established (6 type/slot checks,13 instruction checks,10371 mapped
vtables scanned). Quest stage/start-stop scalar events are the next implementation
boundary; actor-pointer events require separate safe identity work. Ordered event
transport, player-visible eligibility, save-frontier history and permission-gated
reactions remain incomplete. See SYNTH docs/FLAT-SEMANTIC-EVENT-AUDIT.md.
Installed client d454b3e/server f26fcf1 unchanged, client hash/HTTP200 rechecked;
no new hook, build, deployment or gameplay claim. Full goal remains active.

## Fresh quest journal history (2026-09-06)

PARTIAL / PAIRED DEPLOYED: client d454b3e and server f26fcf1 publish fresh quest-only
changes through the existing idle context lane, then retain changed journals as
immutable player-owned history included in the same acknowledged save frontier.
Cached/unavailable state does not trigger new observations; partial absence does
not imply completion or abandonment, and no NPC hearing or AI reaction is invented.
A scoped journal index is installed by schema migration 20260906001.
Both native builds/all 17 suites, context 32, passive seams 90, source/installed
quest SQL 23 and world SQL 53 checks pass. PHP 187 tests/2,461 assertions,
protocol/release/package checks pass; installed artifacts and live data preservation
verified. Exact native quest events/reactions, actual gameplay and VR acceptance
remain open. See the retired parity ledger ([handoff](HANDOFF.md#retired-ledgers)) for hashes and full limitations.

## Historical: worker-side visual encoding (2026-09-06)

PARTIAL / CLIENT DEPLOYED: fc4e1ef moves JPEG encoding, hashing and upload preparation
off the game pump in both lanes. One visual job per session; original copied pixels,
context and actor ownership stay bound. Counter lifetimes cover cancelled/discarded
jobs, and cancelled encoding results do not upload. GDI synchronization, COM cleanup
order and exact JPEG stream length are corrected.
Both native builds/all 17 suites, native encoder 21 checks, exact-method seam 47 checks,
protocol and both package audits pass. Installed DLL hash and unchanged INI/PEX
verified. Server fe85d83 remains unchanged and healthy.
GDI readback/copy is still synchronous; game latency, real imagery, VR eye-view
accuracy and gameplay acceptance are unproven. This is not completion of the visual
capture architecture or the full parity goal. See the retired parity ledger ([handoff](HANDOFF.md#retired-ledgers)).

## Historical: bounded flat discovery (2026-09-06)

PARTIAL / CLIENT DEPLOYED: client9a089f2 caps flat process-list discovery at256
handle reads plus at most64 fresh cached-handle resolutions per capture. Numeric
cursors/aged handles only; no stale actor snapshots or retained engine references.
Partial scans never attest a complete audience. Exact manual picking stays separate;
automatic discovery can converge over successive captures. VR remains exhaustive.
Both DLLs/all17 suites, adapter358, extracted-code flat37705/VR34961 checks,
protocol and package audits pass; installed hashes and unchanged INI/PEX verified.
Server runtimefe85d83 unchanged/HTTP200. This is an operation-count bound, not
measured game latency or freeze-resolution proof. Full parity and manual acceptance
remain open; see the retired parity ledger ([handoff](HANDOFF.md#retired-ledgers)) for evidence and remaining work.

## Historical: native flat picked-reference selection (2026-09-06)

PARTIAL / CLIENT DEPLOYED: c6a6372 observes the exact-runtime native activation
pick through a guarded forwarding hook and revision/epoch-fenced atomic mailbox.
Game-thread resolution reserves the exact actor independently of nearest64 discovery.
Manual flat requests have no nearest fallback; automatic activation and VR pointing
retain their separate behavior. Both DLL builds/all17 suites, targeting110,
adapter285, actual-header seam23, protocol and package audits pass. Installed hash,
unchanged INI/PEX and server HTTP200 verified. Live event delivery, stationary and
obstructed picks, menu/save/load, text/voice and long-session acceptance remain
unproven. Discovery scanning is still unbounded; the full parity goal remains open.

## Historical: same-observation save history (2026-09-06)

PARTIAL / SERVER DEPLOYED: fe85d83 closes the passive checkpoint gap by including
the exact capture's own derived observations before admission commits. Older
acknowledged checkpoints never advance from later writers. Source and installed
SQL53 cover loaded-save ancestry, concurrent-producer exclusion, rollback and
failure; existing ownership/memory and saved-audience38 checks pass. PHP186/2446,
protocol, release audit, installed hash and HTTP200 pass. Checked live state and
client artifacts/config are unchanged. No backfill or new schema. Client runtime
remains 3c01cb1; native builds were not repeated for this server-only change.
Actual engine save/ACK timing, flat/VR dialogue and sustained gameplay acceptance
remain unproven. See the goal ledger; the complete parity goal remains active.

## Historical: passive world observations and native history (2026-09-06)

PARTIAL / PAIRED DEPLOYED: client 3c01cb1 publishes changed observed world labels
without requiring NPC activation or dialogue, using one idle-only background task.
Acknowledgement, cancellation and frame gates avoid duplicate or superseded passive
writes. Server 73d92b9 atomically archives v2 world/item/POI records with their exact
saved-context owner. Player-history retrieval works; no Narrator speaker or NPC
visibility is invented. Old server reproduced missing history; source/installed
SQL30, actual-method75, existing ownership/memory and saved-audience38 checks pass.
Both DLL builds/all 17 suites, PHP186/2446, protocol and release/package audits pass.
Local paired deployment/hashes/HTTP200 and unchanged checked live data/config/PEX
verified. This is sampled observation, not complete semantic event or gameplay
acceptance. Broader events, bounded scanning, targeting and the full roadmap remain.
See the goal ledger for evidence, preserved data and manual test boundaries.

## Historical: scoped per-pump scene capture reuse (2026-09-06)

PARTIAL / CLIENT DEPLOYED: a96c5da shares one immutable base scene among coincident
consumers within a serialized flat/VR pump. Requested actors and detailed inventory
remain per-request copies. Scope exit, load and actions clear the cache; bootstrap
cannot be reused and background cannot replace dialogue refresh. VR pose timestamps
are not renewed. Both DLL builds/all 17 suites, 275 adapter assertions per lane,
protocol and both package audits pass. Flat deployment/hash/unchanged INI/PEX verified;
server 8b2fd44 unchanged with HTTP200. The source scan itself is still unbounded:
this removes repeated captures, not total scan cost or the need for manual freeze/
gameplay acceptance. Full targeting/discovery separation and all roadmap gates remain.
See the paired goal ledger for operation-count evidence and the next architecture gate.

## Historical: captured audience completeness (2026-09-06)

PARTIAL / PAIRED DEPLOYED: client d019c35 and server 8b2fd44 carry complete/partial/
unavailable captured-audience metadata through both protocol versions, exact saved
text/audio context and the native prompt. Overflow, capture failures and export
filtering cannot claim complete observation. Old payload defaults remain conservative.
All four process tiers are still scanned; this is NOT bounded discovery, a measured
performance improvement or a freeze-resolution claim. Both DLL builds/all 17 suites,
34,961 selection checks per lane, protocol/schema checks, 185 PHPUnit tests/2443
assertions, source/installed 38 saved-context checks and both archive audits pass.
Server-first local deploy, hashes, HTTP200 and unchanged live data/config verified.
The custom INI and original PEX are unchanged. Manual flat/VM/VR acceptance and the
broader roadmap remain open. See the paired goal ledger for evidence and limits.

## Historical: optional embedding failure preserves accepted text (2026-09-06)

PARTIAL / SERVER DEPLOYED: df96a86 preserves generated summary text when optional
embedding fails, with the same shared boundary for diary enrichment. Cancellation
and final lease/load/source fences remain authoritative; generator errors still
retry. The previous worker reproduces lost text and retry in actual PostgreSQL.
Source/installed code pass 67 targeted checks; existing diary fixture passes 739;
selected PHPUnit passes 184 tests/2403 assertions. Both native builds/all 17 suites,
protocol and package/release audits pass. Server-only deployment/hash/HTTP200 and
unchanged live counts/config verified; no client deployment. Model identity/backfill,
script VM and flat/VR gameplay proof remain open. See the goal ledger for limits.

## Historical: owned Papyrus packaging and flat deployment (2026-09-06)

PARTIAL / FLAT ARTIFACT DEPLOYED: client 4d592be adds the strict all-or-none
source/flags/provenance/PEX bundle. Both lane packages pass; the deployment helper
re-audits staging and refuses removal of an installed bridge by a missing build.
The original 372-byte PEX and matching PSC are installed in SYNTH_dev; installed
binary verification and hashes pass. DLL/custom INI unchanged; server health 200,
no server deployment. Packaging 11 tests, 28 scratch bundle cases, 380 binary
negative cases, helper guards, both DLL builds/all 17 suites and protocol pass.
Actual VM calls, script consumer integration and flat/VR gameplay remain unproven.
This is not full parity; see the paired goal ledger for exact evidence and limits.

## Historical: reproducible native-only Papyrus artifact (2026-09-06)

PARTIAL / BUILD VERIFIED, NOT DEPLOYED: client 9836043 adds a pinned Caprica
v0.1.5 build and strict complete-binary verifier for the original native bridge.
Explicit ScriptObject inheritance and real local F4SE/vanilla binary imports
produce a deterministic 372-byte PEX with six global native methods and no
executable instructions. Two isolated builds match; 380 negative binary cases,
compiler/input rejection, both DLL builds/all 17 suites and protocol/package
audits passed. The existing packages still contain no PEX, and no game script was
installed. Next is the owned-script packaging/deployment allowance, followed by
manual VM/flat/VR acceptance. This is not a script-runtime or full-parity claim.

## Memory request identity and provenance (2026-09-06)

PARTIAL / SERVER DEPLOYED: af6c526 fixes queryMemory's native diary/legacy ID
collision, preserves exact saved revisions and stops inventing native text-only
vector distances. Legacy behavior and actual prompt injection remain unchanged.
Selected PHPUnit suites passed 183 tests/2392 assertions; actual helper/request
probe passed 52 checks against source and installed code, with the prior installed
helper failing. Synthetic archive/correction probes, both DLL builds/all 17 suites,
protocol and package/release audits passed. Server-only deployment, HTTP 200 and
hashes verified; client DLL/INI/configuration unchanged. Model identity/backfill
and full game/VR acceptance remain open. See the paired goal checkpoint.

## Corrected text recall alongside vector search (2026-09-06)

PARTIAL / SERVER DEPLOYED: server 350ecae keeps saved corrected dialogue and diary
text eligible when vector search succeeds, without reusing invalidated embeddings.
Independent semantic/text pools retain at most 50 each; lexical rank precedes
recency across sources, and final native selection still returns at most one.
Text-only distances remain null. The original correction probe failed before the
fix and passes now. Selected PHPUnit suites passed 183 tests/2370 assertions;
synthetic profile/diary fixture passed 739 checks; SQL pool probe passed 16 checks
against source and installed helpers. Both DLL builds/all 17 suites, protocol,
server release and flat/VR package audits passed. Server-only deployment, HTTP 200,
installed file hashes and unchanged DLL/INI/configuration verified.
Embedding-model identity/backfill, native_diary legacy-ID mapping and gameplay
acceptance remain unresolved. Client implementation remains a1d4cad. See the
paired goal checkpoint for evidence scope and remaining full-parity gates.

## Inventory lock scope and item lifetime (2026-09-06)

PARTIAL / CLIENT DEPLOYED: client a1d4cad copies bounded inventory structure and
retains first-stack ownership under the list try-lock, then releases it before
name/instance/form metadata calls and sorting. Custom instance information remains
supported; unavailable/partial observations remain explicit. Both DLL builds/all
17 suites, the exact-function instrumented lifetime/lock/bounds probe, its sanitizer
run and protocol/package audits passed. The prior helper fails the probe's lock
check. Installed DLL and unchanged INI verified; server unchanged, health 200.
This removes SYNTH's enclosing list lock but does not prove engine metadata calls
cannot block, freeze resolution, full inventory coverage or flat/VR gameplay.
Flat player deep capture remains disabled. See the paired goal checkpoint.

## Actor-bound OpenPrompt native bridge (2026-09-06)

PARTIAL / FLAT NATIVE BRIDGE DEPLOYED: client fd4ab9f adds OpenPrompt kind 5
and a sixth original Papyrus declaration. Exact actor identity and session
cancellation travel with a revision-bound UI draft; Send takes a fresh exact-actor
snapshot and rechecks identity, eligibility and work ownership. No crosshair fallback.
The render bridge rejects stale receipts and preserves one-shot handoff. Both DLL
builds/all 17 suites, 489 config assertions, 77 substituted prompt-method checks,
six source signatures and protocol/package audits passed. Client-only deployment
verified the installed DLL and unchanged custom INI; server unchanged, health 200.
Claude UI delegation failed without edits; Codex performed the bounded fallback.
VR prompt presentation, PEX compilation, native/script runtime calls, flat visual/
input/save-load behavior and full parity remain unproven. See the paired goal
checkpoint and SYNTH docs/NATIVE-EVENT-API.md for exact limits.

## Value-only Papyrus native bridge (2026-09-05)

PARTIAL / NATIVE BRIDGE DEPLOYED, SCRIPT OUTPUT PENDING: client 5a84022 adds five
global primitive-only SYNTHNative bindings and original PSC declarations. Calls
copy into the existing epoch-bound queue; no game reference crosses the callback.
Both real DLL builds/all 17 suites, focused input tests, 38 substituted-VM checks,
positive/five-negative signature audits and protocol/package audits passed.
The deployment skill verified the flat DLL and preserved custom INI; server
unchanged, health 200. No compiler was found in the checked install/PATH; location
was requested. No PEX/ESP was added. Reproducible Papyrus compilation, runtime VM
registration, script calls and separate flat/VR lifecycle acceptance remain open.
This is not a claim of usable or complete script integration.

## Same-owner external Ask admission (2026-09-05)

PARTIAL / AUTOMATED + CLIENT DEPLOYED: client a27b5be separates persistent
conversation ownership from outstanding actor work. Same-owner Ask can use the
ordinary turn-cancellation path while busy; another actor/narrator owner or
ambiguous work cannot be taken over. One-shot calls remain idle-only. Direct player
input establishes ownership; matching end-conversation releases it; continuations
retain their turn's owner. Both DLL builds/all 17 suites, focused owner tests,
56 exact admission-method substitute checks and protocol/package audits passed.
Installed DLL hash and unchanged custom INI verified through synth-full-deploy;
server code/runtime unchanged, health 200. Actual interruption timing, complete
conversation lifetime, third-party/flat/VR acceptance and full API parity remain
unproven. See the paired goal checkpoint and SYNTH docs/NATIVE-EVENT-API.md.

## Exact API actors outside nearest discovery (2026-09-05)

PARTIAL / AUTOMATED + CLIENT DEPLOYED: client 2dfce6f captures the caller-named
loaded actor directly through IFalloutRuntime and reserves a slot in the same
bounded scene, even when nearest-64 discovery omitted it. Canonical identity,
playthrough, generation/frame and loaded cell/worldspace guards remain; ordinary
snapshots and downstream exact actor ownership are preserved. Both DLL builds/all
17 native suites, 212 adapter assertions, 45 exact-function substitute checks and
protocol/package audits passed. The deployment skill verified the installed DLL
and preserved INI; unchanged server health is 200. Actual crowded-scene native
integration, pointer lifetime, sustained flat/VR gameplay and full public API
parity are not yet proven. Total process-list scan/frame-time bounds remain open.

## Native actor dialogue integration API (2026-09-05)

PARTIAL / AUTOMATED + CLIENT DEPLOYED: client cb85761 adds the version-1 native
SpeakExact/Comment/React/Ask API to both DLLs. Bounded copied requests enter the
safe game pump with freshness/epoch/busy and exact actor eligibility checks.
One-shot speech/comment/reaction cannot arm rechat or execute generated actions;
Ask retains normal conversation policy. Both Windows builds/all 17 native suites,
301 fake-server checks, 77 targeting checks, concurrent queue tests, 32 exact
response-method substitute checks, C ABI and PE exports, protocol/package audits
and 183 PHP tests passed. Whole-client GCC probe exposed an unchanged LP64 codec
ambiguity; not claimed passing. Client DLL hash and preserved INI verified through
the deployment skill; server runtime unchanged, health 200. Papyrus, OpenPrompt,
followers, same-actor Ask preemption, crowded-scene exact targeting, SDK packaging
and actual third-party/flat/VR gameplay acceptance remain open. See
docs/NATIVE-EVENT-API.md in SYNTH and the paired goal checkpoint for the contract.

## Faction extra-data traversal bound (2026-09-05)

PARTIAL / AUTOMATED + CLIENT DEPLOYED: client42cfb90 replaces flat's unbounded
extra-data lookup with a512-entry walk under the existing nonblocking read lease.
Capped/cyclic or inconsistent flagged lookup reports unavailable/empty. The pinned
flat head-pointer representation is copied with static layout guards; VR remains
base-only with no imported flat layout. Both builds/all17 native suites, exact
substitute probes flat4594/VR735, three non-PIE sanitizer runs per lane,359 isolated
SQL checks and protocol/package audits passed. Initial PIE sanitizer signal-loop
failure is documented separately, not claimed resolved. Client DLL hash verified
after the known copy fallback; custom INI and server runtime unchanged, health200.
Actual engine pointer safety, package semantics, remaining discovery bounds and
full flat/VR gameplay/parity acceptance are still unfinished.

## Nearby-actor selection storage (2026-09-05)

PARTIAL / AUTOMATED + CLIENT DEPLOYED: client628d587 uses a64-entry nearest-actor
heap and bounded identity set instead of retaining/sorting every candidate.
Finite stable-input nearest ordering matches the prior function in210 randomized
crowds up to10000 actors;34822 substitute checks per lane, both builds/all17 native
suites and protocol/package audits passed. No cross-frame game pointers added.
Client installed with hash verification after an explicit DLL-copy fallback for
the deployment helper's same-size Modified-file skip; custom INI unchanged.
Server code/runtime unchanged, health200. All process handles are still scanned:
retained storage is bounded, not total engine work or frame time. Remaining native
bounds, target/completeness design and actual flat/VR gameplay gates stay open.

## Actor-value observation availability (2026-09-05)

PARTIAL / AUTOMATED + BOTH DEPLOYED: clientcdc7ebd/serverd371317 carry independent
health/AP percentage availability instead of treating missing/invalid native reads
as100%. Old snapshots remain immutable; missing flags produce null in AI prompts,
and inspect-actor detail likewise uses null for unavailable readings. Observed0%
remains distinct. Both builds/all17 native suites,183 PHP tests/2356 assertions,
275 isolated SQL checks,801 broader checks and protocol/package audits passed.
Server-first rollout verified; live checks and custom INI unchanged.
Permanent-value denominator semantics, real engine reads, remaining observation
and full flat/VR gameplay parity acceptance remain unfinished.


## Faction completeness through capture, snapshots and prompts (2026-09-05)

PARTIAL / AUTOMATED + BOTH DEPLOYED: client4a685c1/server44216de carry separate
source mode and optional list completeness. Native omissions/output caps and
server prompt/description caps cannot retain complete status. Immutable snapshots,
current actor/NPC JSON projections, legacy labels and strict schema validation
preserve the distinction; missing older fields stay conservative without backfill.
Both builds/all17 native suites,182 PHPUnit tests/2242 assertions,359 isolated SQL
checks,801 wider regressions, protocol and both package audits passed. Server-first
local rollout verified; live data checks and custom INI unchanged. Actual game
faction semantics, traversal bounds, actor-value/package availability, full parity
and flat/VR gameplay acceptance remain unfinished.


## Nonblocking faction rank capture (2026-09-05)

PARTIAL / AUTOMATED + CLIENT DEPLOYED: client5cbff46 replaces waiting faction lookup
with a nonblocking read lease retained through rank copying. Removed per-base
IsInFaction engine calls; copied flat changes override/remove base memberships;
VR stays base-only.512-entry checks per source array precede iteration; failed
capture reports unavailable. Both native builds/all17 suites and both package
audits passed; exact-function substitute probes flat131/VR52 checks passed.
Client deployed; server unchanged. Engine extra-data-chain traversal, faction
output/prompt completeness and actual game behavior remain unproven/unfinished.
This is readiness evidence, not sustained freeze-free game or full parity proof.


## Protocol transaction commit integrity (2026-09-05)

PARTIAL / AUTOMATED + SERVER DEPLOYED: server70d03e7 rejects failed faction
metadata writes and verifies PostgreSQL's actual COMMIT command tag for request
acceptance, pending/terminal replay and guarded action intents. Real aborted
transactions no longer masquerade as successful persistence.59 isolated SQL checks
passed on source and installed code;181 PHPUnit tests/2047 assertions and801
profile/diary/environment checks passed. Live data digests and client DLL/INI
unchanged. Historical snapshot evidence remains separate from current factions.
Windows protocol peer check passed; WSL verifier has an existing order-only manifest
mismatch with identical hashes, recorded separately. Native/game/VR proof not newly
established. Full parity and remaining capture/gameplay acceptance stay unfinished.


## Selected NPC detail ownership (2026-09-05)

PARTIAL / AUTOMATED + CLIENT DEPLOYED: final selected NPC now receives same-frame,
game-thread inventory/faction/package enrichment before text/voice/activation/
bored/combat/PipVision worker admission. Nearest-NPC deep capture removed.
Frame/generation/identity and immutable merge guards pass both builds and137 adapter
assertions; all17 native suites and protocol/package checks pass. Client26db78f
deployed, server unchanged. This supersedes the prior nearest-detail-source gap
in source, not gameplay proof. Player capture, actor property semantics, discovery
bounds and real flat/headset behavior remain separate unfinished requirements.


## Selected-actor packet coverage (2026-09-05)

PARTIAL / AUTOMATED + CLIENT DEPLOYED: the exact selected NPC now receives the
first NPC context-state slot before bounded bystanders. Crowd cap, same-name,
missing/dead/wrong-identity and immutable ordering regressions pass in both native
toolchains. Client883c86f deployed; server unchanged. Deep inventory/faction capture
still enriches the nearest actor, not necessarily this selected NPC. That separate
capture-ownership gap, actor property semantics and actual flat/headset response
and save-load acceptance remain unfinished. See the paired goal ledger for hashes.


## Nearby reference observation integrity (2026-09-05)

PARTIAL / AUTOMATED + LOCAL DEPLOYED: nearby items and points of interest now preserve
fresh/partial/cached/unavailable evidence through bounded capture, immutable context,
derived events, prompts and inspect_surroundings. Caps downgrade completeness.
Flat/VR builds,17 native suites,179 PHP tests and801 synthetic checks pass; paired
flat/WSL deployment and18 installed-code cases verified. See the goal ledger for
commits and hashes. Current-cell-only scope, per-object property semantics, actor
coverage and real flat/VR timing/save-load acceptance remain separate requirements.
Full behavioral/architectural parity is not complete.


## Quest observation integrity checkpoint (2026-09-05)

PARTIAL / AUTOMATED + LOCAL DEPLOYED: bounded nonblocking quest capture carries
collection and objective quality through immutable context, prompt and read_quests.
Cached/unavailable reads cannot clear the projection or claim fresh action success.
Partial lists update observed identities only. Flat/VR builds, native/protocol/PHP,
779 SQL/loopback checks, and installed-code probes pass. Flat client and WSL server
deployed; live quest records and custom INI preserved. See the paired goal ledger
for exact commits, artifact hashes and evidence. VR objective completeness remains
unsupported by pinned headers. Manual flat/headset response and save-load stability
are not proven; full feature/architecture parity remains active.


Historical alignment baselines captured 2026-08-30: CHIM unstable
`165b21c11f5005ca270f4711bc1c3b8770571902` and HerikaServer unstable
`3a5b79e262c2a9256fa8af1c67fa0205dd23e7d9`. Dialectic and DialecticServer remain the protocol
lineage recorded in the provenance ledgers. This is an evidence ledger; source-connected work is
not promoted to flat or VR in-game proof.

The approved 2026-09-04 goal now targets Dialectic
`c6e92f375a44a680affc22c8ba378320650b92da` and DialecticServer
`907eb634fc49b47abe3490ea8229977ad0b0f6be`; see the retired parity ledger ([handoff](HANDOFF.md#retired-ledgers)).

Status vocabulary: `SOURCE CONNECTED`, `PARTIAL`, `AUTOMATED`, `WINDOWS BUILD PROVEN`,
`CAPABILITY DISABLED`, `APPROVAL REQUIRED`, `IN-GAME PROVEN`, `VR IN-GAME PROVEN`,
`DEFERRED ESP`, `OUT OF SCOPE`.

## Current runtime corrections (2026-09-04)

The approved work and latest evidence are tracked in the retired parity ledger ([handoff](HANDOFF.md#retired-ledgers)).
Historical rows below are not proof that their full capture or game behavior is enabled.
In the current freeze-safe flat adapter, player inventory/faction/package details are
not captured; detailed nearby inventory is bounded to one actor, not sixteen. Other
actor observations can be shallow. The 2026-09-05 inventory checkpoint now carries explicit
complete/partial/unavailable quality through saved prompts and action responses, with bounded
entry/stack traversal. Skipped capture is not observed empty; deep player capture and gameplay
validation remain open. Native v2 now
retains acknowledged generation/sequence bindings through text, voice, rechat and action
continuations. Targets are frozen at admission; context publication uses delivery order,
not frame order. Profile refresh and PipVision description publish and bind their game
snapshot. PipVision's server-side image selection still needs capture-ID ownership.
Whole-stream validation precedes callbacks. Native and loopback SQL checks do not mark
F02/F03/F05/G06 or any in-game/VR row complete; turn preemption and lifecycle soak remain.

## End-to-end flow conformance

Native diary checkpoint (2026-09-05): manual NPC/Player/Narrator and flat sleep/wait batches
are source-connected with configured server workers and acknowledged save checkpoints.
Saved recall, author discovery, archive/restore and immutable corrections are locally deployed.
Full-text export and a printable standalone book now pass synthetic SQL/HTTP/browser checks;
the server API keeps exact actor/role/snapshot/filter ownership and excludes internal evidence.
Server71918ed now passes301-journal generation/export and483 reads in a60-second synthetic
soak after sharing source visibility work. Larger, many-author/control-heavy sustained histories,
historical embedding backfill and manual flat/VR gameplay remain open.
These statements supersede earlier diary gaps in historical rows, not their gameplay gates.

Diary vector source provenance (2026-09-05) now binds new server embeddings to accepted content
and preserves legacy checkpoints without assuming their vector provenance. Unknown/mismatched
vectors are excluded while valid text remains readable. Model-version compatibility, historical
backfill and actual gameplay remain unproven; no client capability change is implied.

Native management now has a deployed saved-snapshot inspector, paginated read API and
revisioned/audited Archive/Restore and immutable summary corrections (2026-09-05).
Corrections preserve original/frozen evidence, hide outdated descendants, cancel their
jobs and use text search without reusing old vectors. Hidden inputs retain coverage.
Populated edits, 71-summary archive/restore and concurrency are synthetic evidence;
the live stack still has zero native histories/summaries. Automatic revision-aware
rebuilding now preserves original evidence and family-wide archive choices; 70 rebuilt
descendants yield 71 active memories in synthetic SQL. Hidden manual corrections now
have explicit one-input regeneration approval with retained history. Keeping/merging the
correction while rebasing, embedding provenance/repair, retention and gameplay production
remain open. The shared-query slowdown was traced to repeated actor/audience resolution
inside memory-source probes. Per-query visible-ID caching and grouped source checks now
yield about 4 ms for 71 memories, 48 ms for 301, and 12 ms for 70 replacements/71 active
memories in synthetic SQL. Larger-history/service soak and in-game latency remain unproven.
Controls do not erase in-flight prompts.

Relationship isolation is not inherited from the memory work. A synthetic actual-SQL
audit confirms relationship reads still use global profile names without native claim,
context or saved-frontier guards. The player-alias parser repair is deployed, but the
native pair/timeline journal now has deployed, SQL-tested groundwork. Completed processed
model speech is also captured with exact response-body/context/lane ownership, without
speaking relationship commands. Optional storage failure preserves completed dialogue;
this does not prove physical hearing. An internal inline evaluator now applies exact
completed-response commands transactionally using frozen ingress/processed-input dependencies,
without widening saved reads; actual SQL proves later visibility, replay and multi-subject
rollback. Dedicated input freezing and bounded durable leases are now SQL-tested too,
including two-process claims and expired-token rejection. An idle provider callback now
feeds strict bounded parsing and atomic result/pair publication with all frozen prior
dependencies, durable no-change, ready replay and restored-save visibility checks.
Configured provider execution now restores the persisted owner and passes all five
supported drivers against loopback fixtures. Discovery/retry/frozen recovery and actual
killed-process recovery are tested, but the installed manager task stays explicit-only
until the coordinated native hook/control cutover. Inline/unclassified responses are
excluded from dedicated backlog using the mode frozen in accepted response evidence.
Versioned dedicated policy now preserves raw-delta notes/best/worst, role detail and
neutral-only type evolution, including native wary. SQL proves metadata inheritance,
frozen-prior integrity, old-policy replay and saved-note visibility.
Native actor locks now have an audited exact-scope management API. A shared actor gate
and canonical-request sequence barrier cancel queued/running evaluation work and reject
pre-change responses after unlock, without hiding accepted saved history. Real concurrent
admin writers, atomic audit rollback, endpoint CSRF/revision checks and actual slow-HTTP
cancellation pass. The management screen/manual edits and legacy-lock migration policy
remain open; this backend API does not control the legacy game-facing path.
Accepted response.v3 now records each line's final canonical addressee/utterance ID after
actual strict/rechat routing, with unavailable ambiguity and unchanged legacy v2 evidence.
Dedicated prompts preserve addressing without calling it hearing. Actual returnLines/SQL
probes pass; independent listener-owned evaluation/publication is still required.
An internal routed-pair facade now reads each NPC's saved perspective independently,
using explicit canonical contextual-NPC scope while leaving default history/memory and
request ownership unchanged. SQL proves +11/-7 scores, dedicated-result notes, dependency
rejection, same-name renames and independent saved forks. It is not yet connected to the
evaluator or gameplay hooks; frozen bidirectional execution/publication remains open.
Neither path is called by game-facing hooks yet. Game-facing prompt,
inline/dedicated-worker and management cutover remains required; see the paired server's
`docs/NATIVE-RELATIONSHIP-MIGRATION.md` for the verified gaps and gates. New unused helpers
do not make the existing legacy relationship path isolated.

These rows prevent individually implemented subsystems from being mistaken for a working CHIM/
Dialectic-style product. The reference sequence and ownership are in
`REFERENCE-STACK-DATAFLOW.md`.

| ID | Required integrated flow | Required evidence | Status |
| --- | --- | --- | --- |
| F01 | Exact DLL load -> config/discovery -> Synthserver init/capability negotiation -> observable readiness | Flat/VR fake server, then separate extender/in-game logs | SOURCE CONNECTED (both DLLs and strict init client build; extender logs pending) |
| F02 | Fresh target/player/audience/world snapshot -> typed state event -> persisted server context/UI readback | Cross-repo fixture E2E plus flat/VR in-game capture | PARTIAL (canonical player/target/audience, bounded actor/inventory/faction/activity/package/active-quest/nearby-item/POI state, native world facts, flat actor LOS, and exact loaded-plugin indices persist through strict state; UI route and in-game readback pending) |
| F03 | Typed/PTT input -> optional STT -> profile/context/memory prompt -> fake/real LLM -> NDJSON dialogue -> subtitle/audio/lipsync | Offline full-stack E2E plus separate flat/VR in-game turn | SOURCE CONNECTED (clipboard text, independent WinMM PTT/open mic, binary STT, real server pipeline, NDJSON, deferred TTS/XAudio2 playback, and game-thread WAV-envelope jaw animation; in-game pending) |
| F04 | Allowlisted action intent -> capability/live-identity check -> game-thread execution -> one correlated terminal result -> optional follow-up | Read-only action fake-runtime E2E, then each enabled action in game | AUTOMATED (inspect actor/surroundings, bounded inventory and active-quest actions use captured canonical state plus correlated result continuations; in-game execution pending) |
| F05 | New turn/halt/save/load/main menu -> generation advance -> network/media/action cancellation -> stale response rejection -> clean re-init | Fault-injection/lifecycle E2E plus flat/VR recovery matrix | AUTOMATED (fake lifecycle cancellation/re-init and stale completion tests; game recovery pending) |
| F06 | Accepted turn -> post-commit memory/relationship/profile jobs -> worker retry/idempotency -> later bounded context retrieval | Server worker/restart tests and later dialogue retrieval proof | PARTIAL (native memory capture/dispatch, providers, recall, deduplication and actual killed-process lease recovery pass synthetic probes. Manager database isolation and real native child idle completion verified. Full multi-task restart/soak, relationship/profile lineage and gameplay proof remain pending) |

## Client foundation

| ID | Requirement copied/adapted from current Dialectic | Initial target | Required evidence | Status |
| --- | --- | --- | --- | --- |
| C01 | Native x64 F4SE/F4SEVR load/version contracts | Separate exact flat and VR artifacts; reject the other runtime | Both Windows builds + corresponding extender logs | WINDOWS BUILD PROVEN (extender logs pending) |
| C02 | Central logging and redacted diagnostics | `SYNTH.log`, request IDs, no secrets/prompt dumps by default | Unit + redaction scan | AUTOMATED (structured bounded records, request IDs, payload suppression and secret/URL redaction) |
| C03 | Shipped defaults and user override INI | `SYNTH.ini` shipped defaults + ignored `SYNTH_custom.ini` override, written by the native F4SE Menu Framework pages, hand edits, or external tools | Precedence/path tests + settings-catalog contract | AUTOMATED (all 42 baseline Dialectic settings plus End Conversation Cooldown are persisted, range-validated, live-reloaded and wired; INI fallback and override precedence are tested) |
| C04 | Runtime generation | Save/load/new game/main menu/shutdown invalidate work | Unit + fake lifecycle | AUTOMATED (generation/cancellation/fake lifecycle; real adapters pending) |
| C05 | Immutable runtime snapshots | No engine/HMD/controller pointer crosses the selected adapter | Static audit + flat/VR fake runtime | AUTOMATED (engine-free owned snapshots and flat/VR fakes) |
| C06 | Bounded task manager | Lanes, priorities, deadlines, cancellation, health, joined shutdown | Stress/unit tests | AUTOMATED (named finite lanes, deadlines, reserve, health, exceptions and joined shutdown) |
| C07 | Game-thread dispatcher | Bounded typed commands and ownership assertion | Unit/integration | AUTOMATED (bounded generation-aware fake-runtime dispatcher) |
| C08 | Strict HTTP/JSON transport | Loopback default, version, size/time limits, cancellation | Fake-server E2E | AUTOMATED (WinHTTP source plus strict scripted timeout/cancel/loss/restart tests) |
| C09 | NDJSON/streamed response queue | Exact request/generation, dedupe, unfinished state | Fixture/E2E | AUTOMATED (shared byte-identical fixtures, native codec, generation/dedupe/bounds/terminal tests) |
| C10 | Deterministic packaging | Separate flat/VR DLL/config/interface/docs manifests/checksums | Per-target release-tree audit | WINDOWS BUILD PROVEN (both real DLL archives pass forbidden-tree audits) |

## Input, dialogue, and presentation

| ID | Requirement | Initial target | Required evidence | Status |
| --- | --- | --- | --- | --- |
| D01 | Crosshair/HMD-gaze/controller-ray and nearest eligible actor targeting | Native FO4 identity; HMD-effective origin in VR | Unit + flat/VR in-game | AUTOMATED (native capture and geometry tests; in-game pending) |
| D02 | Manual target/nearby activation and removal | One authoritative agent registry | Unit + in-game | SOURCE CONNECTED (strict persisted activate/halt; in-game pending) |
| D03 | Automatic nearby activation policy | Humans/ghouls/super mutants/robots configurable; creatures opt-in | Unit + in-game | SOURCE CONNECTED (distance, live/dead/disabled, hostile and native race-editor-ID policy connected; creatures remain explicit opt-in; in-game classification proof pending) |
| D04 | Group conversation audience | Spatially eligible active agents; HMD-relative in VR | Fake snapshots + flat/VR in-game | SOURCE CONNECTED (bounded canonical audience; in-game pending) |
| D05 | Typed text input | Native hotkey/UI path without ESP | Windows/in-game | WINDOWS BUILD PROVEN (bounded Unicode clipboard hotkey fallback; visual chat UI pending) |
| D06 | Push-to-talk and microphone selection | Flat and VR native hotkeys; bounded recording | Device/input fakes + flat/VR in-game | WINDOWS BUILD PROVEN (WinMM PTT builds in both lanes; controller-semantic input is not advertised) |
| D07 | Open-mic VAD | Disabled by default, sensitivity/end-delay/mute | Unit + in-game | SOURCE CONNECTED (bounded VAD, rollover and runtime mute; persistence/in-game pending) |
| D08 | STT upload and cancellation | Strict metadata and generation | Fake STT + in-game | SOURCE CONNECTED (native bounded WAV upload and server STT pipeline; live provider/in-game pending) |
| D09 | NPC TTS playback queue | Correct actor, order, interruption, cache | Fake audio + in-game | SOURCE CONNECTED / LOCALLY DEPLOYED (dedicated FIFO speech delivery prevents faster later audio overtaking; independent network/control, fixed deadlines, cancellation, hash validation, XAudio2 queue and text fallback; audible in-game order/stability pending) |
| D10 | Player TTS/respeech | Server-controlled optional path | Server/client E2E | SOURCE CONNECTED (inherited rewrite/TTS worker emits the committed canonical player identity through strict speech jobs; client text, volume and playback path connected; live-provider/in-game proof pending) |
| D11 | Spatial/3D voice | Actor source; player-root listener flat and fresh HMD listener VR | Math tests + flat/VR in-game | SOURCE CONNECTED (actor source, camera listener flat, fresh HMD listener VR, pan/attenuation; in-game pending) |
| D12 | Passive subtitles | Runtime-owned HUD/VR UI experiment, no blind SWF replacement | Flat/VR in-game | PARTIAL (HUD notification delivery and toggle build; in-game presentation proof pending) |
| D13 | Facing and lipsync | Runtime-specific game-thread path; fail visibly if unsupported | Flat/VR in-game | PARTIAL (flat and VR builds apply bounded native WAV-envelope jaw motion to the exact speaking FormID at configured resolution/intensity and restore prior state on every exit; facing and in-game visual proof remain pending) |
| D14 | Vanilla dialogue capture | Capture-only path never invokes AI by itself | Fixtures + in-game | APPROVAL REQUIRED (no stable capture event exists in the pinned APIs; adding a dialogue-menu hook or content bridge requires separate design and authorization) |
| D15 | Dialogue suppression/restoration | Scoped to active AI line; deterministic restore | In-game interruption matrix | CAPABILITY DISABLED (SYNTH uses its own HUD/audio path and deliberately never takes ownership of vanilla dialogue, so there is no vanilla state to suppress or restore) |
| D16 | Menu/Pip-Boy/combat/VR overlay gates | Configurable pause/cancel policy; optional FRIK UI state | Lifecycle tests + flat/VR in-game | WINDOWS BUILD PROVEN (menu-mode pauses XAudio and new work; scene safety excludes vanilla speakers; combat entry cancellation and combat-dialogue policy are connected in both adapters; FRIK-specific state is not advertised) |
| D17 | Hard halt | Cancel HTTP/STT/TTS/queues/actions and restore presentation | E2E + in-game | SOURCE CONNECTED (all local lanes clear, strict server halt, fresh opaque-session recovery; in-game pending) |
| D18 | Rechat, bored events, auto greeting and combat barks | Bounded, cooldown-aware, no recursive storm | Unit/server E2E + in-game | SOURCE CONNECTED (drained-queue rechat, wall-clock bored events, transition-based greeting with server-side persisted-policy/game-time authorization, and live-state combat barks with ordinary-history retention; in-game pending) |

## Game context

| ID | Requirement | Fallout 4 adaptation | Required evidence | Status |
| --- | --- | --- | --- | --- |
| G01 | Player/world context | Sole Survivor, Commonwealth, location, cell, worldspace, weather, date/time; HMD pose/handedness capabilities in VR | Schema + flat/VR in-game | SOURCE CONNECTED (canonical player plus native location/cell/worldspace/weather/interior/game-time facts use one strict context path; in-game accuracy pending) |
| G02 | Nearby actors | Identity, race, sex, voice, position, dead/disabled, combat, teammate | Snapshot + in-game | SOURCE CONNECTED (flat and VR capture canonical identity plus bounded race/sex/voice/position/live/disabled/combat/hostile/sneaking/level/teammate/life-state state; strict server persistence and in-game proof pending) |
| G03 | Activity/condition | Health/AP, combat, package/activity, equipment | Snapshot + in-game | SOURCE CONNECTED (health/AP availability, combat, equipped items, life state, sit/sleep posture, weapon-drawn state, movement speed/sprint, talking-to-player, power armor and currentProcess.currentPackage identity flow through strict context/prompt state in both lanes; effective/run-once package precedence, percentage semantics, in-game accuracy and action restoration use remain pending) |
| G04 | Nearby items | Base/ref IDs, name, count/value/weight, stealing, crosshair/held priority | Snapshot + in-game | SOURCE CONNECTED (bounded current-cell scan carries runtime reference, base and cell identities, name, per-reference count, value, weight, type, crime-to-activate, camera/HMD looking-at and held state for up to 32 items through strict context, canonical prompt persistence and InspectSurroundings; stacked loose-reference counts and in-game accuracy remain pending) |
| G05 | Points of interest | Doors, locks, map markers, locations | Snapshot + in-game | SOURCE CONNECTED (up to 16 nearest doors, containers, activators, flora, furniture, terminals and map markers carry runtime/base/cell identities, kind, position, distance, lock and camera/HMD looking-at state; current location/cell/worldspace remains supplied by world context; in-game accuracy pending) |
| G06 | Inventory/equipment | Player and actor inventories, equipped state, ammo/caps | Snapshot + in-game | PARTIAL (flat keeps player inventory unavailable to preserve the save/load freeze boundary; nearby discovery is shallow and only the selected NPC receives bounded inventory enrichment. VR non-bootstrap player capture includes inventory. Strict persistence, observation quality and CheckInventory are connected; complete player/actor coverage and in-game accuracy remain pending) |
| G07 | Active quests/objectives | Read-only native quest journal first | Snapshot + in-game | SOURCE CONNECTED (up to 16 tracked or displayed quests carry canonical form/plugin identity, title, editor ID, stage and active-objective count through strict context, playthrough-correlated canonical quest persistence and ReadQuests; flat also carries up to eight displayed objective texts, while the pinned VR headers expose objective identity/count but leave state/text opaque; in-game accuracy remains pending) |
| G08 | Loaded plugins | Names, indices/light indices where applicable | Unit + in-game | SOURCE CONNECTED (flat and VR enumerate the active compiled file collections and atomically replace the persisted server manifest; in-game load-order proof pending) |
| G09 | Factions/relationships | FO4 factions, affinity/companion state where safely observable | Snapshot + in-game | PARTIAL (flat captures up to 32 effective base plus runtime-added/changed faction memberships with canonical form/plugin identity and rank; VR captures active base memberships and explicitly reports `base_only` because its pinned CommonLib surface does not expose runtime-added records/ranks; strict prompt/profile persistence is connected, while native companion affinity/relationship rank and in-game accuracy remain unclaimed) |
| G10 | Spatial awareness | Distance/LOS first; HMD-effective VR origin; nav/path only after bounded native design | Math + flat/VR perf/in-game | PARTIAL (actor/item/POI distances and bounded camera/HMD ray proximity are connected; flat actors additionally use the native player LOS query, while the pinned VR surface does not expose an equivalent and reports unknown; nav/path and in-game proof pending) |
| G11 | Voice samples | Never ship extracted Bethesda audio; local discovery/upload only | Release audit + in-game | WINDOWS BUILD PROVEN (2026-09-04: background loose/BA2 v1 discovery, generation-checked import, flat Tools batch/cancel, and preserved manual samples; native HTTP + WSL PocketTTS produced WAVs for Danse/Preston/Piper; no audio is packaged; candidate quality, MO2 overrides, save-load interaction, and flat/VR playback remain manual checks; see VOICE-CLONING.md) |
| G12 | Fallout 4 world data | Separate import datasets with provenance, no FNV/TTW rows | Dataset audit | AUTOMATED (FO4 seed datasets and source ledger pass server release audit) |

## Native actions

Every action requires an allowlisted typed payload, exact speaker/target resolution, current-scene
validation, game-thread execution, timeout/cancellation, and a structured result. Unsupported
actions return `unsupported_without_esp` or `unsupported_runtime`; they never claim success.

| Group | Dialectic actions | Initial disposition | Status |
| --- | --- | --- | --- |
| Read-only | `Inspect`, `InspectSurroundings`, `CheckInventory`, `ReadQuests` | Native first | SOURCE CONNECTED (all four are capability-advertised and use the fresh bounded native snapshot; flat/VR in-game proof remains pending) |
| Presentation | `SheatheWeapon` | canonical actor lookup, game-thread execution, generation cancellation and verified terminal state in flat/VR | SOURCE CONNECTED; in-game proof pending |
| Presentation | `EndConversation` | drained SYNTH rechat cancellation and configurable reactivation cooldown without altering vanilla dialogue | SOURCE CONNECTED; in-game proof pending |
| Presentation | `Talk` | Normal streamed dialogue, not a mutating runtime action | SOURCE CONNECTED (handled by the dialogue/subtitle/audio pipeline and never advertised as an action capability) |
| Equipment | `EquipItem`, `UnequipItem` | Exact observed inventory binding, native instance/stack validation and equipped/count postconditions | FLAT SOURCE CONNECTED AND DEPLOYED (27c8515); manual equipment/outfit/save-load proof pending. VR CAPABILITY DISABLED until safe instance access exists. |
| Consumption | `Consume` | Exact ALCH instance, synchronous dispatch, verified decrement and owned inventory receipt | FLAT ENABLED AND LOCALLY DEPLOYED (ee8a192 + compatible server ddda7d4); manual effects/save-load proof pending. VR DISABLED. |
| Transfer | `GiveItemTo` | Distinct canonical donor/recipient, exact selected stack and both inventory deltas | FLAT LOCALLY DEPLOYED: client2a20aaa and compatible serverdd9a22c; matched ACK, strict issuance and actual-main synthetic race/continuation checks pass. Manual conservation/save-load acceptance remains open. VR DISABLED. |
| Currency | `GiveCapsTo` | Exact caps identity, integer amount and paired conservation | MATCHED FLAT LOCALLY INSTALLED (client fdbffe4, server caps/rechat ee834a4/82a8a16). Exact ACK, dispatch, paired receipts and synthetic compatibility/races pass. Actual game conservation and save/load acceptance remain open. VR DISABLED. |
| Currency | `TakeCapsFromPlayer` | Exact player donor, NPC recipient and paired inventory evidence | MATCHED FLAT LOCALLY INSTALLED (client08f5d2d, compatible servercd59050/c020de9; maintained contract378c7e1). Distinct ACK, dispatch and paired immutable receipts pass synthetic payment/race checks. Actual conservation/save-load acceptance pending. VR DISABLED. |
| Inventory | `OpenInventory`, `PickupItem` | Native only after exact delta and theft/menu semantics are proven | CAPABILITY DISABLED. Flat world-item capture b6d29a6 is deployed; pickup binding/conservation helpers435e5b7 are source-only. Native approach/transfer/retirement/restoration, matched receipt and OpenInventory menu semantics remain incomplete. No gameplay or inherited VR proof. |
| Movement | `ComeCloser`, `MoveTo`, `Follow`, `FollowPlayer`, `StopFollowing`, `StopWalk`, `WaitHere`, `TakeASeat`, `TravelTo` | Package-backed operations need SYNTH-owned forms | DEFERRED ESP (not advertised; requires the separately authorized content phase) |
| Combat | `Attack` | Native only with original combat-state restoration | CAPABILITY DISABLED (not advertised; safe cancellation/restoration is not proven on either runtime) |
| Trade | `Barter` | Native menu/inventory experiment | CAPABILITY DISABLED (not advertised; opening or mutating Fallout menus is deliberately unavailable) |
| Follower conversion | `MakeFollower` | Requires FO4 companion/package/faction design | DEFERRED ESP |
| Walk speed | `IncreaseWalkSpeed`, `DecreaseWalkSpeed` | Omit unless a safe reversible FO4 implementation is proven | CAPABILITY DISABLED (not advertised; SYNTH does not modify actor movement state) |

## Fallout 4 VR first-class requirements

These rows are additional acceptance work, not optional features. No row inherits proof from flat
Fallout 4. The detailed dependency, stack, performance, and evidence plan is in
`FALLOUT4-VR-PLAN.md`.

| ID | VR requirement | Initial target | Required evidence | Status |
| --- | --- | --- | --- | --- |
| V01 | VR-specific native build | `SYNTHVR.dll`, F4SEVR `0.6.21`, runtime `1.2.72`, pinned CommonLibF4VR/VR Address Library | Clean Debug/Release build, package audit, F4SEVR log | WINDOWS BUILD PROVEN (F4SEVR log pending) |
| V02 | ABI/address isolation | No flat CommonLib/offset/layout/hook leaks into VR adapter; both DLLs reject the other runtime | Static/audit tests + wrong-runtime load evidence | WINDOWS BUILD PROVEN (separate CommonLibF4VR target/package; wrong-runtime in-game evidence pending) |
| V03 | HMD-effective player pose | Fresh HMD position/orientation for gaze, distance, audience, listener and facing target | Transform/freshness unit tests + room-scale/seated in-headset | AUTOMATED (freshness and transform path build/tests; headset proof pending) |
| V04 | Controller input and handedness | Semantic PTT/confirm/halt actions; no persisted raw device/button assumptions | Input fakes + Touch/Index and one additional controller lane | CAPABILITY DISABLED (semantic input is no longer advertised; installation-persistent INI hotkeys remain the functional VR fallback) |
| V05 | Controller-ray targeting | Optional left/right hand ray plus explicit confirmation and actor authority checks | Geometry/fake snapshots + in-headset | AUTOMATED (right-controller ray, HMD fallback and identity checks; semantic confirmation/headset proof pending) |
| V06 | VR audio and presentation | HMD listener, correct NPC source, readable UI/subtitles, VR-safe viseme pump | Audio/UI fakes + in-headset rotation/movement/interruption | PARTIAL (fresh HMD audio listener and HUD notifications build; viseme/headset proof pending) |
| V07 | VR comfort and performance | No camera forcing; bounded scan/presentation work and telemetry | Stress tests + p50/p95/p99 captures in dense scenes | PARTIAL (retained actor/item/POI collections and queues are bounded, but nearby-actor discovery still traverses all process-list handles; total engine scan/frame time and headset performance remain unproven; no camera mutation) |
| V08 | FRIK optional integration | Versioned readiness-checked adapter; minimal profile works without FRIK | Fake ABI present/old/missing + FRIK/no-FRIK in-headset | CAPABILITY DISABLED (no FRIK ABI is linked or advertised; the minimal HMD/controller-node path is independent) |
| V09 | Popular stack coexistence | Minimal, modern FRIK, Mad God's Overhaul, Idle Hands/Essentials, conservative guide, and F4FEVR profiles | Exact versioned profile matrix | SOURCE CONNECTED (no third-party hooks, forms, scripts, or assets are shipped; profile-specific in-headset evidence pending) |
| V10 | OpenVR/OpenXR environment | SteamVR/OpenVR primary; OpenComposite/OpenXR translation best-effort without hardcoded runtime ownership | Runtime detection/fallback tests + available headset lanes | AUTOMATED (SYNTH consumes CommonLibF4VR camera/controller nodes and never owns or hardcodes the compositor/runtime) |
| V11 | VR lifecycle/state | Save/load/new game/menu/controller loss/power armor/cell changes invalidate or refresh safely | Fake lifecycle + in-headset matrix | WINDOWS BUILD PROVEN (save/load/new-game generation invalidation, menu pause, stale HMD rejection and bounded controller-node fallback build; headset proof pending) |
| V12 | VR native actions | Each enabled Dialectic action separately proven or capability-disabled in VR | Fake action catalog + per-action in-headset matrix | AUTOMATED (the six advertised read-only/presentation actions share typed identity/result gates; every other action is absent from the VR capability manifest) |

## Server parity

| ID | Current DialecticServer surface to preserve/adapt | Fallout 4 change | Status |
| --- | --- | --- | --- |
| S01 | Strict JSON input/response/action schemas and streaming | Rename to `synth.*.v1`; add `runtime_variant` and capabilities; no permanent dialectic aliases | AUTOMATED (mirrored schemas/fixtures/hash; full transport E2E pending) |
| S02 | Main dialogue/rechat/action pipeline | `game=fo4`, `flat`/`vr`, Sole Survivor/Commonwealth defaults | SOURCE CONNECTED |
| S03 | PostgreSQL event log and schema update chain | Independent `synth` database | AUTOMATED (fresh isolated PostgreSQL bootstrap and complete migration/seed verification passed) |
| S04 | Long-term and middle-term vector memory | Preserve scoped NPC/global summaries and pgvector | PARTIAL/AUTOMATED (2026-09-05 native recall, embeddings, automatic capture/dispatch, frozen previous-summary dependencies and lineage-scoped counts are deployed; 71-summary synthetic lineage passes transitive visibility and corruption/cycle rejection. Native helpers cannot mutate legacy memory IDs; the confirmed legacy row-delete path protects native history. Embedding provenance/repair, native CRUD, broader management hardening, narrative middle-term migration and in-game proof remain pending) |
| S05 | Relationship system | FO4 actor/faction/companion semantics | SOURCE CONNECTED (native faction snapshots now feed bounded prompt context and linked NPC profile rules without conflating them with the separate AI relationship score; companion affinity/in-game semantics pending) |
| S06 | Dynamic profiles and NPC master | FO4 biographies, voice types, actor IDs | SOURCE CONNECTED |
| S07 | Narrator, diary, active-quest prompt paths | Commonwealth language/data | SOURCE CONNECTED |
| S08 | World knowledge upload/search/audit/reset | FO4-only provenance dataset | AUTOMATED (FO4 dataset, database integration tests, source audit and deployed route proof) |
| S09 | LLM connectors | OpenAI JSON, OpenRouter, Google, Groq, KoboldCpp, Player2 parity | SOURCE CONNECTED (live-provider proof pending) |
| S10 | TTS connectors | XTTS FastAPI, Chatterbox, PocketTTS, Cartesia, Inworld only | SOURCE CONNECTED (five-provider implementation/acceptance scope per user; existing other adapters retained but excluded from further provider work. Real voice/cloning quality and in-game proof pending) |
| S11 | STT connectors | Deepgram, Parakeet, Whisper, LocalWhisper, Gemini, Azure, Inworld, none | SOURCE CONNECTED (7-driver audit; live-provider proof pending) |
| S12 | Player respeech/TTS | Preserve optional modes and cancellation | SOURCE CONNECTED (rewrite, managed connector, preview/worker, strict canonical-player response and native deferred playback paths connected; live-provider proof pending) |
| S13 | Function/action catalog and results | FO4 flat/VR capability manifest filters unavailable actions | SOURCE CONNECTED (eight flat and six VR capabilities have typed implementations/results; flat equipment is newly deployed but not game-proven; all other catalog actions are hidden) |
| S14 | Quickstart and configuration hub | Synth branding, server health, flat/VR version and dependency match | SOURCE CONNECTED (shared SYNTH theme/navigation and local deployed route proof; browser/game integration pending) |
| S15 | Profiles/prompts/action editor | Preserve validation and API-badge secret references | SOURCE CONNECTED (deployed route proof pending) |
| S16 | Request/response/audit logs and queue view | Request IDs, redaction, generation | SOURCE CONNECTED (real persisted log/health routes deployed; live client traffic pending) |
| S17 | Memory/events/world-knowledge/relationship UI | FO4 labels and filters | SOURCE CONNECTED (database-backed routes and local browser smoke proof) |
| S18 | Playthrough snapshots/stats/rollback | FO4 save/gametime identity | SOURCE CONNECTED (strict client game-time feed pending) |
| S19 | Background workers | Memory, rolemaster, profile autofill, TTS lanes, supervised service | SOURCE CONNECTED (deployed health reports the isolated background port; real queued-provider work pending) |
| S20 | Health, automatic backup, log trim, release audit | Apache/systemd/WSL diagnostics | AUTOMATED (305 PHP files linted, release audit passed, and loopback-only WSL health is live) |
| S21 | PHPUnit and database tests | Port and adapt the current HerikaServer baseline | AUTOMATED (218 tests, 1,073 assertions, one explicit cross-repository environment skip, zero failures) |
| S22 | Vision/image features | PipVision store, describe and targeted portrait with no Base64 or client paths | SOURCE CONNECTED (five selectable ITT drivers, closed synchronized JSON, bounded native image upload, opaque media IDs, durable correlation/idempotency, flat crosshair and VR controller/HMD targeting; live-provider and in-game proof pending) |

## Deliberate exclusions

Linked legacy control checkpoint `593281c` (2026-09-05): exact actor-linked profile
preview and confirmed one-time native lock import, audited source IDs, native-decision
precedence and job fencing are locally deployed (schema 20260905018). SQL/HTTP and
PHPUnit 173/1287 pass. No live controls imported; management presentation, manual pair
correction and coordinated gameplay activation remain unfinished.

Native prompt/explicit dispatch checkpoint `1ac1255` (2026-09-05): one saved relationship
graph query supplies exact-context tier/numeric prompt text with explicit unknown state.
The explicit CLI uses separated player and paired lanes. SQL prompt/discovery/isolation
checks and PHPUnit 172/1280 pass; locally deployed. Actual gameplay hooks and automatic
scheduling remain gated pending coherent completion/control/legacy-lock integration.

Player-only contract checkpoint `8ccede9` (2026-09-05): versioned canonical Player-only
inputs, selected-line prompts, pending/saved validation and nonoverlapping paired publication
are locally deployed. SQL proves both execution orders and saved independent scores;
PHPUnit 171/1274. Existing scheduler capture defaults are unchanged pending coordinated
native prompt/inline/dedicated/scheduler/management cutover. No game acceptance is claimed.

Paired dispatch checkpoint `243104e` (2026-09-05): canonical completed-response/listener
discovery, original-owner frozen recovery, independent lane cooldowns and shared actor caps
are implemented and locally deployed (schema 20260905017). SQL two-process/killed-worker,
retry/deduplication/invalid-input liveness and independent actor-lock checks pass; PHPUnit
170/1267. The dispatcher is not yet connected to automatic gameplay scheduling. Separate
player-only evaluations and coordinated native gameplay/management cutover remain required.
Earlier checkpoint remaining-work descriptions are historical.

Saved paired-reader checkpoint `26132ce` (2026-09-05): paired state now participates
in exact saved-history reads with whole-result and transitive cross-owner dependency
validation. SQL save forks, malformed inputs, no-change results and privacy checks pass;
PHPUnit 169/1263 and server deployment are verified. Paired dispatch/recovery and
coordinated native gameplay cutover remain open. Older reader-exclusion notes are historical.

Paired publication checkpoint `d423b9b` (2026-09-05): original-owner configured providers,
atomic independent revisions/result/dependencies and final lease fencing are locally
deployed (schema 20260905016). Five loopback HTTP adapters, forced rollback/cancellation
and PHPUnit 168/1259 pass. Paired saved-history reads, dispatcher recovery and automatic
native cutover remain unfinished. New paired rows are excluded from the old reader.

Paired queue checkpoint `13f9f6e` (2026-09-05): terminal-stable paired jobs, shared
two-slot per-actor limits, bounded retries/deadlines and both-actor cancellation are locally
deployed (schema 20260905015). SQL/two-process checks and PHPUnit 167/1253 pass.
Paired provider execution, dual-owner publication/visibility and native cutover remain open.

NPC interaction checkpoint `67232d5` (2026-09-05): frozen canonical pair inputs,
independent saved priors, selected prompt/connector and both actor controls are locally
deployed with bounded independent model-result parsing. Selected PHPUnit 166/1240 and
isolated SQL checks pass. Paired queue/provider execution, atomic dual-owner publication
and coordinated native gameplay cutover remain unfinished; this is not full parity.

Skyrim-only spells, shouts, souls, vampire/werewolf state, Soulgaze, intimacy/spell effects,
PrismaUI-specific Skyrim views, Background Life, and AI Quest Manager are not required merely for
parity. FO4-specific settlement/workshop control, power-armor-specific actions, companion-affinity
manipulation, and Creation systems are future features after the parity baseline is functional.
Fallout 4 VR itself is no longer an exclusion.

Management checkpoint server `49f6515` (2026-09-05): native relationship lock/unlock
and explicit linked legacy lock import UI locally deployed, with desktop/narrow browser
fixture and actual API/disposable SQL checks. No client changes or live policy mutations.
Native gameplay activation, manual pair correction and flat/VR acceptance remain open.

Server correction checkpoint `75a7524`: exact directed-pair replacement now uses a saved
administrative revision, with independent fork/race/late-worker/inline-inheritance tests
and deployed HTTP API. No client change or live correction. Management editor/remove/reset,
coherent native hook activation and manual flat/VR acceptance still remain.

Server editor checkpoint `ff69d7c`: native directed corrections/details/custom types
are now manageable with browser/API evidence and verified local deployment. Client
unchanged. Reset-to-unknown, coherent runtime activation and manual flat/VR proof remain.

Server reset checkpoint `431ee4c`: saved unknown revisions and administrative reset
now preserve save boundaries through inline/player/paired workers. SQL/API/browser
checks and existing native/protocol/archive audits pass; schema 20260905020 deployed.
Client DLL/config unchanged. Coherent runtime activation and manual flat/VR proof
remain open; no game was launched or controlled.

Server runtime integration `bda8c09` / isolation `f8e45c9` is locally deployed:
canonical saved prompt, completed inline updates and scheduled player/pair work now
run through native ownership. Separate loopback 12348 service has produced one real
player result/state from accepted evidence. Client remains unchanged. Actual flat/VR
behavior and sustained service reliability are still acceptance gates, not inferred
from server execution or automated tests.

## Native PipVision checkpoint (2026-09-05)

PARTIAL / AUTOMATED: exact v2 snapshot, turn and capture-ID ownership now spans capture,
provider completion and the reaction prompt. Cancellation/save/supersession and native
ambient isolation pass no-game SQL/native tests. Legacy gallery rows are not native
knowledge. Saved-timeline visual history and edited persistent knowledge remain open;
flat spoken-image and independent VR acceptance remain unproven. See the goal ledger.

### Accepted saved visual observations (2026-09-05)

PARTIAL / AUTOMATED: immutable native visual results now advance ordered history; ambient
prompts restore only accepted actor-scoped ancestral observations. A post-response client
acknowledgement makes completion eligible for subsequent co-saves. The independent cancel
lane now covers ITT itself. PostgreSQL save/fork/replay/isolation checks pass. Versioned
manual scene edits and real flat/headset capture-save-load behavior remain unproven.
