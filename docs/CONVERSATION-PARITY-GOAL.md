# Conversation lifecycle parity goal

Status: active; implementation and publication authorized 2026-09-09. No game launch authorized.

## CHIM queue-parity checkpoint (2026-09-13)

The renewed goal extends client `6f4db3e` and server `dee17b8` on independent
`codex/chim-queue-parity` branches. Preserve the scalar-FormID wait bridge crash fix
and current-input prompt fix. Current reference source: CHIM unstable `78f50d31`
and HerikaServer unstable `7556404c`, not the older dirty local CHIM checkout.
CHIM also has hard cancellation and generation guards; these are not unique to SYNTH.

| Transition | Required behavior | Implementation/evidence |
| --- | --- | --- |
| Player input admitted | Cancel old generation/turn descendants; current input wins | Existing turn cancellation, independent control lane and reserved queue capacity retained |
| Child dialogue received while parent plays | Prepare without presenting or acting | First voiced child line polls TTS and verifies/downloads WAV on the existing single speech worker |
| Child complete and parent delivered | Fresh scene proof, then ordered delivery | Existing complete-response gate retained; cached bytes reused only after take and fresh release capture |
| Menu pause before dispatch | Hold exact target/parent; recapture on resume | Paused capture handoff retained, including pause during context publication |
| Capture ages before child RPC | Bounded fresh retry; no duplicate child | Two recapture retries, only before trigger admission; no replay of ambiguous/in-flight model requests |
| No next speaker | Explain intentional stop | Server ACK detail is one of no_candidate, private, budget_exhausted, ineligible; client maps to closed trace stages |
| Interruption/failure | Never resurrect speculative output | Parent token, incomplete-stream rejection, media failure retention and existing scene admission remain authoritative |

Preparation retains at most one 16 MiB WAV per speculative child, adds no thread,
provider, model request or schema, and cannot enqueue captions/playback/actions.
Later lines use normal FIFO speech preparation, avoiding a wait for every child's
audio before the first can play. Unsupported WAV encoding still follows native
playback's existing text fallback; this change does not add a codec. Server TTS
could already run early; the new overlap is client status/download/hash work.
An expired speculative response still ends rather than surviving an unlimited pause.
After the delivery worker accepts the fresh scene, a separate once-only release
state ends the speculative timeout. Slow later speech preparation cannot expire
an already admitted child or its descendants. The parent/session cancellation
chain and ordinary speech deadlines remain active. Taking the buffered lines is
not release; media remains inaccessible until that worker admission succeeds.
The fake-server regression reproduced cancellation after take on the earlier
checkpoint and now checks pre-release expiry, once-only release and subsequent
turn cancellation. The release trace records actual worker admission, not merely
submission to its queue.

Baseline: the user-confirmed successful flat session recorded input admission at
monotonic ms149267209, player playback at149269443 (2.234 s), and first NPC playback
at149277267 (10.058 s). The first NPC line waited7.448 s from receipt to speech-ready;
ready-to-audio-queued was10 ms. These are one-run stage measurements, not p50/p95.
The early child returned status-only in94 ms. A read-only database inspection proved
mode=tight and terminal_reason=no_candidate. There is no voiced-child baseline from
that run and no measured in-game latency improvement yet. A later first-chance AV
candidate occurred alongside exit-save activity; fatality/cause remain unconfirmed.
Do not change native offsets/hooks based on that candidate.

Automated acceptance uses existing fake-server cache/release/cancellation tests,
task-lane FIFO/control independence, lifecycle, scene and protocol suites. PHP
rechat policy/identity/complete-framing and compact-current-input suites remain
required. No new test harness, runtime database table, UI, affinity system or TTS
provider is included. CHIM/HerikaServer are references only; Stobe/Dialectic need
no port because this changes SYNTH's own speculative client and ACK diagnostics.

### User-run acceptance still required

1. Load the same save manually and send a distinctive question. Confirm current
   input is answered once, with player TTS before the intended NPC and correct voice.
2. With three eligible NPCs present, exercise tight/conversational/group modes and
   ten turns. Distinguish expected private/no-candidate/budget stops from failures;
   do not change persistent defaults merely to force a chain.
3. Interrupt during generation, pending TTS, media preparation, active playback and
   prefetched continuation. Require no stale speech/action or duplicate next turn.
4. Pause/resume around response completion and continuation capture; move the target
   away and load a save with work pending. Require valid resume or explicit rejection,
   never retargeting, a freeze or output from the previous session.
5. Exercise one failed TTS and a delayed/incomplete response with controlled fixtures;
   require bounded fallback without a permanently blocked queue.

Correlate trace by generation/request/line. `media_prepare_started`, `speech_ready`,
`media_prepared`, `media_reused`, `audio_queued`, `playing`, `spoken` and continuation
stop stages distinguish preparation from delivery. Record input-to-first-player and
first-NPC audio separately, parent-spoken-to-child-playing, and interruption-to-silence.
Compare p50/p95 only after repeated matched runs; audible stop/facing/subtitle proof
cannot be inferred from `cancel_acknowledged`. Flat acceptance and VR headset proof
are separate. The goal remains active until the user-run conversation matrix passes.

## Outcome

