# NPC speech ownership

SYNTH's generated audio uses XAudio2 independently of Fallout's actor voices.
An admitted NPC clip now acquires copied speech ownership on the game thread
before its audio starts. The flat adapter stops that actor's current native
dialogue sound and checks it again on every active game-thread pump. Humans,
ghouls, robots and other actor races follow the same path; lip animation and
listener-facing settings do not control voice ownership.

Ownership lasts for the clip, including menu and presentation pauses. Audio is
stopped before ownership is released on completion, interruption, cancellation,
delivery failure or session retirement. Save/load invalidation discards copied
ownership without accessing old native objects. Queued, stale and cancelled
clips cannot acquire ownership. Narrator clips have no NPC voice to suppress.
If the actor identity or native adapter cannot be verified, NPC delivery falls
back to the existing text presentation instead of starting unguarded audio.

## Native scope and limits

The flat implementation supports Fallout 4 1.11.240 with pinned CommonLibF4
`6266ecc9014b473fc6b6efd04abac324477c63cd`. It copies the exact index-0 dialogue
sound handle from the live actor's AIProcess, performs a fresh validity query,
and requests audio-manager termination of that sound. It never passes a sound
descriptor FormID as a handle, touches the unverified index-1 sound, sets an
actor speaking flag, or calls Actor::StopCurrentDialogue. Releasing ownership
requires no native restoration, so no persistent silence is stored on an actor.

Subsequent vanilla voice starts are stopped at the next owning pump. This is
frame-bounded suppression, not a proven native voice-start interception. Native
Stop confirms request acceptance, not audible completion. An in-game overlap
test is required, including a new bark during a SYNTH reply. An NPC's cut-off
vanilla line is not replayed when the SYNTH reply ends; subsequent normal lines
remain available.

The VR native sound ABI has not been independently audited. The shared clip
ownership logic builds in VR, but NPC audio falls back to text until a verified
VR adapter is provided. Narrator audio is unaffected. Flat results do not
establish VR acceptance.

## Flat executable audit

Audited Fallout4.exe SHA256:
`fdcef37ac1230af6d0b0050eb2142b139ef3a867b37b9211fb6edfcc646072f8`.
Audited 1.11.240 Address Library SHA256:
`65985cc2259384a13cffb538d74776e422e62b0cd3766485a000620242b72b06`.

| Function | Address Library ID | RVA | ABI |
|---|---:|---|---|
| Dialogue sound getter | 2231884 | D02750 | BSSoundHandle*(AIProcess*, int32 index) |
| Sound Stop | 2267045 | 162DE20 | bool(BSSoundHandle*) |
| Sound IsValid | 2267048 | 162DF00 | bool(BSSoundHandle*) |

The getter returns `high + 0x3CC + index*8`, or the engine's invalid handle.
Native actor pause/resume/fade/stop paths all use index 0. Stop writes only the
copied handle's cached state and requests Stop(soundID) from BSAudioManager;
it performs no direct actor, quest, scene or dialogue-item mutation. The adapter
checks the exact runtime, relocation addresses and instruction signatures before
using these functions. It installs no native hook.

Actor::StopCurrentDialogue at C95A00 changes dialogue timers, listeners and
conditional scene flags; FadeCurrentDialogue also changes dialogue state.
Neither is suitable for unconditional voice-only suppression. The four audited
index-0 handle setters at C7D657, C7DA46, C7E0E8 and C96B83 run after playback or
clear the handle, so intercepting those setters would not prevent an audio start.
Universal coverage and serialization of kCanSpeak remain unproven; that flag is
not mutated.

## Acceptance

Use a normal SYNTH conversation while the same NPC is speaking a vanilla line.
Verify that the vanilla audio cuts off, the SYNTH reply stays audible, and new
vanilla barks do not continue over it. Repeat with Codsworth. Check that another
nearby NPC remains audible, Stop cuts SYNTH immediately, a subsequent ordinary
line works, and a save/load during playback does not leave the actor silent.
Radio A/B baselines do not establish any of these results. Radio handoff can use
the same ownership principle, but requires its own station audio adapter.
