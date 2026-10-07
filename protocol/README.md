# SYNTH protocol artifacts

This tree is the executable companion to `docs/PROTOCOL.md`. Every file under `protocol/` is mirrored byte-for-byte between SYNTH and Synthserver.

## Version 1 decisions

- Client events use `schema: synth.event.v1`; `type` selects init, text/audio input, context, activation, persisted-policy greeting plus bounded bored/rechat triggers, action-result, cancellation, or halt behavior.
- Stream lines use `schema: synth.response.line.v1` and the closed line-type catalog.
- `generation` is the only wire generation field. `runtime_generation` is not a compatibility alias.
- `session_id` is a fresh opaque runtime-process lane. It prevents restart collisions while generation rejects stale work within that lane.
- Action and action-result payloads have dedicated closed schemas, but an action result is transported as an `action_result` client event.
- The default compatibility entry point is `main.php`; implementations may route it to the same internal v1 event handler.
- Protocol fields and fixtures are identical for flat and VR. The runtime lane and negotiated capabilities distinguish behavior.
- Actor faction entries use canonical full form/plugin identity and rank; `faction_observation` distinguishes flat effective enumeration from VR base-only or unavailable evidence.
- Actor activity is bounded value state (life/posture, weapon, movement, dialogue, power armor and canonical package identity); engine pointers never cross the adapter boundary.
- Audio and images are native bounded HTTP bodies or responses referenced by opaque content-addressed media IDs; Base64 media in JSON is forbidden.

## Canonical bytes

Files are UTF-8 without a BOM, use LF endings, and end in one LF. `manifest.sha256` lists every regular file under `v1/` except itself, sorted by POSIX path. The verifier checks JSON parsing, closed production object schemas, fixture expectations, manifest integrity, and sibling-tree identity.

Run:

```sh
python3 scripts/verify_protocol.py --peer ../SYNTH/protocol
```
