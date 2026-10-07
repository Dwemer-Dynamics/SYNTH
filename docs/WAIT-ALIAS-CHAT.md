# Persistent wait ownership and paused-chat targeting

## Current: September 14 direct movement hold

The 12:49 diagnostic run confirmed Preston retained `WorkshopSandboxAtWork7x17`
(`00038C10`) while the wait quest and alias were valid. The marker reported a
different cell. Package assignment was not a reliable movement hold.

Wait now uses Papyrus `SetRestrained(True)`, with game-thread confirmation of the
actor's restrained life state. The existing 90-second unpaused lease still handles
expiry, repeat commands, cancellation, Release Wait, and load recovery. Dialogue
targeting permits restrained actors so chat and Release Wait remain available.

Aliases 0–255 track wait subjects; aliases 256–511 persist only restraint transitions
SYNTH applied. The setter's no-change return does not claim pre-existing restraint.
Release and reset clear only recorded restraint. Old package and marker FormIDs
remain for save compatibility, but aliases no longer inject those packages and
the command no longer moves markers. No actor teleport, AI disable, or speed change.

Manual acceptance still required: Preston while working, bump/comment during hold,
AI reply while held, Release Wait, 90-second expiry, and saving/loading during hold.
The success log is now `movement restraint verified`; the package-era checks below
are historical and do not establish success for this implementation. VR does not
gain a native restraint controller from this flat-only change.

## September 14: correct the combat-only package classification

The 12:09 logs show successful alias assignment but no active HoldPosition package.
The live PACK records incorrectly set PKDT byte 5 to 4. This is **Interrupt Override
= Combat**, not a procedure type. xEdit's FO4 definition identifies it as such, and
the pinned CommonLib TESPackage keeps its procedureType separately from PACKAGE_DATA.
The earlier master examples were combat overrides and were misinterpreted.

All 256 live packages now set Interrupt Override to None (0); PKCU continues to
reference HoldPosition 0001D415. No IDs, quest/alias ownership, marker location,
90-second timer, native code or Papyrus signatures change. The release test asserts
all packages are ordinary packages and rejects the previous combat-override bytes.
Existing saved aliases are cleared by the already-installed load reset.

Reference: https://github.com/TES5Edit/TES5Edit/blob/dev-4.1.6/Core/wbDefinitionsFO4.pas
(PACK / PKDT / Interrupt Override). Runtime acceptance still requires the same save:
Wait Here must log `active HoldPosition verified`, survive a bump/comment and release
after 90 seconds of unpaused gameplay. Compilation/package audit is not game proof.

## September 14: nearby player conversation targeting

Typed chat, push-to-talk and open microphone now select a direct eligible target
first, then the nearest eligible listener regardless of facing. Command/inspection
targeting is unchanged. The native snapshot copies loaded-3D/same-area admission;
no actor pointers cross into the selection policy. Selection scans only the existing
bounded actor snapshot and adds no world traversal, raycast, network request or worker.

Direct selection is bounded by the existing interior/exterior hearing range, doubled
for SHOUT or fixed at 200 units for WHISPER/CLOSE. Unaimed selection uses the existing
automatic hearing radius as a proximity allowance; outside it, positive LOS proof is
required. Native LOS remains unavailable, so the wider spatial-hearing fallback is
deliberately unavailable. Close proximity is not a claim that walls were tested.

Drafts retain exact actor identity, mode/model and whether selection was nearby.
Send/voice completion revalidates that recipient without silently choosing another.
Save/load cancellation and VR pose freshness remain mandatory. No protocol or server
changes are needed. Named-address parsing, narrator fallback and CHIM's full spatial
hearing remain separate work. Manual acceptance must cover chat and voice behind/beside
an NPC, direct override, crowded ties, range/mode limits, actor loss and save replacement.

## September 14: bounded HoldPosition correction

The 10:40 log confirms only alias assignment for Preston, not an active wait package.
The old live-slot Sandbox template is replaced with Fallout4.esm HoldPosition
0001D415, one Location input (index 0/version 1), radius zero. The original PKDT
classification mistake in this change is corrected in the section above.
Read-only master inspection confirmed this public interface and its use by vanilla
Preston hold packages. Only SYNTH's existing 256 packages change; the local-800
migration record, quest aliases and marker identities remain stable. No vanilla
records are copied or edited, and no restraint, actor-value, AI-disable or actor
teleport is used. The historic Sandbox/radius-64 design below is superseded.

Assignment, release and reset force EvaluatePackage(True). A separate scalar-ID
verification checks GetCurrentPackage against the exact owned package and forces
reevaluation only if it was replaced. It never moves the marker. Verification is
serialized, round-robin, at most once per second per NPC, behind commands/releases.
Only verification-state transitions are logged. Alias assignment is no longer
presented as active-package proof: require `active HoldPosition verified`.

