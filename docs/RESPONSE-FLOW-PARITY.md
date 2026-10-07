# Response flow and interruption

## Local implementation: 2026-09-08

- WinHTTP reads currently available bytes before decoding incremental NDJSON. A large read alone can wait until the response finishes.
- Owned dialogue lines are admitted after envelope, identity, sequence and duplicate checks. Actions and control callbacks wait for valid terminal completion.
- Failed or truncated streams cancel their exact turn owner and request server cancellation on the independent control lane. They cannot cancel a replacement turn.
- Audio status and verified media downloads do not acquire the serial dialogue/context request lock. Downloads retain integrity, deadline and cancellation checks.
- The existing single speech-delivery worker preserves player/NPC and sentence ordering; server synthesis may run concurrently.
- Native player TTS uses the existing durable owned jobs for XTTS, Chatterbox, PocketTTS, Cartesia and Inworld. Legacy and nonportable extension paths retain their behavior.
- New text, successful push-to-talk capture, confirmed open-mic speech and chatbox Stop invalidate the old dialogue chain. Stop does not disconnect the session.
- Opening the chatbox pauses audio and subtitle progression. Esc/Cancel resumes without sending. Submission stays paused until game-thread admission, avoiding a brief replay of old audio.
- Native subtitles, curated voices, player voice configuration and save/load generation protections are preserved.

## Validation

- All 17 native test executables passed; fake-server coverage includes split lines, early admission, malformed terminal data and parallel speech reads.
- All 40 build-guard tests passed.
- Focused PHP suites passed: PlayerTtsHelpersTest, RequestStreamingTest and NpcVoiceFallbackTest (115 tests, 2,891 assertions).
- Windows x64 releasedbg DLL built successfully.
- A local real-WinHTTP delayed-response probe delivered the first chunk at 5 ms and finished at 1,006 ms. Cancellation and buffered reads also passed. This is a transport test, not an in-game latency measurement.
- The unrestricted PHP suite did not finish: a legacy pipeline fixture exits the test runner. Do not count that run as full-suite success.

## Manual game acceptance still required

1. Load the save normally; ask Preston a question with player TTS enabled. Player speech must precede Preston, with the existing voices and native subtitles.
2. While Preston speaks, open the chatbox: playback pauses. Press Esc: the same utterance resumes.
3. Open the chatbox and send a new message: old speech and queued sentences must not return.
4. Press Stop: silence the current chain without losing the connection; a later message must work.
5. Test push-to-talk and open mic separately. Speech detection cancels the previous chain, including when the recording later produces no usable text.
6. Load another save while speech is pending. No old audio, subtitle, action or continuation may appear afterward.

No game launch, cloud-provider benchmark, release or PR is part of the validation above.

## Verified local deployment (historical)

_Historical receipt, retained as dated evidence. Workstation paths are replaced with
placeholders; see [HANDOFF.md](HANDOFF.md#historical-receipts-and-placeholders)._

- Client repository: `<client-worktree>`, branch `codex/response-flow-parity`, base commit `01f225ac46b52fa136f9df1d3465e15838c510f2` plus local changes.
- Server repository: `<server-worktree>`, branch `codex/response-flow-parity`, base commit `a1e657408239fe4fd763565e68df6243cd9ad818` plus local changes.
- Client destination: `<MO2-mods>/SYNTH_dev`.
- Server destination: `<wsl-distro>:/var/www/html/Synthserver`; HTTP health 200; existing 68-migration schema verified.
- Deployed DLL SHA-256: `950FA850CD159D7EC1008C38F5A2A2AF596E184A1D7AFA32DA48B87C60F15369`.
- Deployed PEX SHA-256: `56D6D12F82FFC41F1CFA91A2AA465B67466022F7C550B0B4CEBA3E56D11FEF9E`.
- Custom INI hash remained `51972C4A66F1EF89B067C6C26747CFB5CFC425E6C667F52180296F433CDF82F3`; server config and complete voice-file digest also remained unchanged.
- Both guarded deployment stages reported success. Packaging retained strict Papyrus provenance after restoring the original LF script bytes in the new worktree.
- Fallout 4 remained closed. No PR, push, release or version bump was performed.
