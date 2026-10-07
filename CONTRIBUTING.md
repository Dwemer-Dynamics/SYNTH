# Contributing to SYNTH

Read [AGENTS.md](AGENTS.md) for the engineering rules and [docs/HANDOFF.md](docs/HANDOFF.md)
for the current state.

## Branches

Start every feature branch from the latest `origin/alpha`. Open every PR as a **draft targeting
`alpha`**. Do not target `unstable`, `dev`, `synth`, or an old feature branch. SYNTH uses an
alpha-first workflow, unlike the other Dwemer mods. `synth` is the GitHub default branch.

`unstable` and `dev` are reserved testing lanes; `synth` is the main/release lane. Maintainers
control promotions explicitly. For dependent work, merge prerequisites into `alpha` before
opening the next focused PR. All PRs still target `alpha`.

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

## Checks before opening a PR

```powershell
python scripts/verify_protocol.py --peer ../Synthserver/protocol
python tests/build/test_build_gates.py
python tests/packaging/test_release_audit.py
git diff --check
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
