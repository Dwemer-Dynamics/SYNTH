# `SYNTH.esp` gameplay records

Gameplay records are permitted as normal implementation work. The former initial-release
prohibition and separate authorization phase were removed at the user's request on 2026-09-09.
`scripts/build_gameplay.py` authors the current original wait package; see `WAIT-HERE.md`.
The checklist below concerns future quest/alias/script content, not a blocker on adding records.

## When an ESP is justified

Use `SYNTH.esp` when owned gameplay records are appropriate for the requested feature.
Expected candidates are persistent
quest/alias ownership, custom AI packages, follower conversion, keywords/factions, holotape or
controller items, dialogue scenes/topics, messages/globals, and attached Papyrus receivers.

## Prerequisites

1. A Windows machine with the exact target flat Fallout 4 and Fallout 4 VR runtimes when the ESP
   is intended for both.
2. Matching Steam Fallout 4 Creation Kit installed and launched successfully.
3. Matching vanilla Fallout 4 and F4SE Papyrus source files.
4. Separate flat and VR MO2 development profiles and empty `SYNTH_esp_dev` mod directories.
5. xEdit/FO4Edit for independent record/error inspection.
6. A clean Git branch and a backup of test saves/load order.

Do not copy FNV `Dialectic.esp`, `Herika.esp`, Skyrim scripts, compiled PEX, form IDs, package
bytecode, game assets, or records into Fallout 4.

## Minimal form design

Start with `Fallout4.esm` as the only master unless a requirement explicitly needs a DLC. Use a
stable SYNTH editor-ID prefix and maintain `docs/ESP-FORM-MANIFEST.md` with purpose and ownership.

Candidate forms, added only when required:

- `SYNTH_MainQuest`: start-enabled controller quest with explicit shutdown/version behavior;
- player reference alias and bounded reference-collection aliases for event delivery;
- `SYNTHNative.psc`: declarations for F4SE-registered native functions;
- `SYNTHController.psc`: thin event/quest bridge, no network/model/business logic;
- custom packages for movement/follow/wait/travel only where native primitives are unsafe;
- opt-in/exclusion keywords/factions and explicit messages/globals;
- optional holotape/controller and dialogue content as separate product decisions.

Do not flag ESL merely because it is possible. The initial dual-runtime plugin should be an
ordinary non-light ESP because Fallout 4 VR does not natively share current flat ESL behavior.
Reconsider only after form count, persistent references, update behavior, flat+VR compatibility,
safe form ID allocation, current compatibility tooling, and xEdit validation are understood.

## Papyrus rules

- Papyrus is an event/form adapter; native C++ and Synthserver own the runtime and AI pipeline.
- No polling loops for state already available from F4SE.
- No file-based request/response bridge.
- Every callback is generation-aware and bounded; no long work in Papyrus events.
- Source `.psc` is tracked. Compiled `.pex` is produced on Windows from documented exact inputs and
  included only after the build is reproducible.
- Native registration and script signatures have a contract test/static audit.

## Build procedure to implement later

1. Open the plugin in the flat Fallout 4 Creation Kit and create only reviewed forms. Record the
   exact CK/masters; Fallout 4 VR does not provide an independent current Creation Kit lane.
2. Save to the dedicated MO2 development directory, not the game Data directory.
3. Compile tracked scripts with the matching Fallout 4 Papyrus compiler and import paths.
4. Run a deterministic packaging script that copies only the approved ESP/PEX/assets.
5. Run xEdit Check for Errors in explicit flat and VR load-order contexts and inspect masters,
   header/form ID ranges, overrides, deleted records, injected records, navmesh, persistent
   references, aliases, packages, and archive paths. A version-check bypass alone is not proof that
   a flat-created plugin is safe in VR.
6. Generate a manifest of form IDs/editor IDs, masters, script source hashes, PEX hashes, and
   package contents.

## Acceptance matrix

- New game and existing clean save.
- Flat Fallout 4 and Fallout 4 VR each load the same reviewed ordinary ESP without unsupported
  light-plugin assumptions, base-record collisions, or missing masters/DLC.
- Save/load, death/reload, fast travel, interior/exterior transition, main menu, shutdown.
- Quest starts exactly once; aliases fill/clear deterministically; uninstall/upgrade behavior is
  documented rather than assumed safe.
- No dirty edits, deleted references/navmeshes, unintended masters, missing scripts, or log spam.
- Native-only features still work when the ESP is disabled; ESP-dependent capabilities disappear
  explicitly from the server action catalog.
- Every package/action restores actor state after success, failure, interruption, combat, cell
  unload, and hard halt.
- VR checks include FRIK and no-FRIK profiles, power armor, HMD-effective targeting, controller
  input, and the exact popular compatibility profiles in `FALLOUT4-VR-PLAN.md`.
