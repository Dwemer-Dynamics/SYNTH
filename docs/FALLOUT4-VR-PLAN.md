# Fallout 4 VR support plan

> **Historical plan (2026-07-18).** Kept for VR design rationale. The current VR runtime pins,
> gaps and acceptance status are in [HANDOFF.md](HANDOFF.md) and
> [ALPHA-ACCEPTANCE.md](ALPHA-ACCEPTANCE.md) (2026-10-06).

Research date: 2026-07-18

## Support decision

Fallout 4 VR is a first-class SYNTH target. It is not a best-effort use of the flat Fallout 4 DLL.
The project will ship and test two native artifacts built from one product codebase:

| Lane | Executable | Script extender | Native artifact | Engine library |
| --- | --- | --- | --- | --- |
| Flat Fallout 4 | supported Steam/GOG `Fallout4.exe` lane | matching F4SE | `SYNTH.dll` | pinned `libxse/CommonLibF4` |
| Fallout 4 VR | Steam `Fallout4VR.exe` `1.2.72` | F4SEVR `0.6.21` | `SYNTHVR.dll` | pinned `ArthurHub/CommonLibF4VR` |

Both artifacts use the same protocol, server, configuration vocabulary, conversation engine,
queues, memory behavior, and action definitions. They have different runtime adapters, dependency
pins, compatible-version declarations, packages, and acceptance matrices. A flat F4SE DLL is never
advertised as VR-compatible merely because both processes are x64.

The VR dependency pin candidates inspected for this plan are:

- `ArthurHub/CommonLibF4VR@1c7b4fc860261eabad9f044e336965c26abe8ee6`;
- VR Address Library release `v1.13.1`, tag commit
  `8f8d65e5941c17c88576e7b1185f26ea337f068f`;
- FRIK design/API reference `rollingrock/Fallout-4-VR-Body@81239d99663bde447480b8098c1b400ef250d798`;
- CHIM dual-runtime design reference
  `Dwemer-Dynamics/CHIM@77c73ffb6bb32c226340bbda93b3aac5a7ad49f8`.

Revalidate all pins immediately before implementation. Do not copy GPL FRIK or F4VR Common
Framework code into SYNTH without an explicit client-license decision. CommonLibF4VR and the VR
Address Library repository are MIT at the inspected revisions; retain all required notices.

## What CHIM teaches us

The inspected CHIM runtime does more than switch addresses. Its VR paths establish requirements
that SYNTH must reproduce for Fallout 4 VR:

- branch by the detected runtime rather than a user INI guess;
- use the HMD/player-camera node for listener position, view direction, pitch, and target geometry;
- do not use the potentially parked or offset player reference as the VR player's effective
  position;
- handle controller input as VR actions/devices rather than assuming Windows virtual-key codes;
- move facial/viseme mutations through a game-thread update pump when the ordinary flat hook is not
  safe in VR;
- skip incompatible flat-only hooks and report the resulting capability honestly;
- throttle per-frame spatial/nav work more aggressively in VR;
- treat VR body/hand mods as optional runtime interfaces with readiness and version checks;
- use distinct dialogue-menu/Scaleform assets where the VR UI differs.

SYNTH will keep these principles while implementing original Fallout 4-specific adapters. Skyrim
addresses, node names, input IDs, hooks, and HIGGS behavior are not portable to Fallout 4 VR.

## Code architecture

```text
                       synth_core (engine-free C++23)
                 protocol, config, tasks, queues, media,
                 conversations, action types, test fakes
                            /                 \
                           /                   \
          flat_f4_adapter /                     \ f4vr_adapter
          CommonLibF4                            CommonLibF4VR
          F4SE runtime lanes                     F4SEVR 0.6.21
          SYNTH.dll                              SYNTHVR.dll
```

`synth_core` must not include `RE::*`, `REL::*`, F4SE, OpenVR, FRIK, Windows input, or raw game
headers. It operates on immutable product-owned snapshots and typed action requests.

Two implementations of `IFalloutRuntime` own every engine-specific operation:

- `FlatFalloutRuntime`: current flat Fallout 4 F4SE/CommonLibF4 implementation;
- `FalloutVrRuntime`: F4SEVR/CommonLibF4VR implementation, HMD/hand/controller/UI adaptations.

