# SYNTH developer handoff

Updated: 2026-10-06. This is the current entry point for SYNTH development. Older plans and
audits under `docs/` are kept as feature evidence, and some carry a historical banner.

## Baseline

| Item | Value |
|---|---|
| Client source | `RANGROO/SYNTH` `codex/chim-conversation-targeting` at `f34da88` (includes `c32f624` and four later targeting/restraint fixes) |
| Paired server | `RANGROO/Synthserver` `codex/chim-queue-parity` at `bcb0c06` |
| Plugin version | `0.1.0` (`xmake.lua`, `vcpkg.json`, `src/adapters/adapter_descriptor.hpp`). This cleanup does not change it. |
| Protocol | The client sends `protocol_version` 2 and accepts responses with version 1 or 2. `protocol/` is mirrored byte-for-byte with Synthserver. |
| Default endpoint | `http://127.0.0.1:8087/Synthserver/` (`config/SYNTH.ini` `[Server] BaseUrl`) |

The public repositories are `Dwemer-Dynamics/SYNTH` and `Dwemer-Dynamics/Synthserver`.
Start all work from `alpha` and target every PR at `alpha`. `synth` is the main/release branch;
`dev` and `unstable` are reserved lanes. See [CONTRIBUTING.md](../CONTRIBUTING.md).
The public client source comes from private cleanup `618e924`; the paired server comes from
`c12fb1b`. Older hashes and branch names below identify historical development checkpoints.

## Supported runtimes

All other runtime builds fail closed.

| Lane | DLL | Game | Script extender | Also required | Engine library pin |
|---|---|---|---|---|---|
| Flat | `SYNTH.dll` | Fallout 4 `1.11.240` | F4SE `0.7.9` | Address Library for F4SE Plugins (1.11.240) | `libxse/CommonLibF4` `6266ecc9` |
| VR | `SYNTHVR.dll` | Fallout 4 VR `1.2.72` | F4SEVR `0.6.21` | VR Address Library `v1.13.1` | `ArthurHub/CommonLibF4VR` `1c7b4fc8` |

F4SE Menu Framework is optional and works on the flat lane only. The VR lane is configured
through `SYNTH_custom.ini` alone.

## Where things live

- `src/core`, `src/client`, `src/tasks`, `src/protocol_native`, `src/json`, `src/actions` and the
  other engine-free folders hold the shared core: protocol, queues, media, conversation and
  typed actions.
- `src/adapters`, `src/flat` and `src/vr` hold the runtime adapters. All `RE::`/`REL::`/F4SE access
  stays behind `IFalloutRuntime` and runs on the game thread.
- `scripts/papyrus/` holds the original Papyrus sources. `scripts/build_gameplay.py` generates
  `SYNTH.esp` deterministically. `packaging/` and `tools/audit_release_tree.py` hold the release
  layout and its audit.
- `protocol/` holds the versioned JSON schemas and fixtures shared with Synthserver.
- `tests/` holds native unit suites, build gates and packaging tests.

## Build and test

