# SYNTH native architecture

For the reference installation topology and the complete game -> client -> PHP -> PostgreSQL/
providers -> client -> game data flow, read `REFERENCE-STACK-DATAFLOW.md`. This document defines
the target native boundaries used to replace those reference mechanisms.

## Ownership model

The selected `IFalloutRuntime` implementation is the only component allowed to include
CommonLibF4/CommonLibF4VR, F4SE/F4SEVR, HMD/controller, hook, or game headers and retain temporary
engine references. `FlatFalloutRuntime` produces `SYNTH.dll`; `FalloutVrRuntime` produces
`SYNTHVR.dll`. Both snapshot engine state on the game thread into plain owned data. Workers may
read snapshots and produce typed commands; only `GameThreadDispatcher` may execute those commands
against the current engine generation.

```text
F4SE or F4SEVR lifecycle/input/events
        |
        v
IFalloutRuntime -> RuntimeSnapshot -> context/dialogue workers -> Synthserver
      ^                                      |
      |                                      v
GameThreadDispatcher <- typed command <- strict response router
```

## Components

- `PluginEntry`: target-specific version data, load, dependency/runtime checks, interface
  acquisition, and rejection of the other runtime lane.
- `IFalloutRuntime`: engine-free snapshot/action interface used by product code and fakes.
- `FlatFalloutRuntime`: flat F4SE/CommonLibF4 lifecycle, menus, snapshots, presentation, and typed
  action primitives.
- `FalloutVrRuntime`: F4SEVR/CommonLibF4VR lifecycle, HMD/controller/handedness snapshots,
  VR-safe UI/presentation, optional FRIK interface, and typed action primitives.
- `RuntimeGeneration`: monotonically invalidates work on state boundaries.
- `RuntimeEventBus`: bounded copied lifecycle/input/dialogue events.
- `RuntimeSnapshot`: immutable game/player/actor/reference/quest/plugin views with freshness.
- `TaskManager`: bounded lanes for HTTP, audio preparation, STT upload, context serialization, and
  maintenance; deadlines, priorities, coalescing, cancellation, joined shutdown, health.
- `GameThreadDispatcher`: typed queue with max pending, generation and identity validation.
- `Protocol`: strict versioned JSON parser/serializer and size/depth/string limits.
- `HttpClient`: loopback allowlist by default, WinHTTP cancellation, connect/read deadlines,
  NDJSON stream parser, no credential logging.
- `AgentRegistry`/`TargetManager`: one source of truth for manually/automatically active actors.
- `DialogueRuntime`: conversation ownership, group audience, response/rechat queue, interruption.
- `InputRuntime`: flat hotkeys/text entry plus semantic VR actions, push-to-talk, optional VAD/open
  mic, handedness, haptic feedback, and keyboard development fallback.
- `AudioRuntime`: TTS fetch/cache, decode, 3D playback using player/HMD listener pose,
  flat/VR-specific subtitle/lipsync/facing capability gates.
- `ContextRuntime`: bounded periodic and before-input snapshots.
- `ActionRuntime`: allowlisted action decoding, exact target resolution, native execution/results.
- `Diagnostics`: queue/task/generation/server/presentation/action health without secrets.

## Threading invariants

1. The game thread never waits for HTTP, disk scans, STT/TTS, audio decode, or model work.
2. Workers never dereference a form, handle, menu, VM, actor, cell, inventory, or UI object.
3. Every worker callback checks generation before enqueueing and every dispatcher item checks again
   immediately before mutation.
4. Queue capacity is finite. Low-priority context work coalesces; player input and hard halt have
   reserved capacity. Saturation is visible and never silently drops accepted gameplay work.
5. HMD/controller transforms are freshness-bounded snapshots; stale poses disable targeting/audio
   updates rather than falling back silently to an unrelated player-root position.
6. Shutdown stops acceptance, cancels pending I/O, drains/cancels completions, joins workers, then
   releases interfaces.

## Protocol and identity

Every envelope includes:

- schema and protocol version;
- request/turn ID and runtime generation;
- game ID `fo4`, runtime variant `flat` or `vr`, and client/server/runtime versions;
- player, speaker, listener/audience identities;
- stable full form IDs plus plugin provenance when available;
- event type/payload and response status;
- explicit capability list for actions/presentation/context domains.
- optional VR capability data and a sanitized compatibility-profile fingerprint.

Never use actor display name as authority. Never execute a server-provided console string, Papyrus
source, file path, URL, form ID outside the current scene/capability, or unknown action.

## No-ESP capability boundary

Expected no-ESP capabilities:

- lifecycle, configuration, logging, hotkeys, crosshair/nearby scans;
- HTTP and strict protocol;
- text/PTT/STT/TTS, native audio, basic subtitles/notifications;
- read-only world/actor/item/quest/plugin context;
- manual/automatic in-memory agent registry;
- actions expressible safely with existing game objects and native calls.

Expected ESP-dependent capabilities:

- persistent quests and aliases, custom package forms, factions/keywords/globals/messages;
- custom dialogue topics/scenes and holotape/controller items;
- robust follower conversion or custom AI packages;
- Papyrus event bridge that requires an attached script instance;
- form-backed MCM/controller integrations. The in-game settings surface is instead
  registered natively with the optional F4SE Menu Framework; see
  `provenance/F4SEMENUFRAMEWORK.md`.

The server receives a runtime capability manifest and must never offer an unavailable action.

## Fallout 4 VR boundary

VR uses the same no-ESP product but independently proves HMD-gaze/controller targeting, semantic
PTT, HMD-relative spatial audio, VR-readable subtitles, VR-safe visemes, lifecycle, native actions,
performance, and compatibility profiles. FRIK and Fallout4 VR Tools are optional interfaces. A
minimal F4SEVR + VR Address Library profile must remain functional without them.

See `FALLOUT4-VR-PLAN.md` for dependency pins, packaging, popular stack compatibility, performance
budgets, and the in-headset acceptance matrix.

## Test architecture

- Pure unit tests: parsing, IDs, config, task manager, generation, queues, geometry/math, actions.
- Fake runtimes: flat and VR `IFalloutRuntime` snapshots and action primitives with thread,
  transform freshness, handedness, and capability assertions.
- Fake server: local HTTP/NDJSON fixtures shared with Synthserver.
- Fuzz/negative: malformed/truncated/oversized JSON, duplicate IDs, stale generations, hostile
  strings/paths/URLs, stream interruption.
- Windows build/package: exact flat and VR x64 outputs and independent manifests.
- Windows in-game: separate flat and in-headset VR external evidence; never mocked or inherited.