Runtime-owned snapshots add:

- `runtime_variant`: `flat` or `vr`;
- exact executable, script-extender, address-library, adapter, and plugin versions;
- HMD world transform and freshness;
- left/right controller transforms, handedness, and input capability flags;
- optional FRIK API version/readiness and VR body/UI state;
- active VR runtime (`steamvr_openvr`, `opencomposite_openxr`, or `unknown`) when detectable without
  invasive probing;
- presentation and action capability bits derived from actual initialized systems.

No HMD, controller, node, actor, form, or CommonLib pointer may escape the adapter or cross a worker
thread. Copy on the game thread and attach a frame/time freshness marker.

## Build and dependency strategy

### Flat target

- Continue with pinned `libxse/CommonLibF4` and the selected flat F4SE runtime lane.
- Build `SYNTH.dll` and reject VR at plugin query/version load.

### VR target

- Build `SYNTHVR.dll` against pinned `ArthurHub/CommonLibF4VR` using its VR-only configuration.
- Require F4SEVR `0.6.21`, Fallout 4 VR runtime `1.2.72`, and the pinned-compatible VR Address
  Library data at runtime.
- Start with CMake/MSVC/vcpkg for the VR target if that is the proven path in CommonLibF4VR and
  FRIK. It is acceptable for the flat target to retain XMake while both invoke a shared core and
  common tests. Build-system uniformity is less important than a reproducible supported ABI.
- Run `vr_address_tools` as an audit aid against VR adapter relocations. Its automated mappings are
  candidates, not proof; each relocation/hook still needs source, disassembly, or in-game evidence.
- Avoid a direct dependency on the GPL `F4VR-CommonFramework` unless the project deliberately
  adopts compatible licensing. Its architecture is a reference, not a shortcut.

### Packages

Produce independent archives:

```text
SYNTH-FO4-<version>.zip
  Data/F4SE/Plugins/SYNTH.dll
  Data/F4SE/Plugins/SYNTH.ini
  Data/F4SE/Plugins/SYNTH/...approved interface assets

SYNTH-FO4VR-<version>.zip
  Data/F4SE/Plugins/SYNTHVR.dll
  Data/F4SE/Plugins/SYNTH.ini
  Data/F4SE/Plugins/SYNTH/...approved VR interface assets
```

Each package manifest records the exact target and dependencies. Each DLL rejects the other
runtime. Release audit fails if both DLLs appear in one archive, if flat Address Library data is
bundled into the VR archive, or if third-party F4SE/VR binaries are redistributed without explicit
permission.

## VR player experience

The Dialectic parity experience remains the target in VR, with these adaptations:

### Targeting and conversation

- Head-gaze/raycast targeting from the HMD is the default VR target path.
- Crosshair/selected reference is accepted only when it corresponds to the current HMD ray or a
  user-confirmed target.
- Optional controller pointing selects an NPC using a hand ray and explicit confirm action.
- Nearest eligible actor and automatic nearby activation use HMD-effective player position.
- Group audience, distance, line of sight, and spatial voice use HMD position, not the player root.
- Seated, room-scale, height-adjusted, left-handed, and power-armor states receive dedicated tests.

### Input

- Typed text remains available as a desktop/debug fallback, not the primary VR interaction.
- Push-to-talk binds to a semantic VR action, with keyboard fallback for development.
- Controller family/handedness is resolved at runtime. Do not persist raw device indices or assume
  Oculus Touch, Index, WMR, or Vive button numbers are interchangeable.
- Open-mic VAD remains disabled by default and must suspend during loading, system overlays, menus,
  Synthserver loss, and hard halt.
- Haptics for accepted target, recording start/stop, response ready, and failure are optional and
  individually configurable.

### Audio and presentation

- The listener pose follows the fresh HMD transform. NPC sources remain attached to copied actor
  positions and update through bounded game-thread snapshots.
- Prove spatial audio with head rotation, room-scale translation, seated mode, interiors,
  exteriors, and active HRTF/spatial-audio mods.
- Subtitle/UI work must be readable in-headset and must not blindly replace dialogue-menu SWFs.
  Provide a VR-specific asset or non-invasive native overlay path, capability-gated when another UI
  mod owns the surface.
