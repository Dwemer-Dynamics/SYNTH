# Local voice cloning

SYNTH automatically queues a sample import for the selected NPC's copied voice type when
publishing dialogue context. It does not wait for the import before sending dialogue.
The first reply can therefore use the configured fallback if its sample is still importing.

The flat client's existing **SYNTH > Tools** page also has **Send All Voice Samples**,
**Cancel**, and a progress/status line. This imports one candidate per discovered voice,
not every recorded line. Existing server samples are kept. Use Synthserver's existing
TTS Studio to listen, replace a poor candidate, or assign a manually chosen voice.

## Boundaries

- Discovery and upload use a dedicated worker and HTTP connection, independent of dialogue.
- Only copied identities and loaded-plugin names cross the game/worker boundary.
- The worker reads the game's virtual Data tree, including MO2's winning loose files.
- Supported archives: BA2 v1 GNRL, unpacked or zlib-compressed entries. Other versions are
  skipped and counted, never interpreted using guessed layouts. Texture archives are ignored.
- Supported samples: FUZ v1/XWMA, XWM, WAV, OGG; maximum 8 MiB per entry. Conversion rejects
  samples shorter than three seconds and limits normalized output to fifteen seconds.
- Candidates are selected by bounded size/length heuristics, not a reviewed transcript map.
  Voice likeness, mod-specific content, and suitability still require listening in TTS Studio.
- Cancellation/save invalidation stops new work. An already accepted server conversion may
  finish and keep its reusable sample; it cannot send a stale reply/action into the game.
- Bulk runs stop after fourteen minutes or a backend/session failure. Bad individual recordings
  are skipped and counted. Running Send All again keeps existing samples and continues missing ones.
- Session construction/retirement runs on the shared coordinator for flat and VR.
- VR shares automatic import but has no flat F4SE Menu Framework Tools page.
- No extracted game audio is included in source or release packages.

The client uses `synth.voice_sample.v2` multipart metadata at `/vsx.php`. Session generation,
request ownership, source path, decoded size, and response schema are checked. Imports never
overwrite saved NPC voice choices. The server distinguishes a saved sample from backend
readiness. Local audio.cpp PocketTTS uses `voice_ref`, not Python's upload API.

## Manual test

1. Start Fallout 4 manually through the SYNTH MO2 instance and load a save.
2. Wait for SYNTH to connect. Speak to an NPC, or run Send All Voice Samples first.
3. Verify the reply uses the intended voice and that Tools displays import progress.
4. Cancel a batch and load another save during a batch. Verify input and subsequent replies
   continue, and no stale response plays in the new session.
5. Preview/replace a sample in TTS Studio, repeat a sentence, and verify the new sample is used.

Windows builds and offline/HTTP probes are not flat gameplay or VR headset validation.
