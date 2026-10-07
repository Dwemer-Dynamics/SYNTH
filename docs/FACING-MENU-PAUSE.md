# Response facing and owned menu pause

Scope: SYNTH and Synthserver, both `codex/response-flow-parity`, local deployment only.
Client base `01f225ac46b52fa136f9df1d3465e15838c510f2`; server base
`a1e657408239fe4fd763565e68df6243cd9ad818`. Existing task changes are preserved.

## Changes

- An invisible original `SYNTHPause` native IMenu uses `kPausesGame`. The engine owns
  its pause count; SYNTH never writes global time, resets a pause counter or closes another menu.
- Chat and ControlMenu cached open flags drive show/force-hide messages on the game thread,
  before early returns. Lifecycle invalidation and session retirement release the pause too.
  No new engine calls run in the framework renderer. Native pause is flat 1.11.240 only.
- Send remains queued while native menu removal is pending. Once the engine exits menu mode,
  normal generation/target validation admits the submitted message. Control actions already
  defer until both the overlay and native menu close. Gameplay resumes on cancel/close.
- Owned chat/control menus always pause SYNTH playback as well as world simulation. The
  existing optional pause setting still governs unrelated game menus.
- Response-owned canonical speaker/listener identities now accompany text-only captions and
  audio-failure fallbacks, not just successfully queued audio. Text-only line IDs provide
  one-shot ownership when no speech utterance exists. Disabling visible subtitles does not
  disable facing. Player/narrator speakers are never rotated; posture/movement/scene guards remain.
- Recent live route records showed Preston replies with `listener_state=unavailable`. Server
  routing now uses the captured player as fallback only for a direct input_text/input_audio
  reply from the captured target NPC. Explicit canonical listeners retain priority. Ambiguous
  labels, invalid explicit references, player/narrator speakers and rechat never use this fallback.

## Reference and review

Dialectic's SpeakManager resolves an explicit listener/rechat target and falls back to the player
for non-rechat speech. It performs one-shot facing and avoids rotating moving/seated/busy actors.
CHIM's PrismaUIBridge requests game pause on chat focus and releases focus when closing.
These are behavior references, not transferable Fallout 4 offsets.

Claude's read-only UI review confirmed the existing cached open flags, render-thread ownership,
and requirement to release before early returns. Its optional new setting was not added.
The implementation uses a native menu instead of directly manipulating global pause state.

## Validation

- Flat x64 build and all 17 native suites pass, including canonical text-only facing binding.
- 43 build/source checks, 12 packaging checks and 191 protocol fixtures pass.
- PHP lint and listener fallback cases pass. Full RequestStreamingTest passes in WSL:
  96 tests, 2,820 assertions. Windows execution hit subprocess-quoting errors; WSL is the
  successful broader test result, not a claim that the Windows suite passed.
- Pinned Fallout 4 Address Library resolves UI::RegisterMenu to RVA 1A826D0 and
  UIMessageQueue::AddMessage to RVA 1A89630 in the installed 1.11.240 executable.
- No game launch or in-game/VR verification. Manual acceptance: open/cancel both menus;
  Send from paused chat; close overlapping native menus without unpausing them; hard halt;
  direct Preston reply; explicitly addressed NPC reply; text-only/audio failure; save/load.

## Local deployment receipt (historical)

_Historical receipt, retained as dated evidence. Workstation paths are replaced with
placeholders; see [HANDOFF.md](HANDOFF.md#historical-receipts-and-placeholders)._

- Client: `<MO2-mods>/SYNTH_dev`.
- DLL SHA-256: `E6533ECB16462F803A96F5DBA059EE242B0FE3B67EE0D8BE9A2137B563F4C01A`;
  built and installed hashes match.
- Custom INI unchanged: `33615F103948696C045CA9C3894DAC8D6D64ADC7AFBB636C37A37A3DCEA83E5E`.
- Server: `<wsl-distro>` `/var/www/html/Synthserver`, HTTP health 200.
- `lib/synth_rechat_routes.php` source and deployed SHA-256 both
  `BECC72E6D7CE4C94B40B9022DB126415BBE912207DA21A6CFB6FE311F9DB601C`; deployed PHP lint passes.
- Deployment helper reported both client and server stages successful, including extracted
  package audit and original native PEX hash verification. No game launch, commit, PR or release.
- Prior DLL backup: `<private-backup>/before-facing-menu-pause-20260909`.