- Facing targets the effective HMD/player point with comfort limits; it must not force the player
  camera or rotate the VR body.
- Lipsync/visemes use a VR-safe game-thread pump if ordinary flat hooks are unavailable. A failed
  viseme capability must not block voice playback.

### Performance and comfort

- No synchronous HTTP, decoding, database, or model work on the game/render thread.
- VR periodic context scans use separate frequency and entity budgets from flat mode.
- Measure frame-time cost at idle, during target scans, during streaming, with group dialogue, and
  in dense settlements/combat. Record p50/p95/p99 adapter time and dropped/deferred work.
- Backpressure drops/coalesces stale low-priority snapshots before it delays a current frame.
- No forced camera movement, artificial head rotation, unexpected locomotion, or unrequested
  haptics. Movement actions act on NPCs, never the player's VR pose.

## Popular setup compatibility targets

SYNTH must work in a clean profile and coexist with the stacks players actually use. These are
acceptance profiles, not dependencies to bundle.

### Profile A: minimal supported VR

- Steam Fallout 4 VR `1.2.72`;
- F4SEVR `0.6.21`, launched through a clean Mod Organizer 2 instance;
- VR Address Library;
- SYNTHVR and no body/hand overhaul.

This is the debugging baseline and proves that FRIK, Fallout4 VR Tools, Buffout, Wabbajack, and an
ESP are not hard requirements for conversation.

### Profile B: modern FRIK stack

- Profile A plus current FRIK;
- current compatible Buffout 4 NG/VR and xSE PluginPreloader when the tester normally uses them;
- optional Fallout4 VR Tools if required by the installed profile;
- common performance/OpenVR or OpenXR compatibility layers documented by the tester.

SYNTH may use FRIK's versioned C API only through an optional adapter after license review. Useful
capabilities include skeleton readiness, fingertip position, handedness-relative hand selection,
Pip-Boy/config visibility, and configuration-menu integration. Missing/old FRIK falls back to
native HMD/controller behavior. SYNTH never manipulates the FRIK skeleton directly.

### Profile C: Mad God's Overhaul

Test the current Fallout 4 VR Mad God's Overhaul Wabbajack release (`2.0.2` at research time) in a
cloned MO2 profile without reordering or overwriting its files. This exercises a large modern mod
stack, DLC content, whichever body/input/UI/hook systems that exact list contains, performance
pressure, and real load-order/plugin inventory. Record the exact modlist version, whether FRIK is
present, and local deviations; do not make SYNTH support depend on an unpinned moving list.

### Profile D: Fallout VR Essentials/Idle Hands lineage

Test a representative Fallout VR Essentials Overhaul or compatible Idle Hands profile so SYNTH
does not accidentally require FRIK nodes or APIs. Dialogue, input, audio, targeting, and halt must
work with the alternate/no-body setup. If the historical list can no longer be reproduced, freeze
a small documented compatibility profile containing only the relevant public dependencies.

### Profile E: conservative vanilla-like guide

Use a version-pinned profile based on Florine's Fallout 4 VR guide to validate the common
MO2/F4SEVR/VR Address Library/Fallout4 VR Tools/Buffout/FRIK combination without a total gameplay
overhaul. The guide is research input; verify each installed current version independently.

### Profile F: F4FEVR content-heavy compatibility

When its current Wabbajack can be reproduced, test an F4FEVR profile because it deliberately makes
flat Fallout 4 DLC and complex content such as Sim Settlements 2 and Point Lookout work in VR. This
is valuable load-order, world-context, quest, actor identity, and performance coverage. SYNTH must
remain neutral to its gameplay changes and must discover DLC/plugins rather than assuming them.

## Third-party mod support policy

“Fallout 4 mod support” does not mean every flat mod is safe in VR:

- Native F4SE DLLs are ABI-specific and require an explicit VR build. SYNTH ships its own VR DLL
  and does not bypass another DLL's compatibility declaration.
- Content plugins and assets may work, but newer flat plugins can depend on records, header/form ID
  behavior, BA2 formats, DLC, Creation Club, or engine features absent from VR. SYNTH neither
  rewrites third-party plugins nor tells users that version-check bypasses prove compatibility.