A player can address an exact NPC using text, push-to-talk or open microphone, hear ordered player/NPC speech, receive a correctly addressed multi-NPC continuation without an unnecessary generation gap, and interrupt or leave the scene without stale speech, actions or continuation returning. Failures must have bounded, visible terminal outcomes. Builds alone do not establish this outcome.

## Baseline and scope

- Source: RANGROO/SYNTH and RANGROO/Synthserver, current work on codex/response-flow-parity. Preserve and publish existing authorized code before the new lifecycle changes. Never publish credentials, runtime voice catalogs/media, saves or build output.
- References: CHIM 9e7eabbdc841c2ed17c5aba07bdbde15f38bb0fa; HerikaServer 2076ba12f54c7a79e75a36b5a587e6bc305b6c84; Dialectic 35d1e0af3b11cc27911d0c12eab864efffbe6125; DialecticServer c4c5fbc41d0050c38c7a7fa758c30b2bd3895150. Compare behavior rather than copying engine-specific code.
- Preserve canonical identity, session/turn/generation ownership, strict envelopes, bounded workers, game-thread engine access, current voice configuration, native subtitles and authored Wait Here records.
- TTS providers remain XTTS, Chatterbox, PocketTTS, Cartesia and Inworld. No affinity feature, unrelated server port, release, main-branch merge or new provider.

## Implementation order and acceptance gates

### 0. Preserve the current baseline

Review outgoing source and unpublished history, run existing relevant checks, commit and push the existing feature branches. Record exact hashes. Separate prior implemented behavior from game-proven behavior. Review existing menu changes through the UI skill. Do not pretend the accumulated baseline is a small new fix PR.

### 1. Finish the input and ownership audit

Trace text, push-to-talk, open mic, player TTS, NPC response, action result and continuation through both repositories. Write an exact entry-point/owner/cancellation matrix. Retain explicit recipients across menu pause; do not silently substitute a nearby actor. Distinguish speaker, addressed listener, selected recipient and eligible audience. Resolve divergence before implementing continuation overlap.

### 2. Delivery-aware continuation

Use existing audio/presentation ownership to expose utterance start and terminal outcomes (spoken, text-only presented, failed, interrupted). Select one final eligible response line only after valid response completion. A server-completed response is not a client-delivery acknowledgement. Text-only presentation must finish before the fallback continuation window. No new large test harness: extend existing audio, presentation, fake-server and protocol suites at the actual boundary.

### 3. Fresh scene handoff

Capture/revalidate on the game thread before dispatch and before playback; workers receive copied snapshots. Check generation, scene, exact actor identities, availability and eligible audience. Preserve immutable parent-response ownership while negotiating any fresh-context protocol extension on both sides. Never silently replace the parent context identifier with an unrelated latest request. Add negative cases for scene change, missing actor, stale snapshot and ambiguous names.

### 4. Safe next-turn overlap

Once the terminal response and final utterance are known, prepare the next turn during final-line playback. Keep audio ordered and only one continuation launch per parent. Action-dependent continuations wait for their owned action result. Cancel prefetched work on interruption, scene invalidation or playback failure according to the explicit delivery policy. Preserve a completion-time fallback when early launch is unsafe. No unbounded prefetch or added provider calls for discarded duplicate launches.

### 5. Unified interruption and observability

Verify every input route invalidates the same owned network, speech, caption, facing, action and continuation work. Menu open/cancel pauses/resumes rather than manufacturing a new turn; submission and Stop cancel. Load/new-game/menu exit invalidates generations. Log bounded identifiers and timestamps, not secrets or raw conversation contents: input admitted, first line, TTS ready, playback start/end, continuation dispatch, cancellation acknowledged. Measure first-response latency and between-turn gap separately under equivalent conditions; do not promise a numerical speedup without measurement.

### 6. Publish and deploy the matched pair

Run native suites, protocol byte parity, focused PHP suites, source gates, x64 build and package audits; preserve configuration and operational data. Push exact source commits on feature branches and deploy client to `<MO2-mods>/SYNTH_dev` and server to `<wsl-distro>:/var/www/html/Synthserver`. Verify DLL/PEX/ESP hashes, HTTP health and preservation evidence. Report VR build/package results separately from flat and never claim headset acceptance.

## Manual acceptance (user runs Fallout)

1. Player TTS -> Preston -> Danse -> player interruption: exact voices, recipients, subtitles and ordering.
2. Interrupt independently during generation, TTS/download, playback and next-turn prefetch; no old work returns.
3. Open paused chat, cancel it, reopen and submit to the retained exact actor; exercise any configured binding.
4. Actor leaves/disables or player changes scene: no stale speaker/action/continuation.
5. TTS unavailable: readable fallback, no premature or duplicate continuation.
6. Action response: correct effect/result ordering, including Wait Here retaining ownership after vanilla comments.
7. Save/load while work is pending: no hang and no prior-generation output.

## Completion reporting

Each stage must record source evidence, automated checks, publication and deployment separately. The engineering work can be ready for manual acceptance; end-to-end gameplay parity remains unproven until the user supplies matching runtime evidence. Do not close the goal merely because builds pass or a deployment finishes.

## Execution evidence: 2026-09-09 baseline checkpoint

