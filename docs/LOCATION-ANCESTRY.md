# Location ancestry

Status: implemented with paired Synthserver validation and native prompt integration. Native unit tests,
protocol fixtures and both lane builds are the only evidence. Nothing here is verified in game.

## What it adds

A flat context already sends `world.location_ref`: the player's current `BGSLocation` as a runtime
FormID and its defining plugin. That ID changes with the load order, and it says nothing about the
places that contain the location. A named interior such as a basement cannot be traced to the
settlement or district it belongs to.

With `context.location_ancestry` the same context also carries `world.location_ancestry`: the
current location and each `parentLoc` above it, nearest first, as plugin-local identities. Each
entry is `(origin_plugin, local_form_id, display_name)`, so two places called "Basement" in
different plugins are always different locations. The wire contract is in
[protocol/v2/README.md](../protocol/v2/README.md#plugin-local-location-ancestry).

## Capture

`location_ancestry()` in `src/adapters/commonlib_capture.hpp` runs on the game thread inside the
existing world capture. It reads only the declared `BGSLocation::parentLoc` member, which the pinned
flat and VR adapter headers both place at `0x50` with a `0x140` size assertion.
No address, relocation or new hook is used. Each lane compiles the walk against its own headers.

Each form is decoded by `core::plugin_local_form_id()` in `src/core/snapshot.hpp`, using the same
loaded plugin manifest the snapshot already carries (flat: the cached manifest, VR: the manifest
taken in the same capture). The prefix must equal the defining plugin's own slot: the compile index
for a full plugin (24 local bits) or `FE` plus the small-file index for a light one (12 local bits).
A plugin missing from the manifest, a mismatched prefix or a runtime-created `FF` form is not
decoded, so a load-order ID is never passed off as a local one.

The walk stops and records why:

| `end` | Meaning |
|---|---|
| `root` | The last entry has no parent. The chain is complete. |
| `unresolved` | The next parent exists but could not be decoded. |
| `cycle` | The next parent repeats an entry (by pointer or by plugin-local identity). |
| `depth_limit` | Eight entries were taken; the next parent was not read. |

Only `root` proves completeness. If the current location itself cannot be decoded, or the first
entry is not the same form as `location_ref`, no ancestry is sent at all.

## Negotiation and compatibility

- The flat client advertises `context.location_ancestry` beside `context.location_identity`.
- The server acknowledges it with `location_ancestry: true` in the init status. Without that
  acknowledgement the client strips the field before encoding, so an older server, whose world
  object is closed, never receives it. Older clients send no ancestry and are unaffected.
- The codec emits ancestry only with a negotiated `location_ref` and refuses an ancestry whose first
  entry, bounds, end or uniqueness are invalid, as it does for other identities.
- VR compiles and runs the same capture but sends nothing new: it does not advertise
  `context.location_identity`, and the ancestry capability depends on it. Enabling both in VR
  needs in-headset evidence first.

## Scope choices

- **Inside the existing context.** Ancestry is part of the immutable context snapshot the server
  already stores under the existing checkpoint and loaded-context rules. A repeated or replayed
  context is the existing idempotent publication; there is no separate export, catalog or new
  database schema.
- **Current location only.** No catalog of every location, no map positions, markers, keywords,
  holds or settlement typing. The chain is what the engine says contains the current location.
- **No guessing.** Names never resolve identity. A missing, unresolved or cyclic parent stays
  explicit and nothing past the last entry is inferred.
- **No knowledge grant.** The chain describes where the captured scene is. It adds no audience,
  witness or memory and does not tell other NPCs about the place.
- **Server prompt integration.** Synthserver uses
  `synth_location_prompt_line()` and `synth_location_ancestry_facts()` in `lib/synth_location.php`
  in the native turn context. Ordinary dialogue already receives the immutable world object.

## Tests

- `tests/protocol_native`: manifest decoding for full, light, wrong-slot, runtime and missing
  plugins; equal names across plugins; gating; and rejection of repeated, mismatched, empty,
  oversized and falsely truncated chains.
- `tests/fake_server`: ancestry reaches only an acknowledging flat server with both capabilities.
- `scripts/verify_protocol.py`: one valid and four invalid v2 fixtures.

Open in-game checks are in [ALPHA-ACCEPTANCE.md](ALPHA-ACCEPTANCE.md#location-ancestry).