An unpaused 90-second clock starts after confirmed assignment. Expiry queues alias
release and revokes in-flight verification; Release Wait remains immediate at the
same bounded VM boundary. A stalled VM can delay physical cleanup, and the existing
30-second watchdog still applies. Save/load revokes old jobs and resets saved aliases
in the next safe world. Reissuing Wait Here starts a new 90-second hold.

Manual acceptance: hold Preston, let him comment, bump him, send a chat, then verify
release at 90 unpaused seconds (menus do not consume the timer). Also test early
Release Wait, two NPCs, reissue, and save/load. Built records/script tests alone do
not prove a forced quest scene yields or the actor visibly remains at the anchor.

Source: SYNTH, `codex/response-flow-parity`, base `01f225ac46b52fa136f9df1d3465e15838c510f2`.
Existing uncommitted response/menu/speech work is preserved. Server unchanged.

## Confirmed regressions

The September 9 log, written through 12:31:09, shows Preston's wait restored at 12:31:02.853,
replaced at 12:31:03.853, then restored again at 12:31:05.855. The previous recovery kept the
command alive but not its package ownership or original anchor. It did not solve gameplay parity.

Normal chat opened without a captured target. Native pause invalidated the crosshair observation,
then Send captured a new scene and required a crosshair target. This produced repeated
`SYNTH text input needs an eligible target` messages in that same log.

## Wait ownership

- Original quest `SYNTH_WaitOwnership` (local 801), priority 255, 256 optional actor aliases.
  Aliases intentionally have no automatic fill type, like the optional empty aliases found in
  Fallout4.esm quest 19955. Native code explicitly starts the script-controlled quest through the VM.
- Each alias owns one original package (locals 900-9FF), linked to its own persistent marker
  (A00-AFF). A new empty STAT 803 and isolated interior CELL 802 hold the original markers.
  There are no edits to vanilla quests, NPC bases, world cells, factions or companion state.
- Near Reference location, radius 64, all wandering/furniture/ambient-conversation inputs disabled.
  Quest aliases keep packages in normal selection after transient bump/dialogue packages end.
  No periodic run-once reinjection, actor restraint, AI-disable, or NPC teleport.
- The marker moves to the NPC's cell and is set to the position captured when the command was issued.
  It does not follow the NPC or re-anchor after speech. Changing cell before assignment rejects it.
- Release clears that actor's alias and evaluates normal packages. Save/load/new-session invalidation
  revokes outstanding operations; a bounded reset clears restored aliases before new assignments.
  Cross-save wait persistence is deliberately not implemented.
- Commands are asynchronous. The action reports queued; the log separately confirms alias assignment
  or release. This is not a claim that an active scene has already yielded control.
- The new original `SYNTHWait.pex` uses engine-supported `ForceRefTo`, `Clear`, `MoveTo`, `SetPosition`,
  and `EvaluatePackage`. Native dispatch and callbacks accept only scalar IDs/tickets/results.
  Papyrus resolves the IDs after checking the current receipt; C++ retains no actor pointer across
  pumps or callbacks. Original typed script signatures remain intact for saved-stack compatibility.
- One script job at a time, process-unique 128-bit random receipts, cancellation after latent operations,
  superseded-command revision checks, and a 30-second unpaused watchdog. Timed-out assignment slots
  are quarantined until reload so a late marker move cannot corrupt a reused slot.
- Local 800 remains for migration only. Its run-once slot is cleared only on exact ownership using the
  existing exact-runtime/address/signature guard; other mods' replacement packages are untouched.

