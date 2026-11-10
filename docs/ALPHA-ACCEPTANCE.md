# SYNTH open-alpha acceptance

## Stage 1 native typed turn

- [ ] Build, package and audit both DLLs with the paired server candidate.
- [ ] Flat and VR, separately: `/talk <message>` to a registered placed NPC streams
  sentence lines with NPC voice; each completed line records one heard row.
- [ ] Interrupt mid-line, then load a save: interrupted/stale lines stay unheard
  and no late audio plays.
- [ ] Unmarked typed text (Wait, Follow, gifts, Jev) still uses its existing routes;
  `/talk` with no message or to an ineligible NPC shows a useful failure.
- [ ] Subtitles-only or voice failure: text appears but the line stays unheard.

## Session ownership composition

- [ ] Flat and VR, separately, on the composed-session build: typed and voice turns, Stop,
  combat interrupt, load, rechat, an action, a diary, PipVision and a control-menu mode change
  behave as on the previous build, with no late audio or duplicate receipts.
- [x] Voice-state Stage B candidate, local prerequisite only: x64 flat and VR builds, both
  package and manifest audits, and exact packaged-DLL matches, paired with server #10 at
  `3e637c6` (server #11 is documentation-only). This is not runtime acceptance.
- [ ] Flat, on the voice-state build: with open microphone listening passively, automatic
  activation still fires; detected speech or held push-to-talk suppresses it and no automatic
  reply starts mid-speech; mute, Stop and load cancel capture without leaving it suppressed.
  VR, separately: push-to-talk and open microphone behave as before.

## Generic gift inventory exchange

- [x] Install the reviewed coordinated gift-menu DLL/PEX/backend update; installed hashes and server health verified.
- [ ] Flat: "I have a gift for you" opens the exact addressed NPC's normal free
  inventory exchange; the NPC can decline the offer in character.
- [ ] Give a weapon and ammunition, close the menu, and verify both inventories,
  the paired receipt, and personality-based gift-batch evidence.
- [ ] Opening/closing without donating, cancellation/load, a busy NPC and wrong
  menu ownership produce no received-gift credit.
- [ ] Named offers including "I have a gift for you, X" continue to use the direct
  exact observed Player-to-NPC transfer without opening the generic gift screen.


## Player-pack gifts

- [x] Install the coordinated client/backend candidate after reviewing the concrete plan; all installed hashes and server health verified.
- [ ] Flat: Jun takes the exact requested unequipped weapon from the player pack;
  paired inventory counts and receipt agree with physical gameplay.
- [ ] Compare different NPC reactions to gifts using their personalities/goals;
  confirm lasting relationship evidence follows the verified handoff.
- [ ] A failed/busy/quest-bound transfer produces no received gift; retrying the
  same receipt produces no second relationship reward.


## Equipped pistol transfer

- [x] Jun equips and unequips Heavy 10mm Pistol with successful live-state/count receipts.
- [x] With a weapon equipped, GiveItemTo transfers the exact weapon to the player:
  Jun's Submachine Gun had equipped=true before the successful transfer, with
  complete paired inventories showing donor zero and player one. Heavy 10mm Pistol
  return also passed in its separate unequipped-start case.
- [ ] Cancellation, busy/quest-bound actors and replaced/ambiguous instances
  retain truthful failure and inventory observations in flat gameplay.

## Manual diary shortcut and reader

- [x] Flat: user confirms F6 requests the crosshair NPC's diary; Sturges' entry
  completed on the first attempt in about 33 seconds, with valid published
  ownership/hashes and dashboard visibility (4 November 2026).
- [ ] VR: verify a controller-accessible diary request and saved entry in-headset.


Status: **OPEN**. The optional flat radio startup and static A/B playback checks
have passed on the isolated test save. The broader baseline (client `f34da88`, server
`bcb0c06`) remains unaccepted. Builds, unit tests, protocol fixtures and package audits are prerequisites. They do
not count as in-game evidence. Record flat and VR results separately. A flat pass never implies a
VR pass.

## October 28 flat long-play feedback

The user completed the 60-minute flat session using DeepSeek V4.1 Flash 1017 and
Cartesia. Completion records the test duration; it does not accept the failures:
conversation movement freezes, distant rechat, camera-dependent hearing, missing
follower attack, late input subtitles, rejected actor batches and stale combat barks.
Earlier accepted quest/relationship/save evidence remains accepted. VR is deferred
for the current user test lane.

Candidate fixes require a focused flat return check before acceptance. Recheck a
walking native follower, a nearby speaker behind the player, separated settlement
NPCs, a death/equipment event batch, an explicit hostile-target attack, early input
subtitles and a short combat ending before the generated bark arrives. Ordinary
NPC automatic approach remains separate from preserving existing native movement.

## Record with every run

- Client commit, package SHA-256 and lane (`SYNTH-FO4.zip` or `SYNTH-FO4VR.zip`).
- Synthserver commit, endpoint, and the LLM/TTS/STT connectors used. Never record API keys.
- The game executable version, the F4SE/F4SEVR build, the Address Library build, the mod
  manager profile and the load order.
- VR only: the headset, its runtime and the controller profile.
- `SYNTH.log` excerpts and the matching server request logs for each failure.

## Prerequisites

- [ ] **Optional radio prototype, flat only:** separately authorize installation
      into a disposable profile and complete [RADIO-PROTOTYPE.md](RADIO-PROTOTYPE.md).
      Prove static native station playback, already played slot refresh with changed
      duration, tuning/menu/world-receiver behavior, actual sound cancellation and
      save-generation ownership. Generated DCR insertion remains unimplemented.
      The rebuilt testing candidate preserves the October 7 conversation
      diagnostics, speech admission and voice sample fixes; confirm ordinary
      conversation still works alongside the separate diagnostic station.
      Quest startup, station discovery and static A/B playback passed after
      correcting SCEN placement, alias startup and the frequency collision.
      Next establish A/B/A reuse of the same filename with changed content and
      duration; cache refresh and the wider radio matrix remain open.

- [ ] Fresh Synthserver install reaches `health.php` on `127.0.0.1:8087`. Quickstart saves
      LLM/TTS/STT settings. See the Synthserver `docs/ALPHA-ACCEPTANCE.md`.
- [ ] The plugin loads only on the declared runtime and logs a clear refusal on other builds.
- [ ] **VR package:** the baseline `SYNTHVR.dll` imported `spdlog.dll` and `fmt.dll`. The VR
      build now uses the `x64-windows-static-md` triplet; a local rebuild has no such imports
      and its package passed the release audit (see `HANDOFF.md`). In-headset load is open.
- [ ] **Hosted source CI (OPEN):** policy chosen 2026-10-06: hosted CI builds and tests
      source only and does not package. The revised `windows-native` (flat and VR DLL builds
      with native suites) and `contracts` (portable C++ suites, protocol fixtures, build
      gates) workflows have not run yet. Previous head `5342870`: both DLLs built and passed
      their native suites, and VR packaging passed. Flat packaging and the contracts
      packaging tests failed only because `out/wait-papyrus/SYNTHWait.pex` is not
      provisioned on hosted runners. Server CI passed at `d171ec7`.
- [ ] **Local release packaging (mandatory):** on a host with the compiled
      `out/wait-papyrus/`, `tests/packaging/test_release_audit.py` passes, both packages are
      built from the exact DLLs under test, and both archives pass
      `tools/audit_release_tree.py`, including the root GPLv3 `LICENSE`. Record each package
      SHA-256 with the run.
- [ ] `SYNTH.ini` defaults apply. `SYNTH_custom.ini` overrides persist. No secrets appear in either file.

## Flat (Fallout 4 1.11.240, F4SE 0.7.9)

Current command and conversation return checks:

- [ ] Typed and spoken `wait here` emit a real Wait Here action and the exact NPC
      visibly stays put. A later player question must not release the hold.
- [ ] Spoken/typed Release Wait, Stop, 90 seconds of unpaused gameplay and save/load
      each release only SYNTH's owned hold. A verbal promise is not execution proof.
- [ ] Wait action admission remains pending until exact owned aliases, active SYNTH
      package and stable stopped position are verified; old queued-success cannot establish waiting.
- [ ] A walking follower can enter Wait after delayed script installation. The native
      anchor is captured once from the observed owned package; later displacement,
      Stop or reload cannot produce a false success or a late owned hold.
- [ ] Release success proves empty owned aliases and fresh absence of SYNTH Wait
      packages. Foreign restraint remains untouched and can still prevent normal movement.
- [ ] A long menu pause preserves the remaining 90-second hold, and Stop/load cleanup
      completes without a connected backend. A stale VM stack cannot reuse an alias.
- [ ] After reload, prior verified waiting can be recalled as history while fresh
      current state reports the owned hold released; the physical order is not replayed.
- [ ] Open microphone completes STT and response binding without an immutable-owner
      mismatch; short valid speech remains accepted and provider errors remain visible.
- [ ] With two eligible nearby NPCs, an idle NPC exchange starts and continues in
      order; player speech, typing, menus and Stop retain priority.

Conversation and media:

- [ ] First dialogue: typed chat to an aimed NPC returns a reply with audio and a subtitle.
- [ ] Nearby targeting: chat and voice reach an NPC beside or behind the player, an aimed
      NPC overrides nearby ones, crowd ties resolve sensibly, and range and mode limits hold.
- [ ] STT: push-to-talk and open microphone (mute/unmute) transcribe and reply.
- [ ] TTS: 3D playback, volume and distance falloff. Consecutive and rechat lines play in order.
- [ ] NPC speech ownership: a vanilla line cuts off before the same NPC's SYNTH
      clip, and a new bark during that clip is stopped on the next pump. Repeat
      with Codsworth; another NPC stays audible. Stop, natural completion,
      interruption and save/load leave subsequent normal dialogue available.
      See [NPC-SPEECH-OWNERSHIP.md](NPC-SPEECH-OWNERSHIP.md). Audible exclusion and
      recovery remain open; native stop receipts alone do not establish them.
- [ ] Lipsync: the log shows `SYNTH lipsync: native-applied`, the animation is visible and
      the mouth resets after the line. Repeat on actor unload and on reload of the same save.
- [ ] Subtitles: chunks stay in sync at the native position, vanilla dialogue takes priority,
      and no stale caption remains after menu, halt, reload or main menu.

Control and lifecycle:

- [x] Optional flat radio diagnostic quest: `EnableProbe` starts the quest,
      fills the exact owned transmitter alias, loads owned scene records and
      receives native enable success on the same test save.
- [x] Optional flat radio station: Pip-Boy shows SYNTH Radio Diagnostics;
      manual tuning to 106.9 and audible A/B previews that finish normally are
      confirmed on the tested save. Live rewrite/cache refresh and the wider
      radio matrix remain open.
- [ ] Actions: each enabled capability runs once, with ownership and truthful results.
      Pickup stays disabled.
- [ ] Halt (`StopTalking`) and interrupt stop speech and pending work without a late reply.
- [x] Saved history after Stop: publish an addressed public NPC-pair result,
      Stop, make a later manual save, reload it and verify both directed readers
      retain the same accepted result. The observed flat failure reused an older
      loaded parent during the same-world restart; the prepared correction binds
      replacements to the latest acknowledged context. Then load an earlier save
      and confirm the later publication remains excluded from that branch.
      Passed on October 24 in flat gameplay: both exact affinity-3 revisions
      remained readable after the new save/reload, and the older save's ancestry
      excluded both. The original newer-save records remained unchanged.
- [ ] Menu pause/resume: chat and native menus pause and resume dialogue without unpausing
      other menus.
- [ ] Combat: combat barks follow the INI settings, and `CancelDialogueOnCombat` cancels
      dialogue.
- [ ] Current NPC combat targets: identify the actual response NPC's human or
      creature opponent, including a prepared responder different from the scene
      focus/player. Target changes/unavailable observations remain truthful and
      do not expand audience or history readership. See [COMBAT-TARGETS.md](COMBAT-TARGETS.md).
- [ ] Generation invalidation: load save, new game, return to main menu and quit each drop
      stale replies, audio and actions.
- [ ] F4SE Menu Framework pages (Hotkeys, Auto Activate, Behavior, Sound, Tools) save to
      `SYNTH_custom.ini`. Without the framework, the INI alone works.

Wait (flat only, `SYNTH.esp` enabled):

- [ ] Wait Here holds Preston while he is working. The log shows `movement restraint verified`.
- [ ] The hold survives a bump or a vanilla comment. An AI reply works while the NPC is held.
- [ ] Release Wait releases only restraint that SYNTH applied. The 90-second unpaused
      expiry releases the NPC.
- [ ] Save and load during a hold. Old saves from earlier SYNTH builds still load.

Stability:

- [ ] Sustained session (60 minutes or more) with repeated conversations, cell changes and
      saves/loads. No freeze, crash, growing queue or log loop.

## VR (Fallout 4 VR 1.2.72, F4SEVR 0.6.21, VR Address Library v1.13.1)

- [ ] The plugin loads and fails closed on other builds. No flat-only path runs.
- [ ] First dialogue, nearby targeting, STT (push-to-talk and open microphone) and TTS,
      tested with the HMD position and controller input.
- [ ] Presentation: replies are readable in the headset, and VR subtitle/notification
      behavior is acceptable.
- [ ] Lipsync, if enabled in VR, is visible and resets after the line.
- [ ] Actions advertised by the VR capability manifest only. Halt, interrupt, menu pause,
      combat cancel and generation invalidation as in the flat lane.
- [ ] Same-world Halt restart retains the latest acknowledged save ancestry;
      an earlier disk save retains its own isolated history after load.
- [ ] `SYNTH_custom.ini` configuration and hotkeys/controller bindings work without Menu
      Framework.
- [ ] Sustained in-headset session with no freeze, crash or growing queue.

Wait/restraint is not implemented for VR.

## Conversation movement and hearing

The long-play session reproduced follower speech restraint and distant NPC rechat.
The candidate preserves native AI movement during speech and checks mutual NPC
proximity for every continuation. This requires new flat gameplay acceptance:

- [x] User observed followers keep moving while talking after the first combined update; this evidence does not accept ordinary-NPC approach.
- [ ] Repeat follower walking conversation after the continuous Follow/errand update without a SYNTH wait restraint.
- [ ] Ordinary eligible NPCs hear typed and voice input from behind the player within the configured radius.
- [ ] A Handy/Protectron/Assaultron/Eyebot/Sentry Bot can open typed chat and hear voice behind the player without a crosshair pick; an explicit eligible pick still wins.
- [ ] Rechat stops when the next NPC is more than 784 units from the previous speaker.
- [ ] Explicit Wait Here retains ownership; speech, Stop and save/load do not release somebody else's hold.
- [x] Flat: Sturges physically approached the player during conversation; the user confirmed it and the log verifies the active owned package (4 November 2026).
- [ ] An ordinary player-engaged NPC can continue walking with the player during conversation.
- [ ] NPC rechat and combat barks never pull bystanders into player approach.
- [ ] Approach stops/restores native AI on completion, Stop, combat, death, load and timeout; existing Wait ownership remains isolated.
- [ ] Exterior walking crosses loaded cells in the same worldspace; entering a different interior releases the approach.
- [ ] An unreachable NPC times out without teleporting, recruiting, or changing factions.
- [x] Actual owned package activation is observed during Sturges approach; the current package matches the expected SYNTH approach package.

Sturges now physically approaches the player during conversation, with the expected
owned navigation package confirmed in the runtime log. The revised repeating
Follow procedure resolves the tested approach failure. Walking together, cleanup,
cell transitions and unreachable destinations remain separate unchecked cases above.

Saved follower errands additionally require:

- [ ] An explicit named recipient/topic is saved under the correct NPC and playthrough; ambiguous recipients are rejected.
- [ ] A ready follower navigates to the exact loaded NPC; dialogue starts only after fresh owned-package arrival and mutual hearing checks.
- [ ] A deferred return waits for departure and the exact saved location identity, then revalidates loaded actors before visiting.
- [ ] Save/load and branching cannot replay a finished errand or let an old execution release a newer visit.
- [ ] Stop, combat, unreachable target, scene change and a player action on the same performer cleanly release only the visit aliases and restore native follow AI.

- [ ] NPC-to-NPC reply and fresh saved relationship publication pass after the exact paired update is installed.