- Existing work predates this goal: the client has 365 and server 428 unpublished commits beyond origin/codex/full-dialectic-parity before this checkpoint. All authors in those ranges are RANGROO. Publishing the feature branches preserves that history; it is not approval to merge the accumulated changes to main.
- Native validation: all 17 executables passed. Protocol verification: 33 schemas, 191 fixture cases, 204 canonical files matching the peer. Build guard tests: 44 passed; package tests: 13 passed. Flat x64 build passed using the pinned dependencies in `<client-worktree>/out/dependencies`.
- Server baseline: 183 focused tests / 4480 assertions passed under WSL PHP 8.2.32. Release audit passed (428 PHP files, 195 JSON files, three World Knowledge checksums). Provider-failure fixture warnings are not live provider acceptance. Existing legacy driver inventory is not a new provider scope expansion.
- Server baseline published: b844e925ed6c762ce9db0a618adf853e21986f68 on origin/codex/response-flow-parity; remote SHA verified.

### Entry/ownership audit

| Input | Current admission / target binding | Required outcome |
| --- | --- | --- |
| Typed chat | prepare_chat_target retains identity before pause; submit_text validates it against a new snapshot; queue_text_for_actor begins the replacement turn | Preserve this explicit-recipient behavior; verify menu callback ownership and reject stale drafts |
| Push-to-talk | begin_voice_capture cancels old work after microphone starts; end_voice_capture selects from the ending snapshot | Retain intended recipient at speech admission and revalidate at submission; do not switch NPC simply because the player looks away |
| Open mic | confirmed peak begins a replacement turn; end_voice_capture uses the ending snapshot | Bind recipient when speech is admitted, retain voice ownership until final audio submission |
| External Ask | exact actor identity and external owner gate, then queue_text_for_actor | Preserve scoped exact-actor interruption; not a generic bypass of active conversation ownership |
| Player TTS | server durable speech jobs share the response; single client speech worker preserves order | Explicit delivery-aware ordering must also cover failed player TTS and text-only presentation |
| Continuation | last eligible NPC callback replaces pending_rechat; global idle permits dispatch with original snapshot | Final validated response plus delivery outcome and fresh-scene authorization; exactly one child |

CHIM has a shared PlayerConversationRouter with explicit UI target, true-crosshair and other deliberate modes. Those extra automatic routing modes are not permission to weaken SYNTH's explicit-target behavior. Dialectic has a bounded PlayerInputTtsGate plus explicit release on arrival, failure and scene change; SYNTH must establish equivalent ordering without copying that engine-specific bridge.

### Contract constraint discovered during implementation planning

Synthserver's synth_rechat_completed_parent_evidence, chain reservation and history checks require the parent and child to share context_sequence. Therefore a fresh scene cannot safely be implemented by merely publishing a new context and changing the child's binding. Keep the immutable chain context/parent hash and add a negotiated, separately owned scene-validation reference (or equally strict explicit contract) with matching server tests. This is a security/correctness prerequisite to early generation, not a cosmetic client snapshot replacement.

### First implementation increment: local delivery gate

Implemented shared per-utterance delivery state across audio, text fallback and continuation. The continuation parent is not ready until its valid response terminal callback and either audio-buffer completion or completion of the fallback reading window. Cancellation remains authoritative. Queue eviction and interrupted playback cannot count as delivered; a later ineligible NPC line removes an earlier continuation candidate. Fallback timers pause alongside speech/menu presentation. VR advances the shared fallback timer while retaining its existing notification display; no new VR HUD claim is made.

This increment does not yet implement server delivery receipts, fresh-scene negotiation, retained voice-input targets or generation overlap. Existing action timing is preserved; caption completion gates continuation, not a new global action delay. A text-complete outcome is a reading-window result, not proof that a renderer displayed it or that the player heard audio. Existing presentation tests now exercise shared outcomes, pause, cancellation and queue eviction; full native and both DLL builds are required before publication.

The unchanged baseline was deployed successfully to the stated targets before this increment. Baseline DLL SHA256: 21EC73689E4D2C6E15BBF69E6963C1729ECA28554324A4EBD264F038CA40BEE6. Client custom INI SHA256 remained 33615F103948696C045CA9C3894DAC8D6D64ADC7AFBB636C37A37A3DCEA83E5E; server voice-tree digest remained 4de2219474167424359b8008d43ab9208a48c1af33b6de3c263005b6990ee737. Server health returned HTTP 200. Neither game was launched.

### UI review corrections

The bounded Claude review returned Fable 5 model metadata and seven findings; Codex verified the source before changes. Confirmed and corrected: the framework's duplicate untargeted chat opener (all registry callbacks now no-op; game-thread polling is authoritative), clipboard fallback on busy/initializing installed menus, stale deferred control target/action state on same-generation session retirement, stale chat/control key latches across blocked menus, and duplicated model-slot lookup with empty RPC submission. Existing input tests and a narrow build integration guard protect the bridge/routing boundary. Source review and builds do not prove render-thread dispatch or actual menu focus in Fallout.

Remaining review items for the goal: verify submenu focus against the pinned framework shim before changing it; examine pending settings/voice-import UI state across session retirement; determine whether ownership-loss notification is safe after the runtime/session may have retired. Do not act on the review's speculative focus finding as if it were game-proven.

## Current checkpoint

