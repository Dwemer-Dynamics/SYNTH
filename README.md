# SYNTH

SYNTH is the Fallout 4 and Fallout 4 VR client for AI-driven NPC conversation. It consists of
two native 64-bit plugins, F4SE `SYNTH.dll` and F4SEVR `SYNTHVR.dll`, built from one
engine-free shared core. Both talk to [Synthserver](https://github.com/Dwemer-Dynamics/SynthServer/tree/alpha).

## Development branches

**Start all work from `alpha` and target every pull request at `alpha`.** This is the SYNTH
exception to the other Dwemer mods' unstable-first workflow. `synth` is the default branch;
`unstable` and `dev` are reserved testing lanes, and `synth` is the main/release branch.
Promotions require maintainer approval; branch names do not imply gameplay acceptance.

```powershell
git clone --branch alpha https://github.com/Dwemer-Dynamics/SYNTH.git
cd SYNTH
git switch -c feature/my-change
```

Read [CONTRIBUTING.md](CONTRIBUTING.md) and [docs/HANDOFF.md](docs/HANDOFF.md).
GitHub may preselect the default `synth` branch for a new PR: change its base to `alpha`.
The repository's PR branch-policy check rejects any other base.

## Status

Open-alpha preparation. Source builds, native unit suites, protocol fixtures and local package
audits pass for both lanes. **Flat in-game and VR in-headset acceptance have not been performed.**
See [docs/ALPHA-ACCEPTANCE.md](docs/ALPHA-ACCEPTANCE.md) and [docs/HANDOFF.md](docs/HANDOFF.md).

## Supported runtimes

The plugin refuses to run on any other build.

| Lane | Package | Game | Script extender | Also install |
|---|---|---|---|---|
| Flat | `SYNTH-FO4.zip` | Fallout 4 `1.11.240` | F4SE `0.7.9` | Address Library for F4SE Plugins (1.11.240) |
| VR | `SYNTH-FO4VR.zip` | Fallout 4 VR `1.2.72` | F4SEVR `0.6.21` | VR Address Library `v1.13.1` |

Optional on the flat lane: **F4SE Menu Framework** adds native SYNTH pages (Hotkeys, Auto
Activate, Behavior, Sound, Tools). SYNTH ships no MCM config. Without the framework, and always
in VR, everything is configured through the INI.

## Package contents

- Both lanes: the DLL, `Data/F4SE/Plugins/SYNTH.ini`, the project `LICENSE`, license notices,
  `THIRD_PARTY_NOTICES.md`, a README, and the dependency and file manifests.
- Flat only: the original `Data/SYNTH.esp` (Wait Here / Release Wait records) and the
  compiled `SYNTHWait` script with its source.
- When built, both lanes also include the original `SYNTHNative` Papyrus bridge with its source.

No Bethesda, Creation Club, F4SE or other third-party plugin, script, archive or binary is
shipped.

## Configuration

`SYNTH.ini` holds the shipped defaults. Put your changes in `SYNTH_custom.ini` in the same
folder. Git ignores it, and the in-game pages write to it.

- `[Server] BaseUrl=http://127.0.0.1:8087/Synthserver/` is the default. `AllowRemote=false`
  keeps the client on loopback.
- `[Hotkeys]` takes DirectInput scan codes. All hotkeys are unbound by default.
- `[Input]`, `[OpenMic]`, `[Audio]`, `[Behavior]`, `[AutoActivate]` and `[Distance]` control
  speech input, playback, pause, combat and NPC selection.

Never put API keys or other secrets in either INI file. Provider keys belong in Synthserver.

## Server

Synthserver normally runs under Apache in WSL on `127.0.0.1:8087`, which matches the client
default. Its standalone PHP development launcher defaults to port `8085`. If you use the
launcher, set `BaseUrl` to match. Setup is described in the Synthserver README.

## Build and test

Windows x64 with Visual Studio 2022 (C++23), Python 3, xmake (flat) and CMake with vcpkg (VR).
See [docs/FALLOUT4-TOOLCHAIN.md](docs/FALLOUT4-TOOLCHAIN.md).

The flat package and the packaging tests need the compiled original `SYNTHWait` script in
`out/wait-papyrus/` (`SYNTHWait.pex` and `wait-build-manifest.json`). The native build scripts
only check the Papyrus source; they do not compile it. Compile it first. This needs:

- hash-pinned [Caprica v0.1.5](https://github.com/Orvid/Caprica/releases/tag/v0.1.5)
  (SHA-256 `75d1f8acb87f5b5dc0b4694ef2cdea7fe0e0beeadd858a2fe8e059889201bfe9`, checked by
  the script), and
- a folder with the compiled flat Fallout 4 base scripts (`.pex`) that `SYNTHWait` imports,
  taken from your own game install. This repository does not include them.

```powershell
python scripts/build_wait_script.py --compiler <path-to>/Caprica.exe --imports <path-to-pex-imports>
python scripts/build_papyrus.py --compiler <path-to>/Caprica.exe --imports <path-to-pex-imports>  # optional SYNTHNative bridge
./scripts/build-flat.ps1 -Mode releasedbg                       # fetches pinned CommonLibF4, runs native suites
./scripts/build-vr.ps1 -Configuration RelWithDebInfo            # needs VCPKG_ROOT; pinned CommonLibF4VR
./scripts/package-flat.ps1 -Dll build/windows/x64/releasedbg/SYNTH.dll
./scripts/package-vr.ps1 -Dll <path-to>/SYNTHVR.dll
python tools/audit_release_tree.py --lane flat --path out/packages/SYNTH-FO4.zip
python tools/audit_release_tree.py --lane vr --path out/packages/SYNTH-FO4VR.zip
```

The VR script builds its vcpkg dependencies with the `x64-windows-static-md` triplet, so spdlog
and fmt are linked into `SYNTHVR.dll`. Neither package ships third-party DLLs, and the release
audit rejects any extra DLL. Each package carries the license notices for what its DLL links
(see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).

These checks run without Fallout or game files:

```bash
cmake -S . -B out/ci-core -DSYNTH_BUILD_CORE_TESTS=ON -DSYNTH_BUILD_VR=OFF
cmake --build out/ci-core && ctest --test-dir out/ci-core --output-on-failure
python3 scripts/verify_protocol.py            # add --peer ../Synthserver/protocol for the mirror check
python3 tests/build/test_build_gates.py
```

`python3 tests/packaging/test_release_audit.py` also runs without the game, but it builds a real
flat package, so it needs the compiled `out/wait-papyrus/` from the step above. On a clean host
without that output it fails; that is expected, not a portable check.

Hosted CI builds and tests source only: the portable job runs the checks above, and the
Windows jobs build each DLL and run its native suites. Hosted CI does not package. Release
packaging is a mandatory local step: run the packaging tests, build both packages and audit
them on a host with the compiled `out/wait-papyrus/` (see
[docs/ALPHA-ACCEPTANCE.md](docs/ALPHA-ACCEPTANCE.md)).

## Documents

- [Developer handoff](docs/HANDOFF.md) and [alpha acceptance](docs/ALPHA-ACCEPTANCE.md)
- [Native architecture](docs/ARCHITECTURE.md) and [client/server protocol](docs/PROTOCOL.md)
- [Fallout 4/F4SE toolchain](docs/FALLOUT4-TOOLCHAIN.md) and [gameplay records](docs/ESP-DEFERRED-SETUP.md)
- [Feature parity matrix](docs/FEATURE-PARITY-MATRIX.md)
- Historical: [program plan](docs/PROGRAM-PLAN.md), [VR plan](docs/FALLOUT4-VR-PLAN.md),
  [reference stack](docs/REFERENCE-STACK-DATAFLOW.md)

## Contributing and license

See [CONTRIBUTING.md](CONTRIBUTING.md).

Project code is licensed under the [GNU GPL v3.0](LICENSE).
Third-party components retain their original licenses and notices.
Bethesda game content is not covered by this license and is not distributed here.

Reused Dialectic material keeps its MIT notice in `LICENSES/DIALECTIC-MIT.txt`. The flat
`SYNTH.dll` compiles in GPL-3.0-or-later CommonLibF4 code. Both release packages ship the root
`LICENSE` with every lane notice. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and
[docs/PUBLICATION-CHECKLIST.md](docs/PUBLICATION-CHECKLIST.md).
