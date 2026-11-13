# Loopback dialogue driver (opt-in, test-only)

`loopback_dialogue_driver.cpp` runs the real `SynthClient` and `SaveContextStore` over real HTTP
against a disposable Synthserver that is already listening on `127.0.0.1`. CMake, xmake, ctest
and hosted CI don't build it, and it is never packaged. The source comment describes what it does.

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

The harness starts the driver as `loopback_dialogue_driver http://127.0.0.1:PORT`. At each
`PHASE <name> <json>` line on stdout, the driver waits for `GO` on stdin. Any other reply, or
stdin closing, aborts the run.

| Phase | What the harness does before replying `GO` |
| --- | --- |
| `context` | Registers an NPC/profile/LLM connector for `target_form` under `playthrough_id` and scripts the fake provider's answer. |
| `checkpoint` | Checks the heard row, utterance state, receipt executions and anchor frontiers, then scripts the next answer. |
| `older_talk` | Checks that the latest provider prompt does not contain the heard line. |
| `newer_talk` | Checks that the latest provider prompt contains the heard line exactly once. |

The driver prints evidence on lines that start with `LOOPBACK `. It prints `DONE` and exits 0
only if every client-side check passed. Otherwise it prints `FAILED <reason>` and exits 1.
