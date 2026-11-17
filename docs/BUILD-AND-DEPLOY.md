# Build and deploy quickstart

How to get from a clean checkout to a SYNTH package that installs as a Fallout 4 mod. Details and the
toolchain pins are in [FALLOUT4-TOOLCHAIN.md](FALLOUT4-TOOLCHAIN.md); the packaging rules are in
[PUBLICATION-CHECKLIST.md](PUBLICATION-CHECKLIST.md). A build, a unit test or a package audit is never
gameplay evidence: flat and VR acceptance are recorded separately in
[ALPHA-ACCEPTANCE.md](ALPHA-ACCEPTANCE.md).

## Tools

- Windows x64, Visual Studio 2022 with the C++ workload (C++23), Git.
- Python 3.
- xmake for the flat lane; CMake with vcpkg (`VCPKG_ROOT`) for the VR lane.
- For packages only: [Caprica v0.1.5](https://github.com/Orvid/Caprica/releases/tag/v0.1.5) and a folder of
  your own compiled Fallout 4 base `.pex` scripts (not shipped).

Keep build output outside the source tree (`out/` is ignored) and use a separate worktree per branch.

## 1. Check without the game

```bash
cmake -S . -B out/ci-core -DSYNTH_BUILD_CORE_TESTS=ON -DSYNTH_BUILD_VR=OFF
cmake --build out/ci-core && ctest --test-dir out/ci-core --output-on-failure
python3 scripts/verify_protocol.py          # add --peer ../Synthserver/protocol for the mirror check
python3 tests/build/test_build_gates.py
```

## 2. Compile the Papyrus script (packages only)

```powershell
python scripts/build_wait_script.py --compiler <path-to>/Caprica.exe --imports <path-to-pex-imports>
```

## 3. Build the DLL

```powershell
./scripts/build-flat.ps1 -Mode releasedbg                     # SYNTH.dll, runs the native suites
./scripts/build-vr.ps1 -Configuration RelWithDebInfo          # SYNTHVR.dll
```

## 4. Package

```powershell
./scripts/package-flat.ps1 -Dll build/windows/x64/releasedbg/SYNTH.dll
./scripts/package-vr.ps1 -Dll <path-to>/SYNTHVR.dll
python tools/audit_release_tree.py --lane flat --path out/packages/SYNTH-FO4.zip
python tools/audit_release_tree.py --lane vr --path out/packages/SYNTH-FO4VR.zip
```

Packages are `SYNTH-FO4.zip` (flat) and `SYNTH-FO4VR.zip` (VR), each with the DLL,
`Data/F4SE/Plugins/SYNTH.ini`, licenses and manifests. Package from the DLL you just built, never from
a stale build directory.

## 5. Install (local test)

1. Install the package as a mod (for example a Mod Organizer 2 mod, or extract into `Data/`).
2. Required: F4SE `0.7.9` and Address Library for 1.11.240 (flat), or F4SEVR `0.6.21` and VR Address
   Library `v1.13.1` (VR). The plugin refuses other builds.
3. Put your settings in `SYNTH_custom.ini` next to `SYNTH.ini`; never put API keys in either file.
4. Start Synthserver (default `http://127.0.0.1:8087/Synthserver/`, see the Synthserver README). Match
   `[Server] BaseUrl` if you use the standalone launcher on port `8085`.

## What this does not prove

Hosted CI builds and tests source only and does not package. Say what you did not run (in-game flat,
in-headset VR) in every PR description.
