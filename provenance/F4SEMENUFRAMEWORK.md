# F4SE Menu Framework consumer API provenance

SYNTH's in-game settings surface is registered through the official consumer API of the
F4SE Menu Framework. This file records exactly what SYNTH consumes, how it is obtained,
and the open licensing question that must be answered before any of it is redistributed.

## Upstream

| Field | Value |
|---|---|
| Repository | `https://github.com/DCCStudios/F4SEMenuFramework` |
| Description | Fork of SKSE Menu Framework 3 (`QTR-Modding/SKSE-Menu-Framework-3`) for Fallout 4 |
| Pinned revision | `b031040dcb9b89d5b0accf8a4e4c99733f4dd63a` (`master`, committed 2026-09-01) |
| Retrieved | 2026-08-31 |
| Consumed paths | `resources/F4SEMenuFramework.h`, `resources/DIK.h` |
| `resources/F4SEMenuFramework.h` blob | `cd2c592291fb35616dae7287d6905eb4ea97d69e` |

The pin is declared in `cmake/DependencyPins.cmake`
(`SYNTH_MENU_FRAMEWORK_REPOSITORY` / `SYNTH_MENU_FRAMEWORK_REVISION`) and enforced by
`scripts/build-flat.ps1`, which verifies the checked-out revision before configuring the
flat lane.

## What SYNTH uses

Only the header-only consumer API, exactly as the upstream `PLUGIN_DEVELOPMENT_GUIDE.md`
prescribes:

- `F4SEMenuFramework::IsInstalled()` — presence check, resolved with `GetModuleHandleW`.
- `F4SEMenuFramework::SetSection()` / `AddSectionItem()` — the five native SYNTH pages.
- `F4SEMenuFramework::AddWindow()` / `Model::WindowInterface` — the one SYNTH window,
  the native chatbox. SYNTH flips only its own `IsOpen`/`BlockUserInput`; it never calls
  `CloseMenu()` and never owns the framework's main menu.
- `F4SEMenuFramework::Hotkeys::Register/GetBinding/SetBinding/HasConflict` — the
  framework-wide hotkey registry and its conflict handling.
- `F4SEMenuFramework::IsAnyBlockingWindowOpened()` — input suppression while the menu owns
  input.
- `ImGuiMCP::*` — the widget calls used to draw the pages.

SYNTH does **not** use the MCM translation layer. It ships no `Data/MCM/Config/SYNTH`
tree, so it is never listed under "MCM Mod Configs (Legacy)".

## Linkage and redistribution

The consumer header is a header-only dynamic API wrapper. Its inline call sites use
`GetProcAddress` against `F4SEMenuFramework.dll` at runtime. SYNTH therefore:

- links no import library and has no build-time binary dependency on the framework;
- ships **no** part of the framework in either release archive
  (`tools/audit_release_tree.py` rejects any `Data/MCM/...` path, and the archives contain
  only `SYNTH.dll`/`SYNTHVR.dll`, `SYNTH.ini`, licences, README and manifests);
- fetches the two headers into `out/dependencies/` at build time only, which is a
  git-ignored build tree.

## Open licensing item

`DCCStudios/F4SEMenuFramework` publishes **no `LICENSE` file**, and the GitHub API reports
`"license": null` at the pinned revision. The consumer header itself carries no copyright
or licence notice.

The upstream documentation explicitly tells plugin authors to copy and use the consumer
header, but the repository currently contains no formal licence file. SYNTH fetches the
header only into the ignored build-dependency directory and does not redistribute the
header, framework DLL, or framework source. Formal licence clarification remains open
before any of those upstream files are added to a SYNTH release archive.

Upstream also derives from `QTR-Modding/SKSE-Menu-Framework-3` and bundles Dear ImGui
(MIT, `imgui/LICENSE.txt`); neither is consumed by SYNTH.