- FO4VR has no bundled DLC. Large VR setups commonly copy legitimately owned flat Fallout 4 DLC
  into the VR profile plus compatibility patches. SYNTH's base conversation feature must not
  require DLC; DLC/world context is discovered from the actual load order.
- No feature uses a hardcoded load-order index. Store form ID plus origin plugin identity and
  resolve it against the current load order.
- Detect and log loaded plugin/runtime capability summaries with redaction. Maintain a small
  compatibility registry only for observed conflicts that SYNTH can safely avoid; do not silently
  patch unrelated mods.
- Coexist with FRIK, Idle Hands, Fallout4 VR Tools, Buffout, HRTF/spatial audio, VR UI mods,
  OpenComposite/OpenXR translation, SteamVR, MO2, and Wabbajack without taking ownership of their
  configuration.

The later SYNTH ESP must use an ordinary non-light plugin first, avoid an ESL dependency for the VR
lane, use safe form ID allocation, and pass a full flat+VR xEdit/load matrix. See
`ESP-DEFERRED-SETUP.md`.

## VR feature and acceptance matrix

| Area | Required automated proof | Required in-headset proof |
| --- | --- | --- |
| Plugin load | VR version export, runtime rejection, dependency diagnostics | F4SEVR log on exact `1.2.72` setup |
| HMD snapshot | fake transforms, freshness/stale rejection, thread ownership | head rotation, room-scale, seated, height offsets |
| Targeting | ray/LOS/angle/confirm tests | gaze and both-hand pointing across controller profiles |
| PTT/VAD | semantic action state machine, cancellation, no raw-index persistence | Touch/Index plus one WMR/Vive or documented unavailable lane |
| Group/spatial | HMD-relative distance/audience/audio math | several NPCs while moving/turning in room scale |
| UI/subtitles | capability and ownership arbitration | readable VR display with Pip-Boy/dialogue/UI mods |
| TTS/lipsync | queue, cancellation, VR game-thread pump fake | correct speaker/audio/visemes; audio survives viseme disable |
| Actions | same typed fake-runtime catalog as flat | per-action VR matrix with menus, combat and locomotion |
| Lifecycle | generation cancellation on load/new game/menu/shutdown | save/load, new game, power armor, cell transitions |
| Performance | bounded queues and synthetic dense snapshots | frame-time captures in minimal and heavy modlist profiles |
| Compatibility | fake optional-interface version/readiness paths | Profiles A-E, with exact modlist/dependency versions |
| Recovery | server/audio/input dependency fault injection | WSL/server/SteamVR/controller loss and safe recovery |

No row may inherit `IN-GAME PROVEN` from flat Fallout 4. VR proof is independently required.

## Server and protocol effects

Synthserver keeps one Fallout 4 world/memory product but receives:

- `game: "fo4"`;
- `runtime_variant: "flat" | "vr"`;
- exact client/runtime versions;
- input, presentation, context, action, FRIK, and VR runtime capabilities;
- optional sanitized mod-environment fingerprint for compatibility evidence.

The server stores runtime variant with sessions/playthroughs and filters actions by capabilities.
Memory is not automatically split between flat and VR when canonical player/playthrough identity is
the same, but rollback/save identity must prevent accidental cross-profile mutation. The management
UI displays runtime lane and disables unavailable configuration rather than pretending parity.

## Primary/reference sources

- F4SE/F4SEVR runtime table: https://f4se.silverlock.org/
- CommonLibF4VR: https://github.com/ArthurHub/CommonLibF4VR
- VR Address Library: https://github.com/alandtse/fallout_vr_address_library
- VR address audit tools: https://github.com/alandtse/vr_address_tools
- Current FRIK source and C API: https://github.com/rollingrock/Fallout-4-VR-Body
- Fallout4 VR Tools API: https://github.com/lfrazer/FO4VRTools
- Mad God's Overhaul: https://github.com/Moyse06/MadGodsOverhaul
- F4FEVR: https://github.com/ajantaju/F4FEVR
- Fallout VR Essentials listing: https://github.com/wabbajack-tools/mod-lists
- Florine's Fallout 4 VR guide: https://github.com/FWDekker/fo4vr-modlist