- Preserved baseline branches are published: client codex/response-flow-parity at fb9c2e061053a7d6c8523b1938ae93ac41530fcf; server codex/response-flow-parity at b844e925ed6c762ce9db0a618adf853e21986f68. Both remote hashes verified.
- Active client work: codex/conversation-lifecycle-parity in `<client-worktree>`. Initial fixes published at 527cdb66339fed8fd2bf0e7018ba27f26dc40374; draft PR https://github.com/RANGROO/SYNTH/pull/2 targets the preserved baseline branch, not main.
- Both DLL builds and audited packages passed, 17 native suites passed, 45 build guards and 13 packaging tests passed, 191 protocol cases / 204 peer files matched. This is automated source/build evidence only.
- Matched local deployment succeeded after the fixes, client code revision 527cdb66339fed8fd2bf0e7018ba27f26dc40374 and server b844e925ed6c762ce9db0a618adf853e21986f68. Client DLL SHA256: 0A376F9E37183F36F1D2E2F420492E08AE2E5C1D43FE9CA547F0163E9401957C. Server health HTTP 200. The custom INI and voice-tree preservation hashes above remained unchanged; original PEX/ESP hashes also matched the audited baseline. No game launched and no manual acceptance claimed.
- Goal remains active. Next: retained voice admission, remaining verified UI ownership issues, fresh-scene protocol design/tests and implementation, then safe generation overlap and trace-based validation. Do not call this checkpoint full parity.

### Clean-checkout / CI follow-up

The first GitHub runs failed despite local success. Reproduced the protocol failure under Linux in a clean detached audit checkout at `<clean-audit-checkout>`: pathlib sorted README differently on Windows. Fixed explicit POSIX ordering and mirrored the manifest; Windows and Linux verification now pass. Client source 30048b6 also fixes Chocolatey PATH propagation, the flat DLL artifact path, and the VR workflow's incompatible classic install command against its manifest. Server 0118b27 mirrors the manifest on codex/conversation-lifecycle-parity, draft https://github.com/RANGROO/Synthserver/pull/2. Both feature branches were pushed; both local deployment stages succeeded again with the same DLL/script hashes and HTTP 200.

Clean Linux checkout now passes protocol and 45 source guards but fails 11 of 13 packaging tests because out/wait-papyrus/SYNTHWait.pex is absent. Local packages pass with the independently compiled, source-bound original script. Do not weaken the package audit, fabricate proprietary game imports, or claim hosted CI success. CI needs a reproducible, authorized way to obtain/build the original script artifact and its verified import provenance. This is distinct from the ongoing conversation lifecycle implementation; continue the other in-scope goal stages while recording this validation limit. The detached audit checkout contains no unique product edits and has not been removed.

### Second implementation increment: retained voice recipient

Push-to-talk now captures the intended recipient and control selection when recording starts. Open mic does so at the first admitted speech peak. Submission captures a fresh scene including that actor and uses the same canonical-identity, availability and distance checks as retained typed drafts. Looking away cannot select a different NPC; missing/replaced recipients fail closed. Narrator/log modes remain untargeted and use the admitted mode rather than a later settings change. Flat and VR hosts verify the same session and generation still own each boundary capture before dispatching it.

Cancellation on menu pause, Stop, combat or mic mute clears the retained recipient and releases only the unresolved recording owner. Existing named actor ownership is preserved. Idle/recording open-mic frames still do no scene capture; this adds one admission snapshot per utterance, not per-frame work. No in-game capture-time measurement or latency improvement is claimed.

Validation: both x64 DLL builds and audited packages passed; all 17 native suites passed in both build lanes, plus 45 source guards, 13 packaging tests and the unchanged 191 protocol cases / 204 peer files. The existing input suite now exercises canonical recipient identity independently of name/pose, including reference, plugin, playthrough and base-identity mismatches. Source guards check both runtime entrypoints and retained controls; these are not microphone-device or gameplay acceptance.

Next substantive gap remains fresh-scene authorization for continuation, then safe overlap and end-to-end interruption traces. The retained voice scene is not a substitute for that protocol work. No server wire contract changed in this increment.

Published and deployed voice increment: client 914b5ecf5b181a250bcd7d2b8ff9210bf34e523a, server 0118b2795b7818bffad0a80dcd44c625776ab36f, both on codex/conversation-lifecycle-parity. Full deploy reported success for both stages and HTTP 200. Deployed DLL SHA256: 3FC742B3DAC07DC1F46A98037B5CFB07F6632DE3E9D6EA13CB8DA2B5BB085E40. Custom INI, both original PEX files and voice-tree digest matched the earlier preservation values. Game remains unlaunched.

Additional hosted-run failures were traced to older xmake rejecting multiple positional targets and hosted vcpkg lacking the manifest baseline object. Build tooling now builds the default engine-free targets and explicitly fetches/verifies the pinned vcpkg baseline before configuring VR. Local validation does not close hosted CI acceptance or its separate Papyrus prerequisite.

### Fresh-scene contract foundation

Added capability-gated scene_context_sequence alongside immutable rechat parent ownership in both protocol trees, with native encoding and PHP ingress checks. Positive safe integer and strictly-newer sequence checks are independent of unchanged parent request/line/turn/context. Existing parent-only clients remain unchanged. Focused negative fixtures and native/PHP cases cover missing reference/capability, invalid trigger, zero, stale/equal and out-of-range sequence. Runtime defaults do not yet advertise this capability, and server preparation explicitly rejects scene-bearing requests before database/profile/provider work rather than ignoring the field.

