# SYNTH developer handoff

## Vanilla dialogue witnesses candidate

Flat-only `context.observed_dialogue` grants canonical witnesses for a vanilla NPC line at the
first sampled observation (policy `same_cell_speaker_radius_1000_v1`). It does not prove a voice
start or audibility. VR does not offer it. Native tests, build guards, protocol fixtures, the flat
and VR x64 builds and the packaging regressions pass. Packaging reused the existing source-bound
wait script and compiled nothing new. A loopback prompt check with a fake provider shows the line
reaches only the granted witness. A source review found no wrong-grant defect; audio start and
queue pressure remain in-game checks. There is no model-answer review, game run or migration. See
[VANILLA-DIALOGUE-WITNESSES.md](VANILLA-DIALOGUE-WITNESSES.md).

## NPC biography and SPECIAL candidate

Authored expertise is preserved once in Background. Skills come only from the exact actor’s copied SPECIAL base/current observations after a negotiated init acknowledgement. New imports use the layout; the server provides an operator-only, provenance-guarded dry-run/apply repair. No database schema change, deployment or game acceptance is implied. See the server [NPC-BIOGRAPHY-REPAIR guide](https://github.com/Dwemer-Dynamics/SynthServer/blob/unstable/docs/NPC-BIOGRAPHY-REPAIR.md) and [protocol/v2/README.md](../protocol/v2/README.md).

## Optional post-combat companion comments

The paired source candidate adds participant-bound, default-off flat comments with
owned cancellation and a latest-start deadline. See [the feature note](COMPANION-COMBAT-COMMENTS.md).
No deployment or flat gameplay acceptance; VR remains unavailable.

## Plugin-local location ancestry

Flat contexts can carry `world.location_ancestry` beside `location_ref`: the current
`BGSLocation` and its `parentLoc` chain, at most eight entries, as plugin-local IDs decoded
through the loaded plugin manifest, with an explicit `root`, `unresolved`, `cycle` or
`depth_limit` end. It needs `context.location_ancestry` and the server's init acknowledgement,
so older servers never see it. It lives only in the existing context snapshot; there is no new
database schema or bulk export. VR skips the ancestry walk and does not offer it. The server
projects it into the native prompt. Native tests, fixtures and builds only; see LOCATION-ANCESTRY.md.

## Private narrator candidate

The optional private narrator contract adds immutable named definitions, saved selection and transcript branches,
and receipt-gated replies using the existing SCTX checkpoint. Legacy Narrator mode remains separate.
See [PRIVATE-NARRATOR.md](PRIVATE-NARRATOR.md) for controls and limits. Source builds and disposable tests do not
establish flat gameplay, VR headset presentation or live microphone/provider acceptance. No deployment is included.

## Installed gift screen and shared wording

The user approved general gifts opening the normal inventory exchange, while
named items transfer directly. The shared Marcy wording helper is installed for
all NPCs. The coordinated installed update adds owned, observed OpenInventory,
post-menu paired receipts and model-valued gift batches. Automated checks pass;
the reviewed DLL/PEX/backend are installed and hash verified. Next verify
the correct NPC's free exchange opens and actual donations become gift facts.
No fixed NPC bonus is used. See PROTOCOL.md. Menu gameplay remains unverified.


## Installed player-pack gifts

The populated live action catalog required explicit registration of the new
reviewed TakeItemFromPlayer builtin. The empty-catalog seeder does not add missing
actions to an existing catalog. Registration is complete and verified; all 34
previous catalog rows were preserved. The initial Marcy/Jun tests had no transfer
intents because the action was absent. Retest after registration.

The user explicitly requested player-to-NPC ordinary item transfers and personal
gift valuation. The coordinated candidate adds TakeItemFromPlayer with its own
flat v2 acknowledgement, exact Player donor/NPC recipient evidence and explicit
direct-input authorization. Accepted gifts feed the existing relationship model
with the recipient's saved biography/personality/goals; there are no fixed NPC
scores. Failed handoffs and receipt retries cannot multiply gift appreciation.
The coordinated flat client/backend update is installed and hash verified. Gameplay acceptance remains open. See PROTOCOL.md.


## Equipped pistol transfer correction

The installed flat update performs verified non-forcing unequip and exact-instance
GiveItemTo within one owned game-thread operation. This addresses Jun equipping
the Heavy 10mm Pistol again between separate requests. Exact counts, instance/
quest ownership and cancellation/load/deadline checks remain required.
Equip, corrected unequip and equipped-weapon transfer are gameplay/receipt
verified. Jun returned the equipped Submachine Gun with complete paired
inventories proving donor zero and player one. Taking ordinary items from the
player pack is now supported by the installed direct transfer; the general gift menu remains a candidate; reverse caps transfer works.
See ACTION-INVENTORY-OBSERVATIONS.md.

## SYNTH-owned playthrough identity

The shared `playthrough_id` is now a SYNTH-owned random UUID v4, not the engine
`currentPlayerID` (which remains a readiness check only) and not a server checkpoint.
It is drawn once with the system-preferred BCrypt generator at NewGame, or at a
successful PostLoadGame that needs a fresh identity, outside the store lock and never
in a frame or co-save callback. The co-save keeps one bounded SCTX version 2 record:
lane, UUID and an optional exact saved checkpoint for that UUID. It is written on every
save, even before any session, network or ACK; a failed write logs once and is not retried.

Each load callback claims the open load when it starts, before reading, and stages only into
that load, including on failure or exception. A second callback in one load is a duplicate.
PostLoadGame is consumed once: the load result is the pointer value itself, which must be
exactly 1 with dataLen exactly 1 and is never dereferenced. A valid staged record is activated
in one locked step. Otherwise a fresh UUID is drawn outside the lock and is discarded if any
later world change (another load, NewGame or a Revert) happened meanwhile. A PostLoadGame
that arrives while a callback is still reading, a repeated PostLoadGame, or a reader left
over from an earlier load cannot restore or publish anything. Only an identity actually
resolved for the current world makes SYNTH ready; a failed, stale or random-failed
resolution leaves it closed. A NewGame whose draw went stale has no lifecycle effect at all:
the later world change that superseded it already did its own teardown, so a stale earlier
completion cannot tear down a newer resolved world. A random failure (or exception) at NewGame
still tears the old world down without readiness.

A missing callback, legacy version 1, malformed, duplicate, unsupported, other-lane,
incomplete or revert-contradicted record gets a fresh UUID with no saved checkpoint and one
log line. Earlier server rows are not rekeyed or deleted. An unstamped save therefore gets
a new identity, a separate history, on every load until a normal save made from it stores one.

F4SE (flat 0.7.9 and VR 0.6.21) publishes NewGame only after the original NewGame_Internal
returns, so any Revert inside the original precedes that publication and happens before the
new identity exists. The exact engine event order is still an in-game acceptance item. Any later Revert outside
a load revokes the identity at once, even if no session ever bound and SYNTH is offline or
disabled. Capture and dispatch close immediately, and the session teardown goes through the
existing deferred lifecycle path. Inside an open load a Revert keeps the load but rejects any
record a callback has already begun to stage.

Every resolution, load and revert also changes a lifetime stamp. The bootstrap reads it just
before its capture and binds only if it still matches, so a snapshot from before a reload is
rejected even when the reload restored the same UUID. VR also uses the lifetime as its
capture epoch, so ordinary capture and actor enrichment never cross a PreLoadGame, NewGame or
Revert; flat keeps its picked-reference stamp, which those events invalidate at once.

The session host checks the world lifetime and the writer before it creates a session, before
it queues initialization, when it publishes the result and when the game thread takes it.
Host initialization only queues the init request; it never waits for HTTP. A terminal result
without a session (an error, a rejected or throwing initialization, a disabled runtime) needs
only the current host request and the same world lifetime, so it still completes the game
thread's pending request after its own teardown or a same-world replacement revoked the
writer. A live session additionally needs its exact writer; one that lost it in the same world
is retired and reported as an error. Context publication and initialization check
cancellation, termination and the writer immediately before and after each HTTP exchange:
a revoked writer sends nothing, and a response that arrives after revocation produces no
publication state, ACK, notification or initialization side effect. Initialization now
carries its task's cancellation into the client. A request already in flight may have
reached the server. Same-world replacement keeps the UUID and latest ACK under
a new writer epoch and runtime session; the C1 explicit ACK and revision rules are unchanged.
No protocol, schema or endpoint changes. The main-menu revert and an early autosave are not
verified in game. See ALPHA-ACCEPTANCE.md.

## Speech receipt checkpoint contract

Whether a heard line is visible after a later load depends on four separate events:

1. **Receipt commit (server).** An `audio_completed` receipt for an emitted line, from its exact
   speaker, stores the line's state and inserts its heard history row in one transaction under
   the session lock. Repeating the stored state is idempotent and adds no row. Any other outcome
   for that line is `receipt_conflict`, and an owner mismatch writes nothing.
2. **Snapshot capture (server).** When a context commits, the server stores an immutable snapshot.
   Its cutoff is the highest history row of that runtime session, generation and playthrough at
   commit time. The cutoff is fixed before the server returns any ACK.
3. **Exact accepted context ACK (client).** The client moves its saved checkpoint only on a
   complete context response with exactly one `context_accepted` status, apart from framing lines
   and any negotiated social offers. The publication must also be uncancelled, the session must
   not be terminal, and the writer must still be current. A bare or wrong completion, an error, a
   transport failure or a revoked writer keeps the older checkpoint.
4. **Disk save (game).** A save writes the latest exact ACK held at that moment into its SCTX
   record. After loading that save, the new session sends it as `loaded_context`, and the server
   exposes the earlier session's history only up to that snapshot's cutoff. Rows outside the cutoff
   stay in the database but are not visible from that save.

A row is covered only by the cutoff of the last acknowledged snapshot. When the receipt, the ACK
or the save happened does not decide it.

**What makes a publication due.** The client keeps a history frontier that a later exact ACK must
cover. A speech receipt raises it only when the receipt sent was `audio_completed` and the reply is
an exact `speech_receipt_accepted` that stored `spoken`. The session must also be uncancelled and
not terminal, in the same generation, with its writer current. Other sources raise the frontier
too. On flat, an accepted dialogue line raises it when it arrives, before presentation. That is not
a heard signal. In either lane, a delivered Wait action result raises it. A raised frontier makes a
world publication due. A publication clears it only if it was raised before the publication started
and the publication received an exact ACK in the current generation. Due is not a schedule: the
next publication comes from the ordinary observation gate, and nothing guarantees one before a save.

**A receipt racing a context.** The server serializes the two. If the receipt commits first, the
context's cutoff covers its row. Another publication still stays due, because the receipt was
noted after that publication started. If the context commits first, its cutoff excludes the row,
even though its ACK may reach the client after the receipt reply. The publication stays due, and
only a later exact ACK whose cutoff covers the row makes it reachable. A save holding the first ACK
shows the line only in the first case.

**Unknown outcomes and no recovery.** A receipt or context that fails in transport may still have
been committed. Examples are a timeout, an unavailable server or a reply that never arrives. The
client treats such a request as unproven:

- A receipt raises no frontier. A committed heard row is in no save until a later exact ACK,
  raised for some other reason, has a cutoff that covers it.
- A context leaves the older checkpoint. The server may hold the lost snapshot, but no save names
  it. The next publication uses a new sequence (the server refuses a reused one as `stale_context`).

Nothing queries the server for committed receipts or snapshots, and receipts are never retried.
A lost context is not resent. Later publications are new requests from the ordinary observation
gate.

**Same-world replacement.** A replacement session in the same world keeps the UUID and the latest
exact ACK under a new writer epoch and runtime session. Its contexts carry that ACK, so it sees the
earlier writer's history only up to that ACK's cutoff. Heard rows outside that cutoff stay in the
database. Neither the replacement nor any save it writes can reach them. Saves already on disk
never change: each keeps its own anchor, and an existing binding keeps the parent it was created with.

The writer epoch gates context publication, the ACK and initialization. A revoked writer publishes
nothing, moves no checkpoint and fails only its own init. It does not gate receipts. An earlier
session that is still live may send a receipt, and the server may commit it with its heard row.
Such a receipt is governed by cancellation, the receipt outbox, the terminal state and the
generation check. Once its writer is revoked it raises no frontier.

**Stop and Hard Halt.** These are separate controls, checked per lane.

- *Flat Chatbox Stop* exists only in the flat chatbox UI. VR has no chatbox and no such control.
  It is dialogue-only. It cancels the control wait and flat Wait packages, then ends voice capture,
  the owned Wait, social tasks and the current turn. Both lanes take the same dialogue-only path,
  without the chatbox steps, when presentation validation rejects a line. The connection, runtime
  session, writer and checkpoint stay, so later exact ACKs still cover heard rows.
- *Hard Halt* is the `StopTalking` binding, wired separately in the flat and VR input paths. It
  makes the session terminal, cancels transport and discards pending network work, so queued
  receipts and publications are never sent. The server halt is best effort: one request on a short
  deadline, and SYNTH reports when it was not acknowledged or could not be queued. A request sent
  before the halt may still have been committed and has an unknown outcome. No result after the
  halt raises a frontier or moves the ACK. Once the halt completes, the session retires, and the
  next session in the same world is a replacement under the rules above.

Native scratch evidence uses a fake in-memory ledger, not the server, and recreates the store from
co-save bytes instead of killing a process. It passed the C3 contract with 15 receipts and 15
contexts. A disposable server HTTP probe for the same contract passed (2 tests, 1430 assertions,
exit 0). It models a lost reply by discarding a received response, not by losing the network.
Neither replaces the flat and VR game checks in ALPHA-ACCEPTANCE.md.

The opt-in loopback driver in `tests/loopback_dialogue/` is the first off-game slice that uses the
compiled client instead of hand-built events. The real `SynthClient` and `SaveContextStore` talk
over real HTTP to a disposable server: init, an explicit native /talk with streamed dialogue, an
audio_completed receipt sent twice from the shipped delivery boundary, a checkpoint, and two
sessions that load the older and the newer co-save. The Synthserver repository owns the harness.
From any clean checkout pair, as root on Linux or WSL:
`sudo bash SERVER/unittests/loopback/run-loopback-dialogue.sh CLIENT`. It builds the driver from
the client `HEAD` and runs it against the server `HEAD`, a disposable PostgreSQL cluster and a
loopback fake provider, all in private namespaces. It requires one heard row. The older anchor's
prompt must omit the line, and the newer anchor's prompt must contain it exactly once. The driver
uses a POSIX test transport, not WinHTTP. It copies no ACK or receipt glue: it calls the same
engine-free `speech_receipt_hook`, `exchange_speech_receipt` and `SaveCheckpoint` that
`SpeechDelivery` and `ContextPublication` delegate to, and checks that the accepted heard receipt
makes a checkpoint due before the covering publication clears it. It proves no audio, game thread,
socket loss or flat/VR gameplay.

## Current handoff priorities

Rangroo's handoff scope is bios, world knowledge, diaries, actions, and basic
rechat/profile setup. Preserve accepted work: flat NPC rechat and movement release
were already gameplay confirmed with Codsworth and Jun, and completed rechat is
also recorded in the current backend. All 11 stored actor sheets have valid
profile/connector assignments and populated biography/personality fields.

The 4 November read-only audit confirms all 204 reviewed biography templates are
populated, all 207 expected actor mappings match, and all 125 lore articles are
active with matching reviewed text and content hashes. Existing successful
requests show biography and lore text reaching the provider. This completes the
reviewed content pack, not an exhaustive Fallout cast/world census.

One bounded deployment gap remains: Pearl, Maxwell, Bert Riggs, Gilda Broscoe,
Keith McKinney and Santiago Avida retain the old exclusive prewar access tag in the
installed pack/database. The reviewed candidate removes it for these live postwar
Vault 118 actors. Their biographies are present; the obsolete tag restricts lore
access. The installed JSON pack also predates other biography revisions already
in the database. No runtime change was made by the audit.

Manual NPC diary generation, the F6 shortcut and dashboard publication are
gameplay verified. Continue only outstanding supported-action cases and specific
reproduced failures; do not restart accepted biography, lore or basic rechat work.
The directed follower-to-NPC errand failed, with no accepted social-task request
found, and remains deferred separately from basic nearby rechat.

## Manual diary control and dashboard reader

The flat control menu now offers Write Diary for its retained NPC, closes on
submission, revalidates the exact actor after capture and calls the existing
owned background diary request. A target change or load cannot redirect a
queued selection. The menu, dashboard reader and configurable WriteDiary hotkey
are installed. Shipped defaults leave the shortcut unbound; local settings assign
F6. It captures only the crosshair NPC, blocks menu input, obeys dialogue pause
and combat policy, and keeps exact actor/session ownership.
Flat gameplay on 4 November confirms F6 works: Sturges' new 1,429-character entry
completed on the first provider attempt in about 33 seconds, was published with
valid ownership/hashes, and appears in the dashboard. The paired backend fixes
the old 15-second background timeout. VR shortcut gameplay remains unproven.

## Settings pages and hotkey bindings

Source and off-game tests only; nothing here has been checked in game. The flat
Hotkeys page lists all eight keyboard bindings once each, all shipping as 0
(unbound), and states that SYNTH reads only its INI chain: generic framework
Hotkeys edits do not change SYNTH keys, and the framework conflict check covers
its own registry only. Hold to Talk has its own enable toggle on the Sound page,
and the Mute Open Mic help separates the saved preference from the session-only
hotkey. Chatbox and help text no longer promise an NPC in untargeted chat modes.
Per-lane availability, including VR's keyboard WriteDiary dispatch to the
right-controller ray or HMD-gaze NPC, the VR SYNTH Control status retry and
unverified PipVision acceptance, is in `CONTROL-MENU-PARITY.md` and the package
READMEs.


## Supported-action consumption correction

Item transfer and both 10-cap directions with Sturges are now confirmed by the
user and successful receipts with complete donor/recipient inventories. The
Purified Water Consume request reached the client but was blocked by a temporary
engine queue-thread check outside that queue-drain phase. The candidate now calls
the audited flat 1.11.240 immediate consumption path on the main thread and keeps
the original action ownership, cancellation/deadline and exact decrement proof.
See ACTION-INVENTORY-OBSERVATIONS.md. The correction is installed; the user
confirmed water consumption and the successful receipt verifies exactly one item
was consumed. This is flat gameplay evidence; VR remains unproven.

Updated: 2026-10-15. This is the current entry point for SYNTH development. Older plans and
audits under `docs/` are kept as feature evidence, and some carry a historical banner.

## Baseline

| Item | Value |
|---|---|
| Client source | `RANGROO/SYNTH` `codex/chim-conversation-targeting` at `f34da88` (includes `c32f624` and four later targeting/restraint fixes) |
| Paired server | `RANGROO/Synthserver` `codex/chim-queue-parity` at `bcb0c06` |
| Plugin version | `0.1.0` (`xmake.lua`, `vcpkg.json`, `src/adapters/adapter_descriptor.hpp`). This cleanup does not change it. |
| Protocol | The client sends `protocol_version` 2 and accepts responses with version 1 or 2. `protocol/` is mirrored byte-for-byte with Synthserver. |
| Default endpoint | `http://127.0.0.1:8087/Synthserver/` (`config/SYNTH.ini` `[Server] BaseUrl`) |

The public repositories are `Dwemer-Dynamics/SYNTH` and `Dwemer-Dynamics/Synthserver`.
Start all work from `unstable` and target every topic PR at `unstable`. Promotions are only
same-repository `unstable` -> `dev` and `dev` -> `synth`; `synth` is the main/release branch and
`alpha` is a legacy lane. See [BRANCHES.md](../BRANCHES.md) and [CONTRIBUTING.md](../CONTRIBUTING.md).
The public client source comes from private cleanup `618e924`; the paired server comes from
`c12fb1b`. Older hashes and branch names below identify historical development checkpoints.

The testing candidate preserves native follower movement, uses an original continuous
Follow package for bounded ordinary-NPC approach, includes conversational robots in
camera-independent hearing, and provides separate owned navigation for saved follower
errands. Arrival requires the actual owned package rather than proximity alone.
Flat gameplay confirms Sturges approaches the player during conversation; the
runtime log confirms the expected owned package. Walking together, cleanup,
cell transitions and NPC-to-NPC errands still need separate gameplay proof.
See [CONVERSATION-HOLDS.md](CONVERSATION-HOLDS.md). Updated flat gameplay remains open.

The 3 November 2026 flat retest confirmed, by user observation, that Wait Here
and Follow Me work after correcting generated version-131 package schedules:
month Any must be `00`, rather than the pre-version-122 `FF` encoding. The live
alias already contained the priority-100 Wait package; the invalid month prevented
selection. See [WAIT-ALIAS-CHAT.md](WAIT-ALIAS-CHAT.md) for the cause and remaining
acceptance scope.

## Supported runtimes

All other runtime builds fail closed.

| Lane | DLL | Game | Script extender | Also required | Engine library pin |
|---|---|---|---|---|---|
| Flat | `SYNTH.dll` | Fallout 4 `1.11.240` | F4SE `0.7.9` | Address Library for F4SE Plugins (1.11.240) | `libxse/CommonLibF4` `6266ecc9` |
| VR | `SYNTHVR.dll` | Fallout 4 VR `1.2.72` | F4SEVR `0.6.21` | VR Address Library `v1.13.1` | `ArthurHub/CommonLibF4VR` `1c7b4fc8` |

F4SE Menu Framework is optional and works on the flat lane only. The VR lane is configured
through `SYNTH_custom.ini` alone.

## Where things live

The [documentation index](README.md) maps each component to its notes, source and tests.

- `src/core`, `src/client`, `src/tasks`, `src/protocol_native`, `src/json`, `src/actions` and the
  other engine-free folders hold the shared core: protocol, queues, media, conversation and
  typed actions.
- `src/adapters`, `src/flat` and `src/vr` hold the runtime adapters. All `RE::`/`REL::`/F4SE access
  stays behind `IFalloutRuntime` and runs on the game thread.
- `scripts/papyrus/` holds the original Papyrus sources. `scripts/build_gameplay.py` generates
  `SYNTH.esp` deterministically. `packaging/` and `tools/audit_release_tree.py` hold the release
  layout and its audit.
- `protocol/` holds the versioned JSON schemas and fixtures shared with Synthserver.
- `tests/` holds native unit suites, build gates and packaging tests.

## Client session ownership

`src/client/plugin_session.hpp` is the composition root. It constructs the owners below in
dependency order, runs the ordered game-thread pump and the lifecycle paths (turn replacement,
combat interrupt, local Stop, hard halt, retirement), forwards the stable flat/VR entry API, and
routes response lines and rejected presentation between owners. Cross-feature admission and
priority checks stay there. `PluginSessionHost`, the lifecycle coordinator and `SaveContextStore`
remain the generation, adoption and persisted-binding authorities.

| Owner | Owns |
|---|---|
| `SpeechDelivery` | Speech lane and job count, media preparation, playback, captions with scene admission, copied presentation validation, spatial frame, receipt outbox |
| `PlayerDialogue` | Turn source and reserved cancel, external owner and cooldowns, PTT/open mic, typed admission, the exact selected/pointed/external target policy |
| `DialogueRequests` | Text/voice request execution and the shared response count |
| `ContextPublication` | Capabilities, initialization, full/delta publication, save ACK and write epoch, history/inventory frontiers, dynamic profile refresh |
| `ServerControl` | Confirmed mode/model-slot selection, pending revision/deadline and its ACK RPC, control-menu target capture |
| `ActionExecution` | Native action queue, replay guard, Wait lifetime, action receipts |
| `SocialTaskExecution` | Walk/visit and exchange, progress, checkpoint and hydration |
| `RechatContinuation` | Parent, capture, retry, speculative media and child scheduling |
| `ObservedEventDelivery` | Quest/actor/player event delivery and the enemy-awareness cache |
| `ActivationTriggers` | Activation cadence, acknowledged actors, activity clock and trigger requests |
| `DiaryRequests`, `VisualCapture` | Diary queue/status/checkpoint; PipVision copy, upload and describe |

`wire_identity.hpp` is the single actor-to-wire identity and actor-key conversion. One quest event
buffer (`QuestEventDelivery`) is shared by observation delivery and context publication. Lanes are
unchanged: network bounded with two workers, speech 16/4/3 with one worker, control 4/4/4 with one
worker reserved for cancellation. The session destructor joins network, speech and control work
before any owner is destroyed. Native unit tests, build gates and fake-server checks do not compile
or exercise a live `PluginSession`; they give no flat or VR gameplay, audio or provider acceptance.
Menu, clipboard, hotkey, notification and native presentation behavior is unchanged by this
composition. `PlayerDialogue` publishes voice activity (recording and the open-microphone
passive/speech flags) as one atomic value, so the flat automatic-activation recheck on a network
worker reads a coherent value without touching microphone state. The game thread classifies it
when capture starts, when open-microphone speech is detected (before the turn and notification
callbacks) and at silence end; every stop, cancel, passive timeout and teardown (including
`~PluginSession` on the retirement worker) publishes known inactivity without reading game-thread
flags, also when building the stopped recording throws. The exact prior checkpoint (client
`3ee97b2`, server `3e637c6`) had both x64 builds and packages audited. This candidate, paired
with server #10 at `3e637c6` (server #11 is documentation-only), has local x64 flat and VR builds,
both package and manifest audits passed, and each packaged DLL matches the built DLL exactly. It
was not deployed; flat and VR gameplay, a real microphone and providers remain unchecked.

## Build and test

The current testing candidate connects explicit text/audio Wait Here and Release
Wait to the existing flat wait controller. See [WAIT-ALIAS-CHAT.md](WAIT-ALIAS-CHAT.md).
Normal new input preserves an accepted hold; Stop and load revoke it. VR remains
unsupported for native wait execution. Automated validation and installation
receipts must identify the exact candidate; these changes still require new flat
gameplay evidence and do not change the previously observed control-menu result.

Idle NPC exchanges reuse the configured bored timer, require acknowledged server
support, STANDARD mode and two eligible nearby NPCs. Passive open-microphone
listening permits the exchange; player speech, typing, menus and Stop take
priority. See the paired server's native rechat ownership document for profile,
listener and completed-parent proof. Close capture does not establish NPC-to-NPC
line of sight or every vanilla speech state. Flat and VR gameplay remain open.

See [README](../README.md#build-and-test) for the commands. Points to know:

- Prepared Stop/save ancestry correction: a replacement session in the same
  loaded world inherits the latest acknowledged context. Each existing session
  keeps its copied parent immutable. Loading a different save still resets the
  store and restores that save's own context; foreign-player and stale-writer
  checks retain their boundaries. The flat return test exposed an older loaded
  parent being reused after Stop, excluding a just-published NPC-pair result
  from the next save. Native regression reproduces that failure and covers
  same-world replacement, older disk saves, immutable bindings and stale writers.
  Both native builds and package validation are required before installation;
  installation and the corrected flat/VR return checks remain open.
- The flat package needs compiled Papyrus in `out/wait-papyrus` (always) and in `out/papyrus`
  (for `SYNTHNative`, when present). Both are built with hash-pinned Caprica v0.1.5 against
  **your own** Fallout 4 binary script imports, which this repository does not contain. The
  native build scripts only check the Papyrus source. `tests/packaging/test_release_audit.py`
  builds a real flat package, so it fails on a host without `out/wait-papyrus`.
- **CI policy (chosen 2026-10-06):** hosted CI builds and tests source only. Release
  packaging is a mandatory local step on a host with the compiled `out/wait-papyrus/`:
  run `tests/packaging/test_release_audit.py`, build both packages and audit both archives.
  The hosted `windows-native` jobs build each DLL and run its native suites; they no longer
  package. The hosted `contracts` job keeps the portable C++ suites, `verify_protocol.py`
  and the build gates, and no longer runs the packaging tests. Synthserver CI never
  packaged and keeps its tracked source-tree safety audit. Do not weaken the audit or commit
  a fabricated PEX.
- Previous hosted results (client `5342870`, before this policy): the flat DLL built and its
  native suites passed, then flat packaging failed because `out/wait-papyrus/SYNTHWait.pex`
  is not provisioned on hosted runners. The VR job built, tested, packaged and audited
  successfully. `contracts` passed its native suites, protocol check and build gates, and
  failed only in the packaging tests for the same missing PEX. Server CI passed at
  `d171ec7`. The revised workflows have not run on hosted CI yet.
- Locally, the static-md VR build, all 17 native tests and the real package audit pass, and
  packaging passed with the pinned compiler and the user's own game imports.
- `scripts/verify_protocol.py --peer ../Synthserver/protocol` checks the mirror against a sibling
  server checkout.
- Synthserver's `unittests/tests/AutoGreetingRuntimeTest.php` reads this repository through
  `SYNTH_CLIENT_ROOT` (default `../SYNTH`). If this repository is missing, the test is skipped.

## Gameplay records and save compatibility

- `SYNTH.esp` is flat only. It contains original wait and restraint records with
  `Fallout4.esm` as its master. Never renumber or remove existing FormIDs, quest aliases,
  PACK/REFR records or the local-800 migration record. Saves refer to them.
- Wait uses exact alias-bound packages wrapping the ordinary Wait template (`001A32D6`).
  The Wait quest priority is 100 and is audited as a signed runtime value.
- Current wait action success additionally requires the active owned package and stable stopped-position
  verification; release requires empty owned aliases and fresh absence of SYNTH Wait packages.
  The fixed position anchor is established only after exact alias/package observation, so a walking
  follower is not cancelled for movement during asynchronous installation. Success still requires
  two stationary samples; pending installation retains the original deadline and cancellation guards.
  Admission stays pending until those checks complete. The 90-second unpaused order
  survives later dialogue, while Stop and load release it rather than restoring it.
  Generic restraint is factual metadata and is preserved. It is not evidence of SYNTH-owned waiting.
  Aliases 0â€“255 bind Wait subjects to packages 0x900â€“0x9FF. Aliases 256â€“511
  are legacy restraint receipts used only for migration cleanup. The local-800
  package and marker records remain for save compatibility. See
  [WAIT-ALIAS-CHAT.md](WAIT-ALIAS-CHAT.md).
- Commit `231bbdb` (PKDT interrupt-override fix) and commit `f34da88` (restraint hold) must stay
  together.

## Behavior in this baseline that is not yet accepted in-game

- `cc883ac`: the chat character counter is hidden. The input limit is unchanged.
- `a842fd6`: typed chat, push-to-talk and open microphone target the nearest eligible
  listener when no NPC is aimed at.
- `231bbdb` and `f34da88`: the wait package classification is corrected, and the NPC is held
  with owned restraint (flat only).
- Earlier features (queue parity, lipsync, native subtitles, facing/menu pause, inventory
  actions) also have open manual acceptance. See [ALPHA-ACCEPTANCE.md](ALPHA-ACCEPTANCE.md).

## Known gaps and deferred work

- **Stage 1 native typed turn (source candidate).** The client advertises
  `dialogue.native_turn`; only `/talk <message>` in the existing text entry
  selects the native route (explicit per-turn intent). It sends `speech_receipt` only after the exact
  `native_turn` init ACK (contract: `protocol/v2/README.md`). Terminal caption
  delivery copies one receipt into a bounded outbox; a background network task,
  never the cancel lane, sends it once without retry. Only completed audio is
  reported as heard. Text-only/reading-timer lines send no receipt because no
  renderer acknowledgement exists, so the server keeps them unheard. Not yet built
  as a DLL, packaged or tested in flat/VR gameplay.

- **Prepared NPC combat-target identification.** [COMBAT-TARGETS.md](COMBAT-TARGETS.md)
  describes bounded native target-handle capture, explicit v2 acknowledgement and
  response-speaker prompt ownership. Other targets do not become listeners or
  history readers. Unknown or older observations cannot identify an opponent.
  Reviewed installation and flat/VR gameplay acceptance remain open.

- **NPC speech ownership (flat candidate).** [NPC-SPEECH-OWNERSHIP.md](NPC-SPEECH-OWNERSHIP.md)
  records clip-lifetime ownership and the audited index-0 native voice Stop path.
  The current actor's voice is stopped before SYNTH playback and on subsequent
  owning pumps, independently of race, lipsync or facing. No actor speaking flag
  or quest/scene state is changed. New native lines are stopped at the next pump;
  zero-overlap voice-start prevention is not established. Flat audible overlap,
  recovery and save/load acceptance remain open. VR NPC clips currently fall back
  to text because its native voice ABI is unverified; narrator audio remains enabled.

- **Optional native radio diagnostic prototype (flat test installation).**
  [RADIO-PROTOTYPE.md](RADIO-PROTOTYPE.md) documents a separate original ESP,
  two native scene media slots, a flat game-thread diagnostic bridge and A/B/A
  changed-duration experiment. It is disabled by default and invalidates queued
  commands on runtime generation changes. DCR insertion, live media rewrite and
  provider generation remain unimplemented. The isolated flat test
  reported missing station records: scene placement is corrected to FO4's
  type-10 quest-child group, with unchanged FormIDs. Native lookup failures now
  identify the specific record or validation boundary. Retest station discovery
  and playback before extending integration. A subsequent saved-game run still
  missed slot A in the global form map. The adapter also checks only the exact
  owned scene in the bounded quest scene list and reports child-loading state
  if absent; it does not promote or reset quests. A subsequent startup report
  still had zero loaded quest scenes. The Papyrus enable command now checks
  the latent Start result, running state and exact transmitter alias, with
  on-screen failure/status notifications. The revised alias is optional and empty
  at startup; the script fills only that alias with the exact owned transmitter,
  rejecting a different existing reference and a generation change. This removes
  mandatory engine alias filling from Start. A flat test on the same save verified
  startup, exact alias assignment, loaded owned scenes and native enable success.
  The preview was audible through Trinity Tower because the original 99.5
  frequency overlapped its vanilla transmitter. The authored diagnostic
  frequency is now 106.9, checked against the test profile's radio fields.
  A flat retest confirmed discovery under the SYNTH station name, manual tuning
  to 106.9 and audible slot B playback. A subsequent same-session baseline
  confirmed A/B previews play and finish normally. Completion observations
  exposed a diagnostic error: the radio action's sound ID is a descriptor FormID,
  not a native audio handle, and completed actions reset their duration/ID fields.
  The corrective native candidate uses audited, locked RadioManager queries and
  releases actual listener handles for only the two owned scene keys. It preserves
  opaque upper status flags, rejects uncertain or foreign audio state, and reports
  pending/playing/paused busy state separately from audibility. The candidate needs
  an in-game during-playback and completion retest before the controlled rewrite.
  A/B/A rewriting and audible cache refresh
  remain open. The original startup rejection reason was not emitted by the logs.
  The VR
  radio seam is unsupported. Default `SYNTH.esp` and package allowlists are unchanged.

  The testing candidate also preserves the existing October 7 conversation
  diagnostics, speech admission and voice sample fixes. Radio is additive to
  those local behaviors; rebuilding and unit checks do not establish their
  in-game acceptance.

- **The complete flat acceptance matrix and VR in-headset acceptance remain open**
  for this baseline. The optional radio startup/enable result above is narrower.
- **VR runtime DLL imports (corrected in this cleanup).** The baseline `scripts/build-vr.ps1`
  used the dynamic `x64-windows` vcpkg triplet, so the baseline `SYNTHVR.dll` imported
  `spdlog.dll` and `fmt.dll`, which the VR package does not ship. The script, the CMake gate
  messages and the build gate tests now use `x64-windows-static-md` (same ports and baseline,
  static libraries, dynamic MSVC runtime). A rebuilt static-md `SYNTHVR.dll` passed the native
  suite (17/17), and its PE import table lists no `spdlog.dll` or `fmt.dll`. A VR package built
  from that DLL passed the release audit locally. The hosted VR job at `5342870` also built,
  tested and audited it. In-headset load remains open.
- The flat `SYNTH.dll` compiles in libxse/CommonLibF4, which is GPL-3.0-or-later with the
  Modding and Linking Exceptions since upstream `0d96d54`. Distributing that DLL carries GPL
  obligations. See [PUBLICATION-CHECKLIST.md](PUBLICATION-CHECKLIST.md).
- `PickupItem` is disabled. Native approach, transfer and cleanup are unproven. See
  [PICKUP-ACTION.md](PICKUP-ACTION.md).
- VR has no wait/restraint controller and no Menu Framework pages. VR presentation, input,
  lipsync and subtitles each need their own headset acceptance. Flat offsets must never be
  reused in VR.
- Scope amendment of 2026-09-08: companion affinity is excluded. Server TTS scope is XTTS,
  Chatterbox, PocketTTS, Cartesia and Inworld.
- Server native embedding-space work is deferred and is **not** alpha-ready. It lives on
  Synthserver `codex/dialectic-parity-core` at `3808838`. Its migration IDs
  `20260908001`Ã¢â‚¬â€œ`006` sort below the baseline's `20260909001`, so they must be renumbered
  before that work resumes. See the Synthserver handoff.
- The flat package audit verifies only SYNTH-owned ESP/PEX bytes. A Script VM call or record
  behavior is proven only by in-game evidence.

## Retired ledgers

The 2026-10-06 cleanup removed `docs/DIALECTIC-PARITY-GOAL.md` (the chronological parity goal
ledger), `CLAUDEX-TASK.md` (a superseded task assignment) and `CONTINUATION-NOTES.md` (a July
checkpoint). They remain in Git history at `f34da88` and in a private archive. The ledger
recorded workstation-specific deployment receipts, hashes and backup paths. It was not a current
specification. Older documents that cite it for "exact hashes" or "remaining gates" now point
here. The current open requirements are the gaps above plus the
[alpha acceptance checklist](ALPHA-ACCEPTANCE.md).

Older local source snapshots were reconciled against Git history and contain no unique source.
Private crash dumps and ad-hoc diagnostic scratch are intentionally not imported.

## Historical receipts and placeholders

Feature docs keep dated deployment and validation receipts as historical evidence. Their dates,
commit hashes, file hashes, runtime versions, record IDs and the fixed server path
`/var/www/html/Synthserver` are original. Workstation-specific locations were replaced with
these placeholders:

| Placeholder | Meaning |
|---|---|
| `<client-worktree>`, `<server-worktree>` | The local checkout used for that receipt |
| `<clean-audit-checkout>` | A clean detached checkout used for a CI reproduction |
| `<MO2-mods>` | The Mod Organizer 2 `mods` folder of the test modlist |
| `<Fallout4-install>` | The Fallout 4 game folder |
| `<wsl-distro>` | The WSL distribution that hosted Synthserver |
| `<scratch>` | Ad-hoc probe scripts and scratch output, retained outside the repository |
| `<private-backup>`, `<server-backup>` | Pre-deployment backups (Windows and WSL), retained privately |

The probe scripts and backups named in those receipts are external evidence. They are not part
of the repository and are not needed to build or test SYNTH.

## Baseline evidence

These checks ran on the baseline commits, not on the cleanup head. They are not in-game proof.

- Windows flat and VR DLLs built. All 17 native suites passed in each lane.
- Python packaging tests (13) and build gates (49) passed.
- Protocol: 33 schemas, 202 fixture cases and 215 mirrored files passed.
- Both real lane archives passed the release audit. That audit does not inspect PE imports:
  the baseline `SYNTHVR.dll` imported `spdlog.dll` and `fmt.dll` (see the known gaps).

## Licensing and publication

See [PUBLICATION-CHECKLIST.md](PUBLICATION-CHECKLIST.md). SYNTH's own code is licensed under
the GNU GPL v3.0 (root `LICENSE`, chosen 2026-10-06). Both release packages ship that file,
and the release audit requires its exact bytes. `LICENSES/DIALECTIC-MIT.txt` covers the reused
Dialectic material, and every third-party notice keeps its own terms. History and publication
decisions remain open.
