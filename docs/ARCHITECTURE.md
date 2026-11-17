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

## Source map and where new code goes

`src/` is header-first C++23 grouped by layer. The groups, from the engine outward:

| Directory | Role | Engine headers? |
|---|---|---|
| `src/core/`, `src/json/`, `src/protocol_native/`, `src/transport/`, `src/tasks/`, `src/context/`, `src/config/`, `src/actions/`, `src/targeting/`, `src/presentation/`, `src/audio/`, `src/voice/`, `src/diagnostics/` | Engine-free logic: snapshots, mailboxes, generation and cancellation, protocol, tasks, typed actions, presentation policy | No |
| `src/runtime/` | `IFalloutRuntime`, `GameThreadDispatcher` and the other engine-facing interfaces | No |
| `src/client/` | Session composition and feature delivery code (`PluginSession`, dialogue, speech, observed events, rechat); 37 flat headers today | No |
| `src/adapters/` | `Flat*` and `Vr*` runtimes and the CommonLib/F4SE bindings (`flat_*`, `commonlib_*`) | **Yes, the only place** |
| `src/flat/`, `src/vr/` | Per-lane `plugin_entry.cpp` | Yes |
| `src/integration/`, `src/input/`, `src/ui/`, `src/lifecycle/`, `src/media_*/`, `src/radio/` | Support areas for other mods, input, menus, lifecycle, media and the radio prototype | Only where already present |

Tests mirror the layout under `tests/<area>/`; `tests/fake_server` and `tests/loopback_dialogue`
exercise the protocol end to end.

### Rule for new feature code

New features use **`src/features/<name>/`**, as the CHIM 4 client does (`src/features/books`,
`orders`, `trading`, ...), with these constraints that keep SYNTH's own boundaries:

1. **A feature folder is engine-free.** It holds the feature's state, decisions, request building and
   response handling as plain headers that include only `core`, `runtime`, `protocol_native` and
   other engine-free groups. It never includes CommonLibF4/F4SE or game headers.
2. **Engine access goes through `src/runtime/` interfaces.** If a feature needs a new engine
   primitive, add it to `IFalloutRuntime` (engine-free signature) and implement it in
   `src/adapters/flat_*.hpp` and `vr_*.hpp`. Never put adapter code inside a feature folder.
3. **Flat and VR stay split in the adapters.** A feature does not branch on lane. It reads the
   runtime capability manifest and degrades or refuses where VR lacks a path; lane-specific code lives
   only in `src/adapters/` and the two `plugin_entry.cpp` files.
4. **`src/client/plugin_session*.hpp` stays the composition root.** A feature exposes one small
   owner object that the session creates and wires; the session does not grow feature logic.
5. **Protocol types stay in `src/protocol_native/`** and are mirrored with Synthserver `protocol/`.
6. **Tests go in `tests/features/<name>/`** (registered like the other native suites), plus a
   `tests/fake_server` case when the wire changes.
7. **No moves in feature PRs.** Existing `src/client/*` headers move into `src/features/<name>/` one
   feature at a time in dedicated PRs. Until then, extend them in place only for bug fixes and hooks
   that call the new feature owner.

Why `src/features/<name>/` and not more flat files in `src/client/`: every new feature touches the
same few directories today (`client`, `core`, `adapters`, `protocol_native`), which makes parallel work
collide and hides what a feature owns. A feature folder gives one owner, one test directory and one
note. Why not copy CHIM 4's folders wholesale: CHIM 4 organizes features around its engine layer, while SYNTH
must keep the engine-free core plus adapters split and two runtime lanes. So the feature folder is
the engine-free half only.

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
