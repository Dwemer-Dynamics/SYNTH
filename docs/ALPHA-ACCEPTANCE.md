# SYNTH open-alpha acceptance

## Flat gameplay evidence — 24 October 2026

- Codsworth and Jun completed a public NPC exchange; both movement holds released.
- Both directional affinity-3 updates were readable before saving and after
  reloading the new manual save. Loading an older save excluded the later updates;
  the newer records remained intact. Jun was absent from that older scene, so his
  exclusion was checked through saved ancestry rather than a live Jun reader.
- Earlier quest-plan testing covered teaching Codsworth, a follow-up without the
  quest title, revised recall after save/load, an uninformed Jun and an older save.
  Formal whisper, cloned actors and overhearing still need separate coverage.
- The normal launcher opened after the startup repair and Codsworth replied.
  Server records confirmed the ChatGPT route and `gpt-6.1-sol` for that reply.
  Other configured models and the existing server profile fallback remain available.

This is flat alpha evidence. VR compiles but has no headset acceptance from this
session. Broader scene/action transitions and sustained sessions remain open.
Earlier unchecked scenarios below remain separate unless explicitly covered here.


Status: **OPEN**. The optional flat radio startup and static A/B playback checks
have passed on the isolated test save. The broader baseline (client `f34da88`, server
`bcb0c06`) remains unaccepted. Builds, unit tests, protocol fixtures and package audits are prerequisites. They do
not count as in-game evidence. Record flat and VR results separately. A flat pass never implies a
VR pass.

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

## Temporary conversation movement

- [ ] Flat addressed participants remain still while a reply or NPC exchange is pending; bystanders continue normal AI.
- [ ] Temporary holds release at completion, cancellation, Stop, combat and before an action; explicit waits and save/load remain correct.
- [ ] NPC-to-NPC reply and fresh saved relationship publication pass after the exact paired update is installed.
