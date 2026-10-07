# Manual NPC, Player and Narrator diary test

This is a local-development acceptance check, not a claim of in-game validation.

1. Start the game yourself after deploying matching SYNTH and Synthserver builds.
2. Choose a diary connector in the target NPC's server profile. Automatic-diary enablement
   is not required for this explicit request. No configuration is changed by deployment.
3. Point at that NPC and submit the exact command `/diary` through the existing text input.
   Flat uses the crosshair. VR uses the controller ray, then HMD gaze; neither selects a
   nearby replacement when no actor is pointed at.
4. Expect a queued notification, or disabled when the profile has no diary connector.
   Queued means admitted, not generated. No player chat line or NPC spoken reply is invented.
5. Send a normal message while diary generation is pending. It must not cancel the diary
   or freeze the game. Report the approximate time so server/job logs can be correlated.
6. Wait for `SYNTH diary complete; included in your next save`. This requires server
   generation plus a fresh acknowledged game-thread snapshot. The client tracks one diary
   at a time with short background checks; another `/diary` while pending is rejected.
7. With latest-diary context enabled for the profile, a conversation after generation and
   that fresh acknowledged snapshot can include the saved diary. Save after that checkpoint;
   load both this save and an earlier save. The earlier save must not inherit the later entry.

Completion checks use two-second HTTP requests at five-second intervals when idle. Tracking
ends after 30 minutes without claiming completion. A generated diary gets up to three executed
checkpoint attempts; a failed checkpoint is reported separately. Menus, speech and active
dialogue defer capture. No old request snapshot is republished minutes later. A save made
before the completion checkpoint still excludes the generated result. Load/halt retires tracking.

Admission now returns an immutable native receipt containing the exact original request body,
session and acknowledged context. Status polling accepts that receipt alone: changing target,
role selection or the latest published context cannot reconstruct a different owner. The three
manual commands use this path. Repeat the normal-chat and retargeting checks while one is queued.

The transport also supports a single bounded batch of up to 16 unique NPCs plus independent
Player/Narrator selections, with manual/sleep/wait reasons and initialized role/observation
capability gates. Verified flat rest hooks now enable the automatic client path.
Multi-selection also requires diary.batch.status. Status remains pending while any sibling job
is pending/running. A terminal diary_partial means at least one validated result and at least
one failed/cancelled job; successful entries still require a fresh checkpoint. The client must
report partial completion, never that the entire batch succeeded. Disabled roles are not failures.
Automatic flat requests share this receipt/checkpoint path with manual commands. They use a bounded
copied-frame queue and the exact audience from their acknowledged context.

## Narrator check

1. Configure the Narrator's server profile diary connector, or its ordinary connector fallback.
2. Submit exact `/diary narrator`. No pointed NPC is required; ownership uses your saved player.
3. Expect the same queued, ready/checkpoint and next-save completion behavior as above. The
   client still tracks one diary at a time across both roles. No spoken reply is fabricated.
4. With latest-diary context enabled in that profile, talk to the Narrator after completion.
   Save, reload that save, then load an earlier save. Only the acknowledged timeline may use
   the entry. NPC diary history must remain separate. Renaming the Narrator must not change ownership.

## Player check

1. Enable the existing Player diary setting on the server. Manual requests respect this setting;
   automatic-diary enablement is not required. Deployment does not enable it for you.
2. Configure its diary connector, or use the established summary/player/profiles and legacy-driver
   fallback. Submit exact `/diary player`; no pointed NPC is required.
3. Expect disabled or queued, followed by the same next-save completion notice after generation
   and a fresh checkpoint. Check that normal conversation stays responsive while it runs.
4. Save after completion, reload that save, then load an earlier save. Report the approximate
   times for journal/log correlation. Player entries remain private and separate from Narrator/NPC
   entries; the legacy diary management page does not yet display these native journal entries.

## Flat sleep/wait observation check

The 1.11.240 client now observes the engine's GameScript sleep/wait start handlers and advertises
automatic diary capabilities only when both hooks are verified at session initialization.
The callback forwards the original call and queues only lifecycle-fenced reasons. Logging occurs
at the next safe menu-free pump, potentially after the rest menu closes, not at a fabricated
start-time snapshot. Repeated starts before consumption retain their enqueue order in a bounded
16-entry buffer. Newest starts beyond capacity, or callbacks losing bounded compare/exchange
retries, are dropped and counted in a separate process-wide health diagnostic. They are not
silently coalesced or converted into a synthetic event in another save/session.

1. Launch manually and load your save. Check for `SYNTH rest observation: exact flat sleep/wait
   start handlers installed`. A changed handler slot disables this observer without disabling
   ordinary SYNTH dialogue. Do not assume another mod's hook was chained or replaced.
2. Open then cancel the sleep/wait menu. This alone must not produce a start observation.
3. With the existing server automatic diary settings enabled for a configured role, actually sleep,
   then wait. After returning to gameplay, expect `automatic diaries queued from safe-pump capture`
   with the active generation. Admission may legitimately disable roles for policy, cooldown or absent
   profiles/connectors. An admitted batch gets a background-generation notice; completion or partial
   completion requires a fresh checkpoint. Confirm normal AI chat still works throughout.
4. Reload an earlier save, repeat, and report approximate times. Pending pre-load observations
   must not survive into the new generation. Check save/load and ordinary sleep/wait behavior.
   Settings-driven session replacement also clears pending starts; it must not replay an old
   observation after SYNTH reconnects. The process-wide drop counter may persist across loads.
5. VR has no installed rest observer in this milestone; it needs independently verified addresses
   and headset testing. The flat hook must never be copied into VR.

Copied frames expire after two minutes while awaiting admission; the queue holds at most16 and
drops newest excess starts. It never retargets queued frames to a later NPC. A stranger without an
existing profile must not block the Player/Narrator batch or create a new profile. Test save/load
while queued and save after completion, then compare the saved journal against an earlier save.

Flat automatic dispatch and server policies are implemented but not gameplay-accepted;
VR observation and native diary management remain unfinished. Flat and VR require separate manual acceptance; build/HTTP results are
not substitutes. The agent must never launch, close or control either game for this test.
