# Fallout 4 and F4SE toolchain

> **Partly historical (2026-10-06 note).** Statements that the package excludes `SYNTH.esp` or
> PEX files are superseded. The flat package now ships original SYNTH-authored ESP/PEX content.
> The README has the canonical build commands, and [HANDOFF.md](HANDOFF.md) has the current status.

Research refreshed: 2026-08-31. Revalidate versions before every Windows release.

## Supported platform facts

- Fallout 4 is a 64-bit Windows process; SYNTH must build x64, unlike Dialectic's x86 FNV plugin.
- F4SE officially supports current Steam and GOG builds. Its current readme excludes Microsoft
  Store/Game Pass and Epic, and the F4SE site separately lists VR.
- The F4SE site currently lists runtime `1.11.240` with F4SE build `0.7.9`, runtime `1.10.984`
  with `0.7.2`, and runtime `1.10.163` with `0.6.23`.
- The same official site lists the independently frozen Fallout 4 VR runtime `1.2.72` with
  F4SEVR `0.6.21`. A flat F4SE plugin is not binary-compatible with F4SEVR.
- All native plugins needed updates for the 1.11 structure/runtime line. SYNTH must not advertise
  broad compatibility until the exact address and structure strategy is proven.
- The Fallout 4 Creation Kit is available through Steam under Software. It is not required for the
  initial no-ESP native plugin.

## Windows developer machine

Required:

1. Windows 11 or supported Windows 10.
2. Steam or GOG Fallout 4 for the flat lane and Steam Fallout 4 VR for the VR lane, isolated in
   separate validation profiles and pinned against surprise updates during a cycle.
3. F4SE `0.7.9` launched through `f4se_loader.exe` for flat runtime `1.11.240`; F4SEVR `0.6.21` launched through
   `f4sevr_loader.exe` for VR.
4. Visual Studio 2022 with Desktop development with C++, MSVC x64, and Windows SDK.
5. Git with submodule support.
6. XMake 3.0 or newer for the flat CommonLibF4 lane and CMake/vcpkg versions proven by the pinned
   CommonLibF4VR lane.
7. C++23-capable MSVC or Clang-CL.
8. Separate MO2 development profiles with `SYNTH_dev` and `SYNTHVR_dev` mod directories.
9. VR Address Library for the VR runtime; a connected headset/runtime for in-headset proof.

Record before testing:

```text
Fallout4.exe file/product version
F4SE build and core DLL filename
F4SE loader filename and SHA-256
SYNTH commit and package SHA-256
CommonLibF4 commit
CommonLibF4VR commit and VR Address Library release/hash
Fallout4VR.exe, F4SEVR loader/core, headset runtime, and controller profile when testing VR
Installed mod-manager profile and load order
```

## Dependency choice

Use two explicit dependency lanes:

- flat: pinned `libxse/CommonLibF4`, whose current documentation requires XMake 3.0+ and C++23 and
  states it replaces F4SE as a static dependency while F4SE remains required at runtime;
- VR: pinned `ArthurHub/CommonLibF4VR`, F4SEVR `0.6.21`, and VR Address Library. The inspected VR
  fork explicitly split from upstream because current `libxse/CommonLibF4` does not support VR.

Keep all library/game types behind the corresponding `IFalloutRuntime` implementation so the rest
of SYNTH is engine-free and a dependency migration remains bounded. Run the VR address audit tools
on the adapter, but never treat an automated address match as runtime proof.

Do not blindly copy the `commonlibf4-template`, FRIK, or F4VR Common Framework build/source trees:
the inspected versions are GPL-3.0. Create original SYNTH rules or consciously adopt compatible
licensing. CommonLibF4, CommonLibF4VR, and the VR Address Library repository are MIT at the
inspected revisions.

## Proposed repository/build layout

```text
SYNTH/
  xmake.lua
  CMakeLists.txt                     # VR target if required by the proven VR dependency lane
  lib/commonlibf4/                 # pinned submodule
  lib/commonlibf4vr/               # independent pinned VR submodule
  src/
    core/                          # no RE/REL/F4SE/VR headers
    flat/main.cpp                  # flat plugin version/load only
    vr/main.cpp                    # VR plugin version/load only
    runtime/IFalloutRuntime.*
    runtime/FlatFalloutRuntime.*   # sole flat game/F4SE/CommonLib boundary
    runtime/FalloutVrRuntime.*     # sole VR game/F4SEVR/HMD/controller boundary
    runtime/RuntimeGeneration.*
    runtime/RuntimeSnapshot.*
    runtime/GameThreadDispatcher.*
    tasks/TaskManager.*
    transport/HttpClient.*
    transport/Protocol.*
    dialogue/
    context/
    actions/
    audio/
    input/
  tests/
  fixtures/protocol/
  Mod/Data/F4SE/Plugins/SYNTH.ini
  scripts/build.ps1
  scripts/test.ps1
  scripts/package.ps1
  tools/audit-release-tree.ps1
```

Expected independent release outputs:

