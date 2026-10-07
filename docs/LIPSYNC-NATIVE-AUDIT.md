# Speech animation: flat Fallout 4 1.11.240

## September 14 runtime correction

The 10:36-10:40 session proves the first hook did not animate either the player or
Preston: zero native-applied frames and 55/76/229/157 worker-thread callbacks.
The old main-thread-only hook branch below is superseded. The game-thread pump now
writes the 13 speech channels while holding the existing verified TrySpinLock.
The pinned library's BSSpinLock::try_lock targets an RW lock and is not used for
facial application or cleanup. The original engine lip function still runs on its
own thread. Our callback there only checks a copied identity/cancellation/expiry
permit and returns the merge decision; it never reads or writes a native object.
The native no-lip branch (0x6D2307 -> 0x6D2943) returns -1 without clearing speech
data, and 0x6D0BD0 compares that speech against final values. A current permit makes
the engine keep merging the game-thread speech values even on unchanged frames.
Cancellation/release/load revoke permission; load still performs no native writes.
Logs distinguish game-thread writes from actual engine merge callbacks.
Worker authorization, wrong identity, revocation, cancellation and 250 ms expiry
have engine-free regression checks. Visible game animation remains manual proof.

## Evidence and implementation boundary

The previous implementation wrote only finalExpression[2] once every 500 ms.
CHIM uses a text-derived viseme sequence, WAV silence gating, blending and a
microsecond animation interval. SYNTH now uses that behavioral approach with its
existing XAudio2 sample clock, not Skyrim FaceGen indices or engine addresses.
The algorithm remains approximate text-based animation, not forced alignment.

Inspected executable: Fallout4.exe 1.11.240.0, SHA-256
`fdcef37ac1230af6d0b0050eb2142b139ef3a867b37b9211fb6edfcc646072f8`.
Read-only PE/disassembly inspection established:

| RVA | Evidence |
| --- | --- |
| 0x2506020 | BSFaceGenAnimationData vtable, current Address Library ID 417061 |
| 0x6D1083 / 0x6D123A | Constructor/destructor references to that vtable |
| 0x6D1630 | Update obtains the lock at object+0x2B4 |
| 0x6D177B | `E8 00 F1 FF FF`, call to UpdateMorphsFromLip at 0x6D0880 |
| 0x6D1783 | Native lip-update return value selects speech+emotion versus emotion-only merge |
| 0x6D17A8 / 0x6D17B1 / 0x6D17C2 | Reads emotion at +0x1C8, max with speech at +0xF0, writes final at +0x18 |
| 0x6D1B73 | Merge covers 0x36 (54) expression channels |
| 0x6D1C9E..0x6D1CBB | Native update marks the combined expression dirty |
| 0x6D08A1 / 0x6D098B | Native lip resource at +0x2C0 and playing/ending state at +0x2E0 |

The adapter checks exact version, call bytes, original entry bytes, merge bytes
and live vtable. It hooks only that call site. Original lip processing always
runs. SYNTH then supplies speech channels for the current audible human actor
and returns true to use the engine's existing merge and dirty propagation.
No engine function is called speculatively using an old Address Library ID.
No global morph/blink patch or native lip resource is installed.

Independent reference material:

- [CHIM SpeakManager at ba5e5c9](https://github.com/Dwemer-Dynamics/CHIM/blob/ba5e5c96645fa8668a0debc894bb0cb85b7bad32/Plugin/SpeakManager.cpp): behavioral reference.
- [CommonLibF4 FaceGen definition](https://github.com/frakkin64/commonlibf4-old/blob/362fa9cf14936552899775f61cef006ad7cf5ca1/include/RE/B/BSFaceGenAnimationData.h): field-layout cross-check; dependency pin unchanged.
- [Screen Archer human morph catalog](https://github.com/maximusmaxy/ScreenArcherMenu/blob/master/Data/F4SE/Plugins/SAM/Menus/Human%20Morphs.txt): semantic names for human-race mouth channels. No external mod is required or bundled.

## Ownership and compatibility

- Actor lookup and native writes are restricted to the runtime adapter/game thread.
  The native callback uses only its engine-provided object while Update owns its
  lock. Off-thread callbacks run the original only and increment a bounded diagnostic.
- Only HumanRace (Fallout4.esm 00013746) is enabled pending other-race mapping proof.
  Human NPCs and third-person player TTS use the same path. Narrators, text-only
  replies, absent 3D and unsupported races do not acquire a face.
- Generation, cancellation, exact utterance, reference handle, exact FaceGen
  object and a 250 ms publication expiry fence the animation. Prefetched audio
  never produces a playback frame before delivery admission.
- Native lip playback wins during overlap. Ordinary completion/pause restores
  only still-owned mouth channels; cleanup uses try-lock and retries instead of
  blocking the game thread. Load invalidation forgets ownership without resolving
  an actor or writing into old/new loaded state.
- VR no longer uses the assumed flat layout. Its build retains audio, captions
  and shared timeline code, but logs facial animation unavailable until the VR
  engine path is independently verified. This is not VR lip-sync parity.
- AnimationResolution retains its key, value and 50..1500 range but now denotes
  microseconds, matching CHIM. The game tick bounds execution; no busy loop or
  timer is introduced. Existing AnimationIntensity remains the strength control.

## Performance and manual acceptance

At most 8192 one-byte text shape entries are allocated per admitted voiced line
on the existing speech worker. Sampling is constant-time. WAV envelope reads are
bounded (about 64 sample positions, maximum eight channels); no extra network
requests, provider features, audio scans or model calls are introduced.
Native application touches 13 mouth channels on one actor per engine update.
Game-thread cost and perceived first-audio latency still require like-for-like
in-game measurement; build/test results are not performance or visual proof.

Manual tests: Preston's Whiterun line; closed lips versus rounded/wide vowels;
silence; interruption; chat/menu pause/resume; consecutive/rechat speech; player
TTS in third person; vanilla-comment overlap; actor unload and same-save reload.
Require `SYNTH lipsync: native-applied` plus visible animation and mouth cleanup.
`armed` alone is not proof. Off-thread or signature-unavailable logs must be
investigated, not bypassed. Do not launch the game automatically.