This is a staged prerequisite, not fresh-scene activation or a new gameplay improvement. Keep the guard until the following coupled consumers are implemented and tested:

1. Publish a game-thread snapshot at continuation dispatch, retaining the prior speaker and exact generation; reject cancellation or excessive capture age before network submission. A null targeted capture must terminate pending recording/continuation rather than retrying a stale target each frame.
2. Resolve the named context under the existing native request lock. context_snapshots has received_at; request_executions has claimed_at. Validate age against admission time, not repeated wall-clock checks that would invalidate a long-running model response. Require the same live session/generation/player/playthrough, a sequence newer than the previous continuation's scene, and matching physical scene identity.
3. Existing WorldState publishes cell/worldspace labels (with FormID fallback only for unnamed cells). Labels alone cannot establish exact physical scene identity. Add copied canonical cell/worldspace references before claiming same-scene validation; do not confuse equal localized names with equal cells.
4. synth_protocol_current_context must expose the validated scene for responder selection, prompts, actions and listener routing. synth_rechat_record_emitted_route currently reads the root context directly; update that consumer too. Preserve completed-parent hash checks and reservation ownership.
5. synth_protocol_history_rows_cte currently resolves witnesses from history_events.context_sequence. A newly arrived actor in the fresh audience must not be silently selected while history remains attributed only to the root audience. Carry or derive the exact scene provenance for witnessed history without changing chain ancestry/context ownership. Test removed and newly arrived actors independently.
6. Add an explicit correlated support acknowledgement, then enable the client capability only for a server that proves support. Extend existing native/fake-server and disposable PostgreSQL tests for wrong session/generation/scene/player, missing/stale publication, ambiguous names, retries and one-child ownership. No real provider call is needed for these boundaries.

Contract checks currently pass with 33 schemas, 196 fixture cases and 209 byte-identical peer files; the focused schema/ingress suite passed 69 tests / 1596 assertions. Full native builds and package audits remain required for this checkpoint. These counts do not prove the activation items above.

Checkpoint validation completed: both x64 DLLs rebuilt, all 17 native suites passed in both lanes, 45 source guards and 13 packaging tests passed, and both packages audited. The server tracked-tree lint/audit passed. Also fixed the null retained-actor capture loop in both hosts: cancel recording and clear the key latch when lookup fails while the same session still owns the capture. Hosted VR now passes and flat reaches packaging; the remaining observed flat failure is the missing original SYNTHWait.pex/provenance bundle, not compilation. Fresh-scene runtime activation remains incomplete.

### Physical scene capture and owned scene reader

Added copied cell/worldspace FormIDs and originating plugins to WorldState in the shared core and CommonLib capture used by both runtimes. Missing metadata yields unavailable identity. The negotiated v2 world.scene field contains closed cell/worldspace references; interior worldspace is null, exterior requires an exact reference. Existing peers retain their original wire shape. Native/PHP/schema tests cover identity preservation, duplicate-label independence, missing capability, zero cell and interior/exterior mismatch. This adds bounded native metadata reads and string copies to existing capture, not network or filesystem work; no in-game performance measurement exists.

The staged server reader now resolves the exact scene under the actual native owner and immutable ingress hash, same active session/generation/lane/player, completed parent, advancing previous-scene sequence and a ten-second admission-time freshness bound. It compares canonical physical scene references and retained target identity; it does not use latest-global context. current_context can consume this view without overwriting newer observed inventory with older action ancestry. The preparation activation guard remains in place: no new client capability defaults or support acknowledgement yet.

The initial read-only missing-owner probe has now been followed by a seeded, isolated PostgreSQL probe using the production advisory-lock adapter. All 18 checks passed: owned scene, admission-age bounds, cancellation/turn/generation/lane/body/parent mismatches, physical scene and actor changes, newly arrived speaker emission and listener routing, historical speaker membership and later-scene isolation. Only the probe-owned disposable database was removed; runtime synth and existing testdb were not reset. Focused PHP tests passed 165 / 4446 assertions.

Listener routing and completed-ancestor history now consume each continuation's exact scene under the real live owner's lock. The historical reader does not impersonate the ancestor in request globals. Root chain ownership remains unchanged. Action-result context/routes/history now inherit the scene through canonically accepted receipts and proven emitted actions; continuation after an action must advance beyond that inherited scene. Inventory overlays stop at the fresh capture but retain post-capture mutations. The expanded isolated PostgreSQL probe passes 23 checks, and the existing RequestStreamingTest passes 96 tests / 2843 assertions with the new inventory boundary regression.

Schema 20260909001 now records nullable durable scene provenance separately from root history ownership. Shared witnessed-history readers use that exact scene while preserving saved frontiers, and memory preparation binds the actual responder. No old history is backfilled. The isolated PostgreSQL probe passes 30 checks, including populated migration repeatability, later/legacy/saved visibility and responder locking. Responder-lock proof stops at zero game time rather than claiming generated memory summary publication; provider generation remains separately untested.

