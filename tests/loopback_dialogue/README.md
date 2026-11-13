# Loopback dialogue driver (opt-in, test-only)

`loopback_dialogue_driver.cpp` runs the real `SynthClient`, `SaveContextStore` and `SaveCheckpoint`
and the shared speech receipt path over real HTTP against a disposable Synthserver that is already
listening on `127.0.0.1`. CMake, xmake, ctest and hosted CI don't build it, and it is never
packaged. The source comment describes what it does.

The driver refuses any base URL other than `http://127.0.0.1:PORT`. Point it only at a disposable
server, a throwaway database and a loopback fake provider, all inside an isolated network
namespace. Never point it at a real Synthserver, a real provider or a game install.

## Run

The Synthserver repository owns the harness. From any clean server/client checkout pair, as root
on Linux or in WSL (`wsl -u root`):

```sh
sudo bash SERVER/unittests/loopback/run-loopback-dialogue.sh CLIENT
```

`SERVER` is a Synthserver checkout and `CLIENT` is this checkout. The command exports the `HEAD`
commit of each checkout into private namespaces and builds this driver from the export. It
starts a disposable PostgreSQL cluster and the server on loopback, then runs the server's
`LoopbackDialogueTest`. Prerequisites, isolation and exit statuses are in that repository's
`unittests/loopback/README.md`.

## Build

The harness builds the driver itself. To build it by hand (Linux or WSL, g++ 12 or later):

```sh
g++ -std=c++23 -O1 -Wall -Wextra -Wpedantic -Werror -Wno-missing-field-initializers -pthread \
  -Isrc tests/loopback_dialogue/loopback_dialogue_driver.cpp -o loopback_dialogue_driver
```

The shipped headers trigger GCC's `-Wmissing-field-initializers` warning on designated
initializers, so that warning is disabled. Clang doesn't need the flag.

## Harness contract

The harness starts the driver as `loopback_dialogue_driver http://127.0.0.1:PORT`, which runs the
default `dialogue` scenario (13 requests). An optional second argument selects the scenario:
`dialogue` (same as omitting it) or `companion` (see below). At each
`PHASE <name> <json>` line on stdout, the driver waits for `GO` on stdin. Any other reply, or
stdin closing, aborts the run.

| Phase | What the harness does before replying `GO` |
| --- | --- |
| `context` | Registers an NPC/profile/LLM connector for `target_form` under `playthrough_id` and scripts the fake provider's answer. |
| `checkpoint` | Checks the heard row, utterance state, receipt executions and anchor frontiers, then scripts the next answer. |
| `older_talk` | Checks that the latest provider prompt does not contain the heard line. |
| `newer_talk` | Checks that the latest provider prompt contains the heard line exactly once. |

The driver prints evidence on lines that start with `LOOPBACK `. It prints `DONE` and exits 0
only if every client-side check passed. Otherwise it prints `FAILED <reason>` and exits 1. A bad
argument prints usage and exits 2.

## Companion scenario (opt-in)

`loopback_dialogue_driver http://127.0.0.1:PORT companion` runs one flat session,
`loopback-companion`, against the fixture companion Piper (`Fallout4.esm` `0x00002F1F`). Its
capabilities are the dialogue set plus `dialogue.turn_cancel` and the flat Wait set from the shipped
`append_wait_capabilities`. The phases, in order:

| Phase | Driver work before it | What the harness does before replying `GO` |
| --- | --- | --- |
| `companion_context` | Sends init (generation 1), then context 1 with Piper as the target, the audience (`complete`) and one captured actor state. | Checks the stored snapshot, and that no `core_npc_master` row exists for Piper. Adds an LLM connector and a default profile, but no NPC row. Scripts the answer to the ordinary question. |
| `companion_question` | Sends a manual `activate` bound to context 1 and expects `activate_accepted`. Sends context 2 and an unmarked question. Expects at least one dialogue line, no native-turn line and no action. | Checks the `active_agents` manual row. Checks that the server created Piper's `core_npc_master` row, bound to her actor identity. Scripts the marked conversation answer. |
| `companion_conversation` | Sends context 3 and a marked conversation. Expects exactly one native-turn line, spoken by Piper. | Scripts the answer to the unmarked `Wait here.` so that it returns exactly one `WaitHere` action for Piper. |
| `companion_action` | Sends context 4 and an unmarked `Wait here.`. Expects exactly one `wait_here` / `action.wait_here` action owned by Piper, with no target. The shipped `prepare_wait_action` checks it against a synthetic captured scene. The same validator must refuse, locally, the action with an absent actor, the action with another origin plugin, a VR scene, and a cancelled turn whose session is still live. `append_wait_capabilities` adds nothing for VR. The shipped replay guard admits the action once and then refuses it. | Checks that there is a pending `action_intents` row. Scripts any continuation answer that the result requests. |
| `companion_result` | Sends a simulated `unavailable` result twice, and both must complete. Sends a contradicting `failed` result, which must be refused with `HTTP 409: action_result_conflict`. Tries a result with an unnegotiated inventory observation, which must be refused locally. | Checks that there is one terminal result, `unavailable`. Checks that the intent is `complete` and that no physical Wait was recorded as verified. Scripts the next conversation answer. |
| `companion_done` | Sends context 5 and a marked conversation that supersedes the command's turn. Resends the result, which must be refused with `HTTP 409: superseded_turn`. Sends context 6, cancels it and expects `cancel_accepted`. Sends a conversation bound to context 6, which must be refused with `HTTP 409: turn_cancelled`. Cancels the generation 1 owner. `prepare_wait_action` then refuses the action's turn, and a result returns `cancelled` with `request cancelled before send` and no HTTP. Re-initializes as generation 2. A result whose binding has a live generation 1 owner is refused locally with `turn owner belongs to another runtime generation`. | Checks the final request accounting. |

Expected `request_executions` rows: init 2, context 6, activate 1, input_text 4, action_result 2,
and cancel 1, for 16 in total. Three more HTTP requests are refused before any claim is stored:
the conflict, the stale result and the cancelled-turn conversation. The harness may assert
provider call counts only for answers it scripted.

Limits of the companion scenario:

- Nothing is executed in a game. The actor state and the scene that `prepare_wait_action` validates
  are synthetic. The executor outcome is simulated and is reported honestly as `unavailable`, with
  a `SIMULATED` detail. `verified_wait_result` is not exercised, and no physical Wait is claimed.
  Flat and VR in-game evidence remain open in `docs/ALPHA-ACCEPTANCE.md`.
- `ActionExecution`, `ActivationTriggers`, `DialogueRequests` and `ContextPublication` need Windows
  headers, so the driver calls the engine-free `SynthClient` APIs in their order instead. It
  doesn't route lines, apply hold suspension, or prove the game-thread dispatch.
- The cancelled-turn probe deliberately leaves the local turn token live so that it can reach the
  server. In production, the cancellation also cancels the local token, and the request is never
  sent.
- `send_action_result` takes no owner argument. The local cancellation probe relies on the owner
  that travels in the `ContextBinding`: `publish()` acknowledges each context under the session
  owner's token, and `SynthClient` checks that token before it calls the transport. The probe
  cancels the owner directly. It doesn't prove that the Windows-only coordinator cancels it on a
  load, a new game or the main menu, and it doesn't cover a cancellation that arrives while a
  request is in flight. The native fake-server suite covers the in-flight case.
- VR is not covered. Wait capabilities are flat-only.