Record layout references: [xEdit FO4 definitions](https://github.com/TES5Edit/TES5Edit/blob/dev-4.1.6/Core/wbDefinitionsFO4.pas).
This is authored-record/source validation, not Creation Kit or game execution proof.

## Chat targeting

The manual chat hotkey captures the selected eligible NPC before requesting the pausing UI. The
existing draft displays `To: <name>` and retains only identity plus session cancellation, not a stale
world snapshot. Send waits until the native pause is removed, recaptures that actor, and validates
FormID, base FormID, source plugins, playthrough, session generation, distance and current eligibility.
It never silently switches to another NPC after opening the draft.

Manual drafts keep normal text admission/interruption rules, distinct from stricter external Ask
requests. Narrator/logging modes remain untargeted; diary commands remain commands. This repairs
the pause handoff, not CHIM's entire target-picker/Everyone/automatic-routing feature set.

The UI skill was used with a bounded local Opus review; it timed out and Codex completed the work.
No game launch or visual/gameplay validation was performed.

## Build and manual acceptance

Build the additional owned script before packaging the flat client:

```powershell
python scripts/build_wait_script.py --compiler <path-to>/Caprica-v0.1.5/Caprica.exe --imports <client-worktree>/out/papyrus-import-probe
```

The pinned compiler runs twice against actual local game/F4SE binary imports; only the original
script and its provenance are packaged. Header/script identity and hashes are checked independently
by release auditing. This is not a general executable PEX semantic validator. Fresh flat packaging
requires the separate wait-script build, just as it requires a built DLL; VR does not package it.
The deployment helper allowlists and hash-checks the new script in addition to SYNTHNative.pex.
The scalar-ID bridge also requires the real `commonpropertiesscript.pex`, `camerashot.pex`, and
`sound.pex` imports because it calls `Game.GetForm`. The September 13 build uses the extended
local import directory `<client-worktree>/out/wait-form-id-imports`.

Manual acceptance after restarting Fallout 4 through the existing MO2 profile:

1. Confirm `alias controller reset complete` and then `alias assignment confirmed` after Wait Here.
2. Bump Preston and allow several ordinary comments. He should return to/hold the original radius.
3. Release during speech; normal AI should resume without the old wait being reapplied.
4. Wait two different NPCs at different places; reissue and release each independently.
5. Aim at Preston, open chat, check the retained name, spend over 30 seconds paused, then Send.
6. Repeat with another NPC and narrator mode. Load/halt during a pending command and verify cleanup.

Game-specific scene priority, pathfinding back to the anchor, and native VM execution remain manual
acceptance requirements. Flat source/build/package evidence does not establish VR gameplay support.

## September 13 load crash: native object argument packing

The 08:40:20 fault from process 46584 (`SYNTH-fault-46584-148410671.log`) records a null read
at Fallout4.exe + `0x211EDD0`, phase `wait Reset dispatch`, on runtime `1.11.240.0`.
The deployed diagnostic DLL was commit `481c927`, SHA-256
`44607A52507209393A333E8CDD598C7826D822404A68D008C1BDBE400D7E61F8`.
Disassembly of that exact DLL at return offset `0x4E180` shows the form argument packer calling
VM vtable byte offset `0xB0` with only the type name and output object. The captured engine
caller at `0x2119990` expects another argument in R9; the nested function reads through null R9
at the fault PC. The pinned CommonLib `CreateObject` overload ordering selects the properties
overload for the two-argument call. Delaying dispatch did not repair this ABI mismatch.

Native Reset/Apply now dispatch to `ResetByFormId`/`ApplyByFormIds`, using signed 32-bit IDs
with their original bit patterns. The script checks the current receipt before resolving forms
with `Game.GetForm` and calling the existing typed helpers. No native `RE::*` object argument
reaches CommonLib's object packer. Old typed signatures remain unchanged for saved stacks;
their current-ticket checks still reject retired work. No vendor header, engine offset, gameplay
record, save, or server change is required.

Manual acceptance must show `Reset scalar dispatch accepted`, then `alias controller reset complete`,
followed by successful dialogue and Wait Here/Release on the same save. A passing build and two
identical script compiles do not establish those in-game results.

## Local deployment receipt - 2026-09-09 (historical)

_Historical receipt, retained as dated evidence. Workstation paths are replaced with
placeholders; see [HANDOFF.md](HANDOFF.md#historical-receipts-and-placeholders)._

- Source: `<client-worktree>`, `codex/response-flow-parity`, base
  `01f225ac46b52fa136f9df1d3465e15838c510f2` plus preserved and current uncommitted work.
- Destination: `<MO2-mods>/SYNTH_dev`, Fallout 4 `1.11.240.0`.
- Client-only helper reported `SUCCESS - SYNTH flat client to MO2` and
  `Deployment completed successfully.` Extracted release audit passed.
- Built/installed DLL SHA-256: `21EC73689E4D2C6E15BBF69E6963C1729ECA28554324A4EBD264F038CA40BEE6`.
- Installed ESP SHA-256: `DF0B8C7D1A2ACBD35B79E8D11D9E17EC8B626CEEF5DD0E4455BF9E35F99E6BD3`.
- Installed SYNTHWait PEX SHA-256: `327EA4FA388D3A14627557A9284AF23E26925955B8931291112538EAC0C4C33A`.
- Existing SYNTHNative PEX unchanged: `56D6D12F82FFC41F1CFA91A2AA465B67466022F7C550B0B4CEBA3E56D11FEF9E`.
- Custom INI preserved: `33615F103948696C045CA9C3894DAC8D6D64ADC7AFBB636C37A37A3DCEA83E5E`.
- Default profile still enables `SYNTH.esp`. No profile, save, game-executable or server changes.
- Prior DLL/ESP/native bridge backup: `<private-backup>/before-alias-wait-chat-20260909-130919`.
- Flat build and all 17 native suites passed, including 667 actions checks and targeted-chat receipt checks.
  All 44 source/build gates, 13 packaging checks, 191 protocol cases, and diff whitespace checks passed.
  Two isolated wait-script compiles produced identical canonical bytes. Tests verify all 256 distinct
  alias/package/marker mappings and stale/revoked/duplicate callback handling.
- Synthserver was not redeployed. Read-only health returned HTTP 200.
- No game launch, in-game acceptance, VR build/gameplay claim, PR, push, release or version bump.