The client now stages a game-thread capture handoff in both flat and VR hosts. It retains the emitted actor's full identity, captures outside PluginSession, checks the same session/generation after capture, and discards missing/replaced/changed-scene or more-than-two-second captures. The worker publishes the fresh snapshot and changes only the separate scene reference. Cancellation, stop, actor stop and quiescence account for the handoff slot. This has compiled-runtime evidence, not game-thread execution proof.

The existing v2 init status has an optional rechat_scene boolean acknowledgement. Client readiness requires a complete, correctly correlated init, the offered capability and true acknowledgement. A new generation clears it; missing acknowledgement refuses the session without changing its capability manifest. Existing flat feature acknowledgements remain independent, and the fresh-scene check covers VR too. Existing fake-server tests exercise both lanes, stale correlation, truncation, failure and readiness reset.

Client handoff checkpoint validation: flat and VR x64 DLL builds and all 17 native suites pass, including 1387 fake-server checks. The 45 source guards, 13 packaging tests, both package audits and 202 protocol cases / 215 byte-identical peer files pass. This does not simulate runtime capture retirement, native input devices or game presentation; no game was launched.

### Real preparation checkpoint

The server preparation coordinator now validates the owned fresh scene before policy reads, cached choices or reservations, replacing the temporary unconditional rejection. It does not advertise support: server acknowledgement and client default capability remain disabled together.

The disposable PostgreSQL probe now calls the actual coordinator instead of inserting a prepared selection manually. All 34 checks pass, including selection of a newly arrived actor, exact replay, rejection of a stale scene on the cached path, and rejection of a second child without spending another round or inserting a selection. Existing route, action, history and additive migration checks also pass. It uses the production advisory-lock adapter and source schema; no model/TTS calls, runtime database reset or game launch occurred. Existing PHP suites pass 165 tests / 4450 assertions.

The consumer audit found a concrete activation gap: synth_memory_run_persisted_job still joins the root context target to the job actor, while capture now binds the actual continuation responder. This rejects legitimate jobs for a different responder. The dispatch capacity check and rebuild lock also use the root target. Resolve those through the same proven response identity, retaining the immutable job ingress/context owner, before enabling the handshake. Verify a persisted job with different root target and responder, not only the existing zero-game-time lock probe. Relationship-derived evidence also retains root-target assumptions; keep excluded affinity out of scope and explicitly establish its activation boundary.

Memory consumer follow-through: capture, dispatch capacity, rebuild locking and persisted execution now use one locked response-context resolver. Persisted jobs retain the original ingress/context owner but validate their actor against the actual responder; claim/publication requires the same batch actor. The isolated PostgreSQL probe passes 37 checks, now including actual capture, enqueue, lease claim and summary publication for responder 2 while root target remains 3 and context owner remains 7. Wrong-actor and retired-generation jobs do not invoke the summary callback. The successful deterministic callback runs outside SQL locks; no model/embedding provider or gameplay was exercised.

### Fresh-scene activation

Both runtime defaults now offer dialogue.rechat.scene. The server emits rechat_scene=true only for v2 init with turn ownership, parent ownership and scene support together, in flat or VR. The existing flat feature status code is preserved. Missing prerequisites, v1, non-init events and non-init status cannot acknowledge it. The client already requires complete correlated acknowledgement and clears readiness across generations. This activates the existing game-thread capture/worker publication path; it does not add generation overlap.

The relationship audit traced optional model-speech metadata through response capture, durable storage and derived readers. It is post-completion work and cannot change the emitted reply; its root-target evidence ownership still needs adaptation. That is a remaining derived-feature limitation, not a reason to claim the verified dialogue/context/history/memory path is still disabled. No relationship settings were changed and excluded companion affinity was not added. Do not claim relationship-derived fresh-scene parity from this activation.

Validation: both x64 DLL builds, all 17 native suites in each lane, 45 source guards, 13 packaging checks, both audited packages, 202 protocol cases / 215 matching peer files, 166 PHP tests / 4480 assertions, and the 37-check disposable PostgreSQL preparation/memory probe pass. Those are separate source/build/database boundaries, not one running-game end-to-end test. No game, microphone, headset or live model provider was exercised.

Next: safe generation overlap and interruption/terminal-reason traces. Revisit remaining typed/periodic capture expressions that call session methods around runtime capture; the new voice/continuation host guards do not prove those older paths safe. Continue the derived-evidence ownership audit before calling full parity complete. Fresh-scene support is enabled for manual testing, not gameplay-verified.

### Capture lifetime follow-through

Both hosts now split profile refresh, diary checkpoint, visual capture and background activation/publication into capture, session-pointer/runtime-generation revalidation, then submission. Flat rest capture, typed targeted/untargeted submission and chat opening are guarded; VR clipboard submission is guarded. External-request draining retains its internal epoch rejection and now also stops the caller's frame if capture retired the session. Flat clipboard fallback rechecks ownership after the Win32 read. This removes member calls whose receiver could be evaluated before a reentrant capture destroyed it.

Claude Opus 5 performed the bounded read-only chat-path review and confirmed the lifetime hazard. The implementation follows the existing guard idiom without new helpers or menu behavior changes. It deliberately returns quietly on retirement rather than opening a notification against a departing session. Existing source assertions that required unsafe inline capture were replaced with guarded-submission checks; those guards do not simulate native reentrancy.

