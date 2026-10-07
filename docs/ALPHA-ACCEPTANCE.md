# SYNTH open-alpha acceptance

Status: **OPEN**. No item below has passed for the current baseline (client `f34da88`, server
`bcb0c06`). Builds, unit tests, protocol fixtures and package audits are prerequisites. They do
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

Conversation and media:

- [ ] First dialogue: typed chat to an aimed NPC returns a reply with audio and a subtitle.
- [ ] Nearby targeting: chat and voice reach an NPC beside or behind the player, an aimed
      NPC overrides nearby ones, crowd ties resolve sensibly, and range and mode limits hold.
- [ ] STT: push-to-talk and open microphone (mute/unmute) transcribe and reply.
- [ ] TTS: 3D playback, volume and distance falloff. Consecutive and rechat lines play in order.
- [ ] Lipsync: the log shows `SYNTH lipsync: native-applied`, the animation is visible and
      the mouth resets after the line. Repeat on actor unload and on reload of the same save.
- [ ] Subtitles: chunks stay in sync at the native position, vanilla dialogue takes priority,
      and no stale caption remains after menu, halt, reload or main menu.

Control and lifecycle:

- [ ] Actions: each enabled capability runs once, with ownership and truthful results.
      Pickup stays disabled.
- [ ] Halt (`StopTalking`) and interrupt stop speech and pending work without a late reply.
- [ ] Menu pause/resume: chat and native menus pause and resume dialogue without unpausing
      other menus.
- [ ] Combat: combat barks follow the INI settings, and `CancelDialogueOnCombat` cancels
      dialogue.
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
- [ ] `SYNTH_custom.ini` configuration and hotkeys/controller bindings work without Menu
      Framework.
- [ ] Sustained in-headset session with no freeze, crash or growing queue.

Wait/restraint is not implemented for VR.
