# Native dialogue subtitles (flat Fallout 4)

The previous `ShowAISubtitles` path called `push_notification`, so AI replies appeared
as HUD notifications. Flat dialogue now feeds the existing `HUDSubtitleText` component.
Status/error notifications remain unchanged. No SWF, ESP, dialogue scene, or hook is added.

## Exact-build evidence

Inspected on disk; this is not in-game acceptance:

- Fallout4.exe 1.11.240.0 SHA256:
  `FDCEF37AC1230AF6D0B0050EB2142B139EF3A867B37B9211FB6EDFCC646072F8`
- version-1-11-240-0.bin SHA256:
  `65985CC2259384A13CFFB538D74776E422E62B0CD3766485A000620242B72B06`
- Address Library ID 450198 identifies HUDSubtitleText's primary vtable, RVA 2533A80.
- Constructor RVA A2FD5B installs that vtable. RVA A2FE08 selects the embedded
  `BSTValueEventSink<HUDSubtitleDisplayEvent>` at component +258; A2FE13 installs
  vtable ID 105062 (RVA 2533A08). A2FE58 registers this same sink with the native event source.
- Update RVA A301F0 takes the spin lock at component +280, copies optional data
  at +260 and eventReceived at +278, and resets eventReceived at A302B1. It uses
  the received value to update visibility (A302F5), speaker text (A3033E) and
  subtitle text (A30356). These match the pinned CommonLibF4 sink's 0x30 layout.

`flat_dialogue_subtitles.hpp` checks runtime version and both object vtables,
reacquires the HUD through its menu-map lock, bounds the component scan, and tries
the sink lock once. It changes only optionalValue/eventReceived. It does not call
arbitrary event sinks or mutate SubtitleManager's priority array/current speaker.
No game-object pointer survives the frame. Native/other-mod subtitle values take
priority; clearing compares the currently displayed speaker/text with SYNTH's owned
caption, so a replacement native line is not cleared.

Spoken captions are carried by the actual playback clip, not a response-arrival timer.
Text-only replies use a bounded queue and reading timer. Menu hiding, turn cancellation,
generation changes, and session halt remove stale captions. VR retains its existing
notification presentation: none of this flat-only layout is shared with its ABI.

## Manual acceptance still required

Without agent game control, verify: several spoken chunks display in sync at the
native subtitle position; text-only replies remain readable; vanilla dialogue wins;
menu open/close, hard halt, save reload, and returning to the main menu leave no stale
caption. Custom HUD replacements and VR require their own acceptance/implementation.

## Local readiness, 2026-09-08 (historical)

_Historical receipt, retained as dated evidence. Workstation paths are replaced with
placeholders; see [HANDOFF.md](HANDOFF.md#historical-receipts-and-placeholders)._

- Source: RANGROO/SYNTH, `codex/dialectic-parity-core`, uncommitted subtitle changes
  on base `01f225ac46b52fa136f9df1d3465e15838c510f2`.
- Flat and VR DLL builds passed; all 17 suites passed in each lane. Build gates
  (40), packaging tests (11), protocol verification (32 schemas / 188 fixtures /
  200 canonical files), and both release archive audits passed.
- Claude Opus 5 performed the bounded read-only UI review. Its VR regression
  concern was handled by preserving the original VR notification path; no flat
  offsets were introduced into VR. The empty-speaker fallback remains `SYNTH`.
- Deployed through `synth-full-deploy` with `-SkipServer -SkipBuild` to
  `<MO2-mods>/SYNTH_dev/F4SE/Plugins/SYNTH.dll`.
- Installed DLL SHA256:
  `12CCBF598AC94F825075D7FBCD92F60320DEDCB6D2CF30B04ED2F38C7DEB1D54`.
- Previous five-file mod backed up and hash-verified at
  `<private-backup>/pre-native-subtitles-SYNTH_dev`.
  Only the DLL changed; both INIs and Papyrus source/binary hashes were preserved.
- Server stage/HTTP health check skipped; server, credentials and voice settings
  were not changed. No game/editor was started, stopped or controlled. No PR,
  push, version bump or release. In-game acceptance above remains outstanding.