Both x64 builds, 17 native suites in each lane, 45 source guards, 13 packaging checks, both package audits and unchanged protocol peer verification pass. No game or headset was launched. Safe generation overlap, interruption traces and derived-evidence ownership remain unfinished; these capture fixes do not close those gates.

### Bounded generation overlap

The pump can now prepare one next response while the completed parent's final NPC audio line is playing. Eligibility requires exact caption identity, completed response, no active response/action/speech-fetch work, no queued action and no fallback caption. It uses the existing owned fresh-scene capture/publication path. Text-only delivery and action-dependent turns retain completion-time dispatch.

The speculative child has a cancellation source descending from the parent's turn and a single bounded response buffer (128 parsed lines, still inside the existing 4 MiB response limit). Nothing reaches client subtitles, audio or actions until both complete response and exact parent delivery are proven. The pump uses a nonblocking buffer read, then schedules delivery on the network lane, not on the game thread. Release is once-only. Parent discard, turn cancellation, timeout, failed response and overflow reject the buffer; no duplicate continuation is sent as a retry. Stop/actor stop and external-admission ownership include prefetched state. This overlaps generation, not client audio download/decode; no numerical latency improvement is claimed.

Existing fake-server suite now has 1402 checks, including incomplete parent/response, one-time release, failure, expiry, overflow and parent cancellation. Both DLL builds and 17 native suites per lane, source/package checks and audited packages pass. These buffer tests plus host source gates do not exercise a native audio device, game-thread scene transition during prefetch, or server-side heard/delivery receipts. Interruption/terminal traces, scene-change acceptance, derived-evidence ownership and user-run game acceptance remain open.

### Prefetch release capture

Delivery completion now moves the buffered response into the existing host capture handoff instead of immediately delivering it. Both hosts capture again outside PluginSession and retain their session/generation guard. Release reuses the exact scene/player/parent identity checks and then validates every NPC speaker, action actor and action target against copied original identity plus the new capture's availability/range. It never substitutes a new actor. Failed/missing capture cancels the child without cancelling a newer player turn. Accepted output is delivered on the network worker using the release snapshot; a capture older than two seconds at worker admission is discarded. This second capture does not send another rechat request or rewrite server scene/history ownership.

Existing buffer tests now total 1404 checks, including independent cancellation after buffer drain. Source gates verify that buffer drain hands off to capture rather than handle_line and that release checks speakers/actions before worker delivery. This is not native scene-transition execution proof; actor departure during subsequent playback remains part of manual acceptance. Interruption traces and derived-evidence audit remain next.

### Bounded conversation diagnostics

The follow-on codex/conversation-trace branch adds native-log events for admission, response application, speech readiness, audio queueing, playback/text delivery, continuation dispatch, speculative receipt/release, cancellation acknowledgement and retirement. Prefetch failure records its first observed cancellation, expiry, discarded-parent, overflow or incomplete-response reason. Events carry monotonic milliseconds, generation, context sequence and sanitized request/line IDs, never dialogue text, audio, URLs or provider error bodies. Existing notification behavior is unchanged; trace events create no in-game notifications.

Each session retains at most 128 records and uses try-lock admission/drain with a cumulative dropped count. Each host pump drains at most eight events to its existing native logger. This is lossy diagnostics, not a complete audit ledger: retirement drains only a bounded batch, late worker events can disappear, and concurrent producer timestamps do not establish causality. Missing events alone do not prove an operation never happened. Logger sink overhead has not been measured in-game.

For manual evidence, filter native logs for SYNTH conversation and correlate generation/request/line IDs. speech_ready precedes client media fetch/decode; audio_queued is queue acceptance, not audible delivery; playing/spoken and reading/text_complete distinguish audio from fallback presentation. prefetch_received is speculative network arrival; line_applied occurs only on application after release validation. Compare matching event timestamps to localize delay without claiming a benchmark. A cancel acknowledgement is server transport completion, not gameplay proof that every old effect stopped.

Both DLL builds and all 17 native suites in each lane pass. The existing diagnostics suite now has 32 assertions covering sanitized IDs, monotonic records, bounded overflow, duplicate-state suppression and first prefetch failure reason. Source guards (45), packaging tests (13) and protocol verification (33 schemas, 202 cases, 215 matching files) pass. Remaining gates include derived-evidence ownership, clean-checkout Papyrus provenance in CI, and user-run gameplay/VR acceptance. No game was launched.

### Acceptance audit: playback admission remains incomplete

Current source inspection establishes a gap in stage 3, independent of the optional
relationship evidence work. `NativeAudioPlayback::pump` starts queued clips after checking
generation and cancellation, but has no current scene/actor proof. `DialogueCaptions::frame`
similarly starts a fallback reading window with only those ownership checks. The flat and
VR hosts call the session pump before facing/presentation. `face_speech_listener` checks
live identities later and can skip rotation, but its failure does not invalidate audio.
Facing is also absent for some legitimate lines, so it cannot serve as the delivery gate.

The existing prefetch release capture proves eligibility at response release, not at every
later utterance start. Media download, queued preceding speech or a paused menu can outlive
that capture. Do not mark stage 3 complete on the strength of release checks or facing.
This is a source-confirmed missing gate, not a reproduced game freeze or measured latency.

