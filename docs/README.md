# SYNTH documentation index

Where to find the current notes for each part of the client, and the source and tests behind
them. This index only points to existing documents. Status and open checks stay in
[HANDOFF.md](HANDOFF.md) and [ALPHA-ACCEPTANCE.md](ALPHA-ACCEPTANCE.md); a build or unit test is
never flat or VR gameplay evidence.

## Start here

- [README](../README.md): runtimes, package contents, configuration, build and test.
- [CONTRIBUTING](../CONTRIBUTING.md): branches, rules, code and comment conventions, PR checks.
- [AGENTS](../AGENTS.md): engineering boundaries. [BRANCHES](../BRANCHES.md): branch policy.
- [HANDOFF.md](HANDOFF.md): current baseline, session ownership, known gaps.
- [ARCHITECTURE.md](ARCHITECTURE.md): adapter, thread and protocol boundaries.
- [PROTOCOL.md](PROTOCOL.md) and [protocol/README.md](../protocol/README.md): the wire contract
  mirrored with Synthserver.

## Components

Paths are relative to the repository root. "Nearest tests" are suites that exercise that code;
they do not prove the whole feature. "No dedicated note" means the behavior is described only
in the handoff, matrix or source comments.

| Component | Notes | Source entry points | Nearest tests |
|---|---|---|---|
| Plugin load and runtime checks | [FALLOUT4-TOOLCHAIN.md](FALLOUT4-TOOLCHAIN.md) | `src/flat/plugin_entry.cpp`, `src/vr/plugin_entry.cpp`, `src/adapters/adapter_descriptor.hpp` | `tests/adapters`, `tests/build` |
| Session composition and feature owners | [HANDOFF.md](HANDOFF.md#client-session-ownership) | `src/client/plugin_session.hpp`, `src/client/plugin_session_host.hpp`, `src/lifecycle/lifecycle_coordinator.hpp` | `tests/lifecycle`; no test compiles `PluginSession` itself |
| Runtime adapters and game thread | [ARCHITECTURE.md](ARCHITECTURE.md) | `src/runtime/fallout_runtime.hpp`, `src/runtime/game_thread_dispatcher.hpp`, `src/adapters/flat_fallout_runtime.hpp`, `src/adapters/vr_fallout_runtime.hpp` | `tests/adapters`, `tests/native` |
| Generation, cancellation and worker lanes | [ARCHITECTURE.md](ARCHITECTURE.md#threading-invariants) | `src/core/generation.hpp`, `src/core/cancellation.hpp`, `src/tasks/task_lanes.hpp` | `tests/tasks`, `tests/lifecycle` |
| Protocol and transport | [PROTOCOL.md](PROTOCOL.md), [RESPONSE-FLOW-PARITY.md](RESPONSE-FLOW-PARITY.md) | `src/protocol_native/v1_codec.hpp`, `src/json/json.hpp`, `src/client/synth_client.hpp`, `src/client/winhttp_transport.hpp`, `src/transport/transport_state.hpp` | `tests/protocol_native`, `tests/json`, `tests/transport`, `tests/fake_server`, `scripts/verify_protocol.py` |
| Save identity and context publication | [HANDOFF.md](HANDOFF.md#synth-owned-playthrough-identity) | `src/core/save_context.hpp`, `src/client/save_checkpoint.hpp`, `src/client/context_publication.hpp`, `src/adapters/commonlib_save_context.hpp` | `tests/lifecycle`, `tests/loopback_dialogue` (opt-in) |
| Speech, audio and presentation | [NPC-SPEECH-OWNERSHIP.md](NPC-SPEECH-OWNERSHIP.md), [NATIVE-SUBTITLE-AUDIT.md](NATIVE-SUBTITLE-AUDIT.md), [LIPSYNC-NATIVE-AUDIT.md](LIPSYNC-NATIVE-AUDIT.md), [FACING-MENU-PAUSE.md](FACING-MENU-PAUSE.md) | `src/client/speech_delivery.hpp`, `src/client/speech_receipts.hpp`, `src/audio/`, `src/presentation/` | `tests/audio`, `tests/presentation` |
| Player input and targeting | [FLAT-PICKED-REFERENCE-AUDIT.md](FLAT-PICKED-REFERENCE-AUDIT.md) | `src/client/player_dialogue.hpp`, `src/input/`, `src/targeting/targeting.hpp` | `tests/input`, `tests/targeting` |
| Typed actions, Wait and items | [WAIT-ALIAS-CHAT.md](WAIT-ALIAS-CHAT.md), [PICKUP-ACTION.md](PICKUP-ACTION.md), [CAPS-CURRENCY-AUDIT.md](CAPS-CURRENCY-AUDIT.md), [ACTION-INVENTORY-OBSERVATIONS.md](ACTION-INVENTORY-OBSERVATIONS.md), [WORLD-ITEM-CAPTURE.md](WORLD-ITEM-CAPTURE.md) | `src/actions/actions.hpp`, `src/client/action_execution.hpp`, `src/client/wait_action.hpp`, `src/adapters/flat_wait_package.hpp`, `src/adapters/flat_pickup_package.hpp` | `tests/actions` |
| Conversation movement and hearing | [CONVERSATION-HOLDS.md](CONVERSATION-HOLDS.md) | `src/client/conversation_holds.hpp`, `src/runtime/conversation_approach.hpp`, `src/adapters/flat_conversation_approach.hpp` | `tests/actions`, `tests/presentation`, `tests/packaging/test_approach_gameplay.py` |
| Follower errands (social tasks) | No dedicated note; [HANDOFF.md](HANDOFF.md) | `src/client/social_task_execution.hpp`, `src/runtime/social_task.hpp`, `src/adapters/flat_social_visit.hpp` | `tests/actions`, `tests/fake_server`, `tests/build/test_social_visit_gates.py` |
| Rechat and idle exchanges | Server `docs/NATIVE-RECHAT-OWNERSHIP.md` | `src/client/rechat_continuation.hpp`, `src/client/rechat_prefetch.hpp`, `src/client/ambient_exchange.hpp` | `tests/fake_server`, `tests/protocol_native` |
| Observed game events | [FLAT-SEMANTIC-EVENT-AUDIT.md](FLAT-SEMANTIC-EVENT-AUDIT.md), [QUEST-REACTIONS.md](QUEST-REACTIONS.md), [FLAT-REST-EVENT-EVIDENCE.md](FLAT-REST-EVENT-EVIDENCE.md), [FLAT-INVENTORY-CHANGE-AUDIT.md](FLAT-INVENTORY-CHANGE-AUDIT.md) | `src/client/observed_event_delivery.hpp`, `src/client/quest_event_delivery.hpp`, `src/core/*_mailbox.hpp`, `src/adapters/flat_*_events.hpp` | `tests/context`, `tests/fake_server` |
| Location ancestry | [LOCATION-ANCESTRY.md](LOCATION-ANCESTRY.md) | `src/adapters/commonlib_capture.hpp` (`location_ancestry`), `src/core/snapshot.hpp`, `src/protocol_native/v1_codec.hpp` | `tests/protocol_native`, `tests/fake_server`, `scripts/verify_protocol.py` |
| Combat identification | [COMBAT-TARGETS.md](COMBAT-TARGETS.md) | `src/core/combat_target.hpp`, `src/core/enemy_awareness.hpp`, `src/client/actor_state_combat.hpp` | `tests/native`, `tests/context`, `tests/fake_server` |
| Diaries | [MANUAL-DIARY-TEST.md](MANUAL-DIARY-TEST.md) | `src/client/diary_requests.hpp`, `src/core/automatic_diary_queue.hpp` | `tests/fake_server`, `tests/input` |
| Visual capture | No dedicated note; [PROTOCOL.md](PROTOCOL.md) | `src/client/visual_capture.hpp`, `src/media_capture/window_capture.hpp` | `tests/context`, `tests/fake_server`; window capture untested |
| Voice samples | [VOICE-CLONING.md](VOICE-CLONING.md) | `src/voice/` | `tests/native` |
| Configuration, Control menu and settings pages | [README](../README.md#configuration), [CONTROL-MENU-PARITY.md](CONTROL-MENU-PARITY.md), [F4SEMENUFRAMEWORK.md](../provenance/F4SEMENUFRAMEWORK.md) | `config/SYNTH.ini`, `src/config/config.hpp`, `src/ui/`, `src/client/server_control.hpp` | `tests/config`, `tests/input`, `tests/build` |
| Native and Papyrus APIs for other mods | [NATIVE-EVENT-API.md](NATIVE-EVENT-API.md), [PAPYRUS-API.md](PAPYRUS-API.md) | `src/integration/synth_external_api.h`, `src/integration/`, `scripts/papyrus/` | `tests/input`, `scripts/verify_papyrus.py` |
| Gameplay records (`SYNTH.esp`) | [ESP-DEFERRED-SETUP.md](ESP-DEFERRED-SETUP.md), [WAIT-HERE.md](WAIT-HERE.md) | `scripts/build_gameplay.py`, `scripts/build_wait_script.py`, `scripts/papyrus/` | `tests/build`, `tests/packaging` |
| Radio prototype (optional) | [RADIO-PROTOTYPE.md](RADIO-PROTOTYPE.md) | `src/radio/`, `scripts/build_radio_*.py` | `tests/radio` |
| Diagnostics and fault capture | No dedicated note | `src/diagnostics/`, `src/adapters/native_fault_recorder.hpp` | `tests/diagnostics` |
| Packaging, licensing and publication | [PUBLICATION-CHECKLIST.md](PUBLICATION-CHECKLIST.md), [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) | `scripts/package_release.py`, `packaging/`, `tools/audit_release_tree.py` | `tests/packaging/test_release_audit.py` |

Cross-cutting status: [FEATURE-PARITY-MATRIX.md](FEATURE-PARITY-MATRIX.md) and
[CONVERSATION-PARITY-GOAL.md](CONVERSATION-PARITY-GOAL.md). Off-game client/server check:
[tests/loopback_dialogue/README.md](../tests/loopback_dialogue/README.md).

## Documentation audit (2026-11-13)

- **Current entry points.** README, CONTRIBUTING, AGENTS, BRANCHES, HANDOFF, ALPHA-ACCEPTANCE,
  ARCHITECTURE and PROTOCOL agree on branch policy, runtimes and evidence boundaries.
- **Feature notes.** Most notes are per-feature records with their own scope, evidence and open
  checks. Few name source files, so the table above supplies the entry points.
- **Historical.** [PROGRAM-PLAN.md](PROGRAM-PLAN.md), [FALLOUT4-VR-PLAN.md](FALLOUT4-VR-PLAN.md)
  and [REFERENCE-STACK-DATAFLOW.md](REFERENCE-STACK-DATAFLOW.md) are marked historical.
  [PUBLIC-SNAPSHOT.md](PUBLIC-SNAPSHOT.md) records the 2026-10-06 snapshot; its branch paragraph
  predates the current `unstable` policy in [BRANCHES.md](../BRANCHES.md), which wins.
- **Gaps.** No dedicated note for follower errands, visual capture or diagnostics/fault
  capture. Some notes cite files from reference repositories (for example Dialectic's
  `ActionManager.cpp`); these are not SYNTH source. The layout block in
  [FALLOUT4-TOOLCHAIN.md](FALLOUT4-TOOLCHAIN.md) names superseded `scripts/build.ps1`,
  `scripts/package.ps1` and `scripts/test.ps1`; the current scripts are listed in the
  [README](../README.md#build-and-test).
- **Reference guidance** reused or excluded for the conventions:
  [provenance/REFERENCES.md](../provenance/REFERENCES.md#engineering-guidance-references).

## Keeping docs current

Update the feature note, the handoff and the acceptance checklist; do not start a chronological
ledger. Add a row here only when a component gains a note or a new source root. Use relative
links with the exact Git path case (for example `docs/HANDOFF.md`, `provenance/REFERENCES.md`).