See [README](../README.md#build-and-test) for the commands. Points to know:

- The flat package needs compiled Papyrus in `out/wait-papyrus` (always) and in `out/papyrus`
  (for `SYNTHNative`, when present). Both are built with hash-pinned Caprica v0.1.5 against
  **your own** Fallout 4 binary script imports, which this repository does not contain. The
  native build scripts only check the Papyrus source. `tests/packaging/test_release_audit.py`
  builds a real flat package, so it fails on a host without `out/wait-papyrus`.
- **CI policy (chosen 2026-10-06):** hosted CI builds and tests source only. Release
  packaging is a mandatory local step on a host with the compiled `out/wait-papyrus/`:
  run `tests/packaging/test_release_audit.py`, build both packages and audit both archives.
  The hosted `windows-native` jobs build each DLL and run its native suites; they no longer
  package. The hosted `contracts` job keeps the portable C++ suites, `verify_protocol.py`
  and the build gates, and no longer runs the packaging tests. Synthserver CI never
  packaged and keeps its tracked source-tree safety audit. Do not weaken the audit or commit
  a fabricated PEX.
- Previous hosted results (client `5342870`, before this policy): the flat DLL built and its
  native suites passed, then flat packaging failed because `out/wait-papyrus/SYNTHWait.pex`
  is not provisioned on hosted runners. The VR job built, tested, packaged and audited
  successfully. `contracts` passed its native suites, protocol check and build gates, and
  failed only in the packaging tests for the same missing PEX. Server CI passed at
  `d171ec7`. The revised workflows have not run on hosted CI yet.
- Locally, the static-md VR build, all 17 native tests and the real package audit pass, and
  packaging passed with the pinned compiler and the user's own game imports.
- `scripts/verify_protocol.py --peer ../Synthserver/protocol` checks the mirror against a sibling
  server checkout.
- Synthserver's `unittests/tests/AutoGreetingRuntimeTest.php` reads this repository through
  `SYNTH_CLIENT_ROOT` (default `../SYNTH`). If this repository is missing, the test is skipped.

## Gameplay records and save compatibility

- `SYNTH.esp` is flat only. It contains original wait and restraint records with
  `Fallout4.esm` as its master. Never renumber or remove existing FormIDs, quest aliases,
  PACK/REFR records or the local-800 migration record. Saves refer to them.
- Wait uses Papyrus `SetRestrained` with a game-thread check of the actor's life state.
  Aliases 0–255 track wait subjects. Aliases 256–511 record only the restraint that SYNTH
  applied. Old package and marker records remain for save compatibility only. See
  [WAIT-ALIAS-CHAT.md](WAIT-ALIAS-CHAT.md).
- Commit `231bbdb` (PKDT interrupt-override fix) and commit `f34da88` (restraint hold) must stay
  together.

## Behavior in this baseline that is not yet accepted in-game

- `cc883ac`: the chat character counter is hidden. The input limit is unchanged.
- `a842fd6`: typed chat, push-to-talk and open microphone target the nearest eligible
  listener when no NPC is aimed at.
- `231bbdb` and `f34da88`: the wait package classification is corrected, and the NPC is held
  with owned restraint (flat only).
- Earlier features (queue parity, lipsync, native subtitles, facing/menu pause, inventory
  actions) also have open manual acceptance. See [ALPHA-ACCEPTANCE.md](ALPHA-ACCEPTANCE.md).

## Known gaps and deferred work

- **No flat in-game or VR in-headset acceptance has been performed** for this baseline.
- **VR runtime DLL imports (corrected in this cleanup).** The baseline `scripts/build-vr.ps1`
  used the dynamic `x64-windows` vcpkg triplet, so the baseline `SYNTHVR.dll` imported
  `spdlog.dll` and `fmt.dll`, which the VR package does not ship. The script, the CMake gate
  messages and the build gate tests now use `x64-windows-static-md` (same ports and baseline,
  static libraries, dynamic MSVC runtime). A rebuilt static-md `SYNTHVR.dll` passed the native
  suite (17/17), and its PE import table lists no `spdlog.dll` or `fmt.dll`. A VR package built
  from that DLL passed the release audit locally. The hosted VR job at `5342870` also built,
  tested and audited it. In-headset load remains open.
- The flat `SYNTH.dll` compiles in libxse/CommonLibF4, which is GPL-3.0-or-later with the
  Modding and Linking Exceptions since upstream `0d96d54`. Distributing that DLL carries GPL
  obligations. See [PUBLICATION-CHECKLIST.md](PUBLICATION-CHECKLIST.md).
- `PickupItem` is disabled. Native approach, transfer and cleanup are unproven. See
  [PICKUP-ACTION.md](PICKUP-ACTION.md).
- VR has no wait/restraint controller and no Menu Framework pages. VR presentation, input,
  lipsync and subtitles each need their own headset acceptance. Flat offsets must never be
  reused in VR.
- Scope amendment of 2026-09-08: companion affinity is excluded. Server TTS scope is XTTS,
  Chatterbox, PocketTTS, Cartesia and Inworld.
- Server native embedding-space work is deferred and is **not** alpha-ready. It lives on
  Synthserver `codex/dialectic-parity-core` at `3808838`. Its migration IDs
  `20260908001`–`006` sort below the baseline's `20260909001`, so they must be renumbered
  before that work resumes. See the Synthserver handoff.
- The flat package audit verifies only SYNTH-owned ESP/PEX bytes. A Script VM call or record
  behavior is proven only by in-game evidence.

## Retired ledgers

The 2026-10-06 cleanup removed `docs/DIALECTIC-PARITY-GOAL.md` (the chronological parity goal
ledger), `CLAUDEX-TASK.md` (a superseded task assignment) and `CONTINUATION-NOTES.md` (a July
checkpoint). They remain in Git history at `f34da88` and in a private archive. The ledger
recorded workstation-specific deployment receipts, hashes and backup paths. It was not a current
specification. Older documents that cite it for "exact hashes" or "remaining gates" now point
here. The current open requirements are the gaps above plus the
[alpha acceptance checklist](ALPHA-ACCEPTANCE.md).

Older local source snapshots were reconciled against Git history and contain no unique source.
Private crash dumps and ad-hoc diagnostic scratch are intentionally not imported.

## Historical receipts and placeholders

Feature docs keep dated deployment and validation receipts as historical evidence. Their dates,
commit hashes, file hashes, runtime versions, record IDs and the fixed server path
`/var/www/html/Synthserver` are original. Workstation-specific locations were replaced with
these placeholders:

| Placeholder | Meaning |
|---|---|
| `<client-worktree>`, `<server-worktree>` | The local checkout used for that receipt |
| `<clean-audit-checkout>` | A clean detached checkout used for a CI reproduction |
| `<MO2-mods>` | The Mod Organizer 2 `mods` folder of the test modlist |
| `<Fallout4-install>` | The Fallout 4 game folder |
| `<wsl-distro>` | The WSL distribution that hosted Synthserver |
| `<scratch>` | Ad-hoc probe scripts and scratch output, retained outside the repository |
| `<private-backup>`, `<server-backup>` | Pre-deployment backups (Windows and WSL), retained privately |

The probe scripts and backups named in those receipts are external evidence. They are not part
of the repository and are not needed to build or test SYNTH.

## Baseline evidence

These checks ran on the baseline commits, not on the cleanup head. They are not in-game proof.

- Windows flat and VR DLLs built. All 17 native suites passed in each lane.
- Python packaging tests (13) and build gates (49) passed.
- Protocol: 33 schemas, 202 fixture cases and 215 mirrored files passed.
- Both real lane archives passed the release audit. That audit does not inspect PE imports:
  the baseline `SYNTHVR.dll` imported `spdlog.dll` and `fmt.dll` (see the known gaps).

## Licensing and publication

See [PUBLICATION-CHECKLIST.md](PUBLICATION-CHECKLIST.md). SYNTH's own code is licensed under
the GNU GPL v3.0 (root `LICENSE`, chosen 2026-10-06). Both release packages ship that file,
and the release audit requires its exact bytes. `LICENSES/DIALECTIC-MIT.txt` covers the reused
Dialectic material, and every third-party notice keeps its own terms. History and publication
decisions remain open.
