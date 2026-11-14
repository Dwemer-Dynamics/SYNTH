# Vanilla dialogue witnesses (flat candidate)

Flat-only, negotiated `context.observed_dialogue`: when the game pump first observes a
vanilla NPC line that Fallout selected for subtitle display, the client copies the speaker, INFO
and text and grants canonical witnesses once, inside the published scene. Policy
`same_cell_speaker_radius_1000_v1`, basis `first_observed`. The context carries the
first-observed frame, game clock ticks and capture age. It never claims a voice start,
audibility, line of sight or that anyone listened. VR does not offer the capability.

Source: `src/adapters/flat_vanilla_dialogue.hpp`, `src/core/event_witness.hpp`,
`src/client/dialogue_observation_delivery.hpp`; schema
`protocol/v2/schemas/witness-observation.schema.json`.

## Semantics

- Sampling, not callbacks: one nonblocking manager read lease and one source spin-lock attempt
  per pump. Contention, an oversized array (more than 32 entries), text over 512 bytes, an
  ambiguous match or a stale source value is `unavailable` and changes nothing; it never
  manufactures an absence. What the source shows also depends on the game's subtitle selection
  and settings. That dependence has not been checked in game.
- A line qualifies only when exactly one priority entry matches `currentSpeaker` and the
  displayed source text, that entry is INFO-backed, and the audio manager reports the speaker's
  dialogue sound as playing. That is engine playback state, not proof that anyone heard it.
  Text is length-bounded before it is copied.
- Residency is keyed by speaker form/plugin/playthrough, INFO identity and exact text. Display
  name changes never start a new line. An observed absence, or a positively read player or
  non-actor line, ends the residency. Identical lines repeated between two samples merge. A line
  already showing when the tracker arms (session or generation change) is a baseline and is never
  delivered.
- Delivered only if the speaker is a live, loaded actor in the player's cell. The INFO must not
  come from `SYNTH.esp`, the speaker must not be a SYNTH-owned speaker and must not be a `0xFF`
  form. Witnesses are high-process live NPCs in the speaker's exact cell, at most 1000 units from
  the speaker. Up to 64 handles are discovered. More than 8 witnesses, or any unresolved scene,
  grants nothing. Later arrivals never join. Unknown evidence grants nothing.
- The witness cell is copied at collection and must equal the following snapshot's scene, interior
  flag, generation and playthrough. Otherwise nothing is granted. At most 4 captures are pending;
  overflow drops and is counted. A retry replays the original capture and scene.

## Flat 1.11.240 audit provenance

The audited executable has SHA-256 `FDCEF37AC1230AF6D0B0050EB2142B139EF3A867B37B9211FB6EDFCC646072F8`.
The audited Address Library has SHA-256 `65985CC2259384A13CFFB538D74776E422E62B0CD3766485A000620242B72B06`.
The plugin fails closed unless all of the following match at load:

| Item | Check |
| --- | --- |
| `SubtitleManager` singleton / RW lock | RVA `30E8B08` / `3313598` via the pinned SDK IDs |
| Selection entry | RVA `107E520`, 32 prologue bytes |
| Source clear entry | RVA `107EF20`, 32 prologue bytes |
| `BSSoundHandle::IsPlaying` | ID `2267046`, RVA `162DE60`, 32 entry bytes; called on a local copy with `assumeSuccess` cleared |
| Dialogue handle getter | RVA `D02750`, 24 bytes (existing speech-exclusion audit) |

The layouts, the selection and clear behavior, and the IsPlaying manager branch (tracked-sound
flag `+24`) come from on-disk analysis. The runtime byte checks cover only the listed prologues.
None of this has been exercised in game.

## Status

All 19 native suites pass (the core native suite alone has 207 assertions), and the existing 65
build guards pass unchanged. The protocol check (45 schemas, 403 cases, 395 mirrored files)
passes. The flat and VR x64 DLLs build with the pinned SDK and existing installed ports, with no
package installs or fetches. Capture stays unavailable in VR.

Packaging: all 22 packaging regressions pass. Both lane archives were staged and audited locally
and never deployed or distributed. The archives reuse the existing compiled wait script and its
build manifest unchanged, checked against the current source and flags by the existing bundle
verifier. No wait script was compiled for this change and no compiler path was qualified.

The server's opt-in loopback check compiles this client's driver against the paired server, a
private disposable database and a fake provider (3 tests, 778 assertions). It publishes synthetic
game state, not engine state. The observed-line marker reaches the granted witness's prompts once
in an ordinary turn and once in `/talk`. It reaches a non-witness, a different same-name actor,
and the witness after loading a save from before the observation zero times. A retry keeps one
unchanged history row. The server's full existing suite passes with one skip, and the skipped
client-wiring test passed separately against this client commit.

A read-only source review found no wrong-grant defect and accepted three conservative limits that
can only miss lines: the audio-playing check can lag the selected subtitle; the 5-second delivery
cadence, busy gate and 4-capture queue can drop lines in fast scenes; and strict plugin-name and
text eligibility can reject unusual input. Audio start and queue pressure are in-game acceptance
checks.

These are prompt checks, not model answers or game acceptance. The limits above still apply:
first-observed basis, sampling, dependence on subtitle settings and residency keying, and the
fixed 1000-unit radius. No real provider has been called. There is no in-game evidence, VR capture
is unavailable although the VR DLL builds, native gameplay and performance are unmeasured, and
there is no schema or database migration. Nothing is deployed.