Next implementation must cover audio and text fallback together:

1. Bind a copied, engine-free presentation owner to each response caption: original scene,
   player/playthrough, explicit speaker/listener roles and canonical identities. Preserve
   player TTS and narrator policy; never fabricate an NPC identity from a display name.
2. Expose a bounded pending-presentation capture handoff before delivery. The host captures
   outside session calls and rechecks session pointer/generation before returning proof.
   Never call native capture from an audio lock, network worker or caption mutex.
3. Hold the exact queued utterance until its proof is accepted. No frame between worker
   enqueue and host validation may start it. Validate canonical scene, player, availability,
   exact actor identity/range and proof freshness using shared copied-snapshot rules.
4. Revalidate expired proof after menu pause or queue delay. Unavailable/invalid proof
   discards that utterance and invalidates its continuation rather than retargeting or
   converting an invalid speaker into an unvalidated text fallback. Transient queue-lock
   contention means pending/busy, not invalid and not permission to start.
5. Active-scene/actor invalidation must stop owned delivery and speculative child work;
   decide and test that lifecycle separately from start admission. Do not equate failure
   to rotate a moving actor with actor invalidity. Stop/new input/save retirement remain
   authoritative and cannot resurrect a previously rejected caption.
6. Extend existing presentation/fake-server suites for pending admission, exact once-only
   start, arrival between frames, pause-expired proof, identity replacement, actor departure,
   scene change, cancellation, player/narrator and TTS-failure fallback. Build both lanes,
   audit packages, push and deploy the matched pair, then obtain user-run game evidence.

Priority is this explicit conversation acceptance gap before additional score/evaluation
work. The server audit and its unresolved evidence limitations remain recorded in
Synthserver's retired parity ledger ([handoff](HANDOFF.md#retired-ledgers)); no settings or affinity features are authorized by
this prioritization. Current deployed client is 15fcfbb; server is daf4885. No runtime code
changes or new deployment are implied by this audit checkpoint.

### Per-utterance scene admission implementation

The codex/presentation-scene-admission follow-on branch binds a copied original scene and
exact speaker/listener identities to every production caption. Both hosts inspect one
pending or active caption before pumping playback, release queue locks, capture through
the runtime adapter, then recheck session/generation before submitting the snapshot.
Audio and text share the proof. Worker arrival between host inspection and the pump stays
pending. Player/narrator lines still require their original scene/player; any explicit NPC
listener is retained without inventing a narrator actor. Failed TTS does not bypass the gate.

Proof expires after 250 ms. Audio start/resume and text start wait for fresh proof; active
audio stops advancing while its proof is expired. A failed scene, identity, availability
or range check rejects the caption and invokes the existing whole-dialogue cancellation
path, including speculative continuation. Late completion cannot make rejected delivery
successful. This is frame-driven revalidation, not instantaneous actor-departure detection.
Menu pause admits no captures; unpause cannot start stale audio. Input cooldowns and facing
eligibility are deliberately not presentation validity checks.

The new handoff uses try-lock queue inspection and performs at most one capture per host
frame, only for a due active/front-queued caption. At steady state each such caption can
require roughly four validations per second; an active clip, queued successor and fallback
can each be due. Native capture uses the existing batch cache where available. This adds
bounded copied-snapshot work, not HTTP/model work, but game-thread cost and audible pause
behavior have not been measured. Device-level XAudio stop/start and native scene changes
still require user-run acceptance; source and engine-free tests cannot prove those.

Existing presentation tests cover waiting before first proof, expiry, pause, reacquisition,
terminal rejection, late completion and a valid spoken caption while fallback proof is
expired. Both runtime fixtures exercise identical labels with different physical cells,
replaced/disabled speakers, future/stale captures and valid ownership. Source guards verify
capture/session revalidation precedes pumping and queues contain no runtime capture call.
Both full native suites, both DLLs, protocol peers and packages remain required before deploy.
Relationship persistence and the separate CI/manual acceptance limits remain open.

### Remaining menu ownership review

The bounded Claude Opus review invocation could not start at the invoked executable path;
Codex continued under the UI skill fallback without claiming a Claude review result.
Source inspection found that setting edits are immediate persistent INI writes, with a
pending display echo rather than deferred per-session actions. Their persistence across
settings reload is intentional; no setting reset was added. Control submenus stay in the
same window and close ownership is released after End. The pinned framework header forwards
Selectable, IsWindowFocused and SetItemDefaultFocus to their corresponding exports. This
does not prove actual submenu navigation/focus; that remains a manual UI check, not a
confirmed defect warranting speculative focus changes.

Voice-import requests were a real separate gap: an unowned atomic action could survive
same-generation session retirement. The bridge now atomically binds availability, session
revision and pending action. The renderer captures a ticket before drawing the controls;
requests from an old ticket cannot enter or overwrite the replacement session's queue.
Retirement/invalidation disables admission and drops unread requests; adoption enables a
new revision. Ordinary publication does not reset the active revision. Cancel still replaces
an unread Import All, and consumption remains once-only. No scanning, file work or network
work was added to the controls. Existing input tests cover these transitions, including an
old frame trying to overwrite a new session's cancellation. VR has no new menu surface.
No game was launched and no visual or render-thread execution proof is claimed.
