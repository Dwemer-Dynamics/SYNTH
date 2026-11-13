# Contributing to SYNTH

Read [AGENTS.md](AGENTS.md) for the engineering rules and [docs/HANDOFF.md](docs/HANDOFF.md)
for the current state.

## Branches

Start every feature branch from the latest `origin/unstable`. Open every topic PR, from this
repository or a fork, as a **draft targeting `unstable`**. Do not target `dev`, `synth`,
`alpha`, or an old feature branch. `synth` is the GitHub default branch.

`unstable` is the integration lane, `dev` is the tested-development lane and `synth` is the
main/release lane. `alpha` is a legacy lane. Maintainers promote only from this repository,
`unstable` -> `dev` and `dev` -> `synth`; the PR branch-policy check rejects reverse, skipped
and fork promotions, including any PR from this repository's `alpha`. A fork PR is a topic PR
whatever its head branch is named and may target `unstable`. See [BRANCHES.md](BRANCHES.md). For
dependent work, merge prerequisites into `unstable` before opening the next focused PR.

Explain the concrete behavior change, list actual checks and remaining limits, and link paired
client/server changes. A build does not prove flat or VR gameplay behavior.

## Rules

- Keep each change focused. Do not mix behavior changes with cleanup, formatting or generated
  files.
- Engine, F4SE/F4SEVR and game-object access stays behind the runtime adapters and on the game
  thread. Never copy flat offsets into VR.
- Protocol changes are versioned. Mirror `protocol/` byte-for-byte with Synthserver, and link
  the paired server PR.
- Never renumber or remove existing `SYNTH.esp` FormIDs, aliases or records. Saves refer to
  them.
- Never commit DLLs, PDBs, archives, `out/`, logs, game files, Bethesda script imports,
  `SYNTH_custom.ini`, credentials or personal paths.
- Prefer the existing test suites. Add a new test file only when existing suites cannot cover
  a high-risk change.

## Code and comments

These apply to new and changed code. Do not reformat or re-comment untouched code.

- Start a new header or source file with a brief comment on what it owns when the file name
  alone does not say so. Put a brief purpose comment above a class or function only when its
  role is unclear from its name and signature.
- Comment intent where it is not obvious: thread, ownership, lifetime, ordering, generation or
  cancellation rules, save compatibility and deliberate limits. Do not restate the code. There
  is no comment-per-function or per-line quota.
- Use descriptive names and straightforward flow. Give each piece of state one clear owner (see
  the [session ownership table](docs/HANDOFF.md#client-session-ownership)). Do not add a helper
  that is used once and makes the flow harder to follow.
- Match the surrounding style: `snake_case` functions and members, `PascalCase` types, the
  file's existing indentation and brace placement. There is no repository-wide formatter.
- Engine access stays behind the selected `IFalloutRuntime` and runs on the game thread.
  Workers get copied snapshots, check the runtime generation before queueing work, honour
  cancellation and never block the game thread.

Examples from the current source:

```cpp
// src/core/save_context.hpp: intent that the code cannot show
// Fixed-size little-endian encoding avoids ABI padding and unbounded co-save allocations.

// src/client/plugin_session.hpp: thread rule at the call boundary
// Called once on the game thread at speech admission, never from the microphone worker.
```

`src/client/speech_delivery.hpp` shows a longer owner comment for a class whose responsibilities
are not obvious from its name.

## Documentation

Find component notes and their source entry points in [docs/README.md](docs/README.md). Update the
relevant feature note, [docs/HANDOFF.md](docs/HANDOFF.md) and
[docs/ALPHA-ACCEPTANCE.md](docs/ALPHA-ACCEPTANCE.md); do not start a chronological ledger. State
what was checked and what remains unchecked. Use relative links with the exact Git path case.

## Checks before opening a PR

```powershell
python scripts/verify_protocol.py --peer ../SynthServer/protocol
python tests/build/test_build_gates.py
python tests/packaging/test_release_audit.py
git diff --check
```

`--peer` takes the `protocol` directory of your local server checkout, not the checkout root.
The command assumes a sibling clone of `Dwemer-Dynamics/SynthServer` named `SynthServer`;
adjust the path to your actual protocol directory.

For a narrow check, run one suite. A `tests/<suite>/run.py` script compiles and runs its suite;
most accept a compiler through `CXX`. The `ctest` form needs the CMake core build from the
[README](README.md#build-and-test).

```powershell
python tests/tasks/run.py
ctest --test-dir out/ci-core -R synth-tasks-tests --output-on-failure
```

For native changes, also build and test the affected lane(s). See
[README](README.md#build-and-test). Report flat and VR results separately. Never infer in-game
behavior from builds; use [docs/ALPHA-ACCEPTANCE.md](docs/ALPHA-ACCEPTANCE.md).

## Licensing

Submit project contributions under the GNU GPL v3.0 (root `LICENSE`). Preserve third-party
notices: reused Dialectic material keeps its MIT notice in `LICENSES/DIALECTIC-MIT.txt`, and
every notice in `packaging/licenses/` stays with its lane (see
[docs/PUBLICATION-CHECKLIST.md](docs/PUBLICATION-CHECKLIST.md)). Do not add third-party code
without its license notice and a provenance entry in `provenance/`.