```text
SYNTH-FO4-<version>.zip
  Data/F4SE/Plugins/SYNTH.dll
  Data/F4SE/Plugins/SYNTH.ini
  Data/Interface/SYNTH/...         # only if the flat UI experiment needs it
  README.txt
  LICENSES/...
  manifest.json

SYNTH-FO4VR-<version>.zip
  Data/F4SE/Plugins/SYNTHVR.dll
  Data/F4SE/Plugins/SYNTH.ini
  Data/Interface/SYNTH/...         # only reviewed VR-compatible assets
  README.txt
  LICENSES/...
  manifest.json
```

No `SYNTH.esp`, Papyrus PEX, BA2, game DLL, F4SE binary, game voice, or runtime-generated file is
allowed in the initial package.

## Build and test commands to implement

```powershell
git submodule update --init --recursive
xmake f -m releasedbg
xmake build
xmake run synth-tests
powershell -ExecutionPolicy Bypass -File .\scripts\build-vr.ps1 -Configuration RelWithDebInfo
powershell -ExecutionPolicy Bypass -File .\tools\audit-release-tree.ps1
powershell -ExecutionPolicy Bypass -File .\scripts\package.ps1
```

The final exact commands may change with the original `xmake.lua`, but CI, docs, and local scripts
must use one canonical path.

## F4SE plugin contract

- Export valid target-specific plugin version data and `F4SEPlugin_Load` using the selected
  library's supported macro/API. `SYNTH.dll` rejects VR and `SYNTHVR.dll` rejects flat runtimes.
- Declare a precise compatible runtime or valid Address Library/structure-independence flags.
- Register for F4SE lifecycle messages before performing runtime work.
- Acquire messaging/task/Papyrus/Scaleform/serialization interfaces only when needed and check
  interface versions.
- Initialize on `GameDataReady`/load events; invalidate on pre-load/new game/menu teardown; join
  workers at shutdown.
- Put save-persistent data in F4SE serialization only if it cannot safely be reconstructed. Version
  every record and resolve forms/handles during load.
- Never do network or blocking media work inside a lifecycle callback.
- On VR, snapshot HMD/controllers/nodes only through verified VR layouts and move game mutations
  through the VR-safe game-thread dispatcher. Do not reuse flat player-camera assumptions.

## Windows CI

Use a private GitHub Actions Windows runner initially with independent flat and VR jobs:

- clean checkout with recursive submodules;
- pinned XMake plus pinned VR CMake/vcpkg setup;
- x64 Debug and Release/RelWithDebInfo builds for both artifacts;
- native unit/integration tests that do not load Fallout;
- no hosted packaging: the flat package needs Papyrus compiled against the user's own game
  script imports, so both release packages are built and audited locally (policy chosen
  2026-10-06);
- no game/F4SE binaries or secrets in CI;
- dependency and source SHA in the manifest.

CI proves that source compiles; it does not prove that either DLL loads or behaves in its game.

## Windows in-game acceptance matrix

At minimum test flat Fallout 4 as listed below, then execute the independent VR matrix in
`FALLOUT4-VR-PLAN.md`. No flat result is inherited by VR.

Flat minimum:

1. Plugin load and version rejection on the exact supported/unsupported runtime pair.
2. Main menu -> new game, existing save, repeated load, death/reload, and return to main menu.
3. Interior/exterior cells, fast travel, workshop/settlement, power armor, dialogue, Pip-Boy,
   barter/container, combat, pause, alt-tab, and shutdown.
4. Crosshair and nearby actor identity, disabled/dead actors, companions, creatures, robots,
   settlers, hostile actors, unloaded references, and cell transitions.
5. Server absent/start/restart/timeout/malformed response, then cancellation and reconnection.
6. Text/PTT/open-mic, subtitles, multiple speakers, overlapping TTS, interruption, and hard halt.
7. Every enabled native action with exact pre/post evidence.
8. At least a two-hour soak with bounded queues, frame-time telemetry, no stale-generation work,
   no leaked threads, and no crash.

VR minimum additionally includes F4SEVR load, HMD-effective targeting/audio, semantic controller
input and handedness, in-headset UI/subtitles, optional FRIK present/absent, room-scale/seated/power
armor, minimal and heavy modlist profiles, and VR frame-time captures.

## Creation Kit and Papyrus

Not required for the initial run. When authorized, install the Steam Fallout 4 Creation Kit aligned
with the game, obtain the matching vanilla and F4SE Papyrus source files, compile only tracked
SYNTH `.psc` sources, and follow `ESP-DEFERRED-SETUP.md`. Do not use a Skyrim or FNV compiler,
headers, records, form IDs, or compiled scripts.

## Licensing/release gate

The checked F4SE and F4SEVR readmes say native plugin source must be publicly available. Private
development can continue, but do not distribute `SYNTH.dll` or `SYNTHVR.dll` until the owner
reviews the current script-extender terms, GPL/MIT/LGPL boundaries for any optional integration,
copied notices, and the required source-publication plan. This document is an engineering flag,
not legal advice.
