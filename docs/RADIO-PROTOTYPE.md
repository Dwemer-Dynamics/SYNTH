# Native radio diagnostic prototype

This optional prototype builds original radio records, a flat runtime diagnostic
bridge and A/B/A test recordings. The isolated flat test confirms quest startup,
station discovery at 106.9 and audible slot A and B playback. Reuse of an already
played file remains unverified. It does not insert generated news into Diamond
City Radio. The purpose is to establish native file reuse before connecting the
public-news service.

## Boundaries

The separate `SYNTHRadioDiagnostics.esp` contains twelve original records with
`Fallout4.esm` as its only master and no overrides. The ordinary `SYNTH.esp` wait
records and release allowlist are unchanged. Both transmitter and receiver are
initially disabled. The quest and scenes do not start automatically. The native
probe is off until explicitly enabled and is disabled again at every runtime
generation change.

Flat access is limited to Fallout 4 **1.11.240** behind the game-thread
`IFalloutRuntime` adapter. VM callbacks exchange scalar requests and copied JSON;
they do not resolve game objects. Only one command can be pending. No network,
provider generation, filesystem scan, media rewrite or audio decoding runs in
the radio game-thread pump. VR's radio seam returns unsupported; no flat offsets
or radio implementation are used in VR.

The radio actions play our own SNDR descriptors through the native scene system.
This does not use SYNTH's separate XAudio2 dialogue output. Existing DCR scenes
are observed read-only for later boundary investigation.

Fallout 4 scene records must be inside the owning quest's type-10 Quest Children
group, within the top-level QUST group. A top-level SCEN group is not a valid
FO4 placement and caused the test station's record validation to fail before
its transmitter could be enabled. The builder now uses quest-child groups with
the same existing FormIDs and scene fields. Structural tests verify the group
ancestry. Native receipts identify the individual missing or invalid record
while retaining all validation checks. Startup and slot B playback subsequently
passed on the same flat test save.

A subsequent saved-game run still reported slot A absent from the global form
map. Resolution now also checks the validated owning quest's scene list: only
one exact matching FormID and SCEN type is accepted, within a 256-entry limit,
and all parent/action/alias/phase/sound checks remain required. A wrong-type
global form is rejected. If neither list contains the scene, the bounded receipt
reports owned quest scene/file counts and promotion state plus a vanilla scene
type as a control. This distinguishes an unloaded quest child from a registry
lookup issue; no quest is reset or promoted by the native probe.

## Reproducible build

Run from the client checkout with the pinned toolchain and the user's own binary
Papyrus imports. Replace the placeholders with local paths. These commands create
build output; they do not install it.

```powershell
python scripts/prepare_radio_media.py --output out/radio-prototype/media
python scripts/build_radio_gameplay.py --output out/radio-prototype/records/SYNTHRadioDiagnostics.esp
python scripts/build_radio_papyrus.py --compiler <Caprica.exe> --imports <owned-game-imports> --output out/radio-papyrus
xmake f -y -p windows -a x64 -m releasedbg --build_flat=y --commonlibf4_dir=<pinned-CommonLibF4> --menuframework_dir=<pinned-menu-headers>
xmake build SYNTH
xmake build synth-radio-tests
xmake run synth-radio-tests
python -m unittest discover -s tests/radio -p 'test_*.py'
python scripts/build_radio_bundle.py --dll build/windows/x64/releasedbg/SYNTH.dll --papyrus out/radio-papyrus --media out/radio-prototype/media --output out/radio-prototype/bundle
python scripts/build_radio_bundle.py --audit out/radio-prototype/bundle
```

The media and optional bundle outputs must be new directories. The pinned
Caprica v0.1.5 compiler builds both original scripts twice in isolated temporary
directories. Their canonical outputs must match; source/compiler/import hashes
are recorded. Compiler timestamps, usernames and machine names are removed from
owned PEX headers. Imported game/SDK scripts are never published.

The optional bundle contains its own ESP, two owned PEX/source files, test media,
the rebuilt flat SYNTH.dll, notices, source and SHA-256 manifests. Its closed
allowlist rejects additional scripts/plugins, links, changed source/record/media
bytes and unsupported DLL architecture. The replacement A clip stays under
`Replacement/`, outside `Data/`; it is not automatically installed or swapped.
It is an optional extension to the normal flat package, not a complete setup.

## Diagnostic station and controls

After a separately authorized installation into a disposable test profile, use
the original quest's console functions. These are instructions for future manual
acceptance, not steps performed during the build.

```text
cqf SYNTH_RadioDiagnostics EnableProbe
cqf SYNTH_RadioDiagnostics PlayA
cqf SYNTH_RadioDiagnostics Observe
cqf SYNTH_RadioDiagnostics PlayB
cqf SYNTH_RadioDiagnostics StopProbe
cqf SYNTH_RadioDiagnostics DisableProbe
```

The transmitter alias is optional and initially empty, matching vanilla
script-filled aliases and SYNTH's working wait controller. Enable calls the
latent quest `Start()` function when needed and checks its result and running
state. It then fills only an empty alias 0 with the exact owned transmitter and
verifies the assignment before queuing an enable request. A different existing
reference is rejected. The generation is checked after latent startup so an old
request cannot assign an alias after a save change. A failed start or alias
check produces an on-screen notification and Papyrus trace. `StartProbe` performs
only that startup and owned-alias assignment; `ShowStatus` reports
running/starting/stopping/stopped and alias readiness
without starting, stopping or resetting anything. Close the console to let the
game run and display the notification. A queued message confirms command
admission only; the native receipt still determines whether enabling succeeded.
The revised startup removes mandatory engine alias filling from `Start()`.
A flat test on the same save verified successful startup, the exact transmitter
alias, loaded owned scenes and native enable success. The preview was audible
when tuned to Trinity Tower because the original 99.5 frequency collided with
its vanilla transmitter. The diagnostic frequency is now **106.9**, checked
against the test profile's transmitter and receiver fields. The flat test
confirmed discovery under the **SYNTH Radio Diagnostics** name, manual tuning
to 106.9 and audible slot B playback. A subsequent same-session baseline also
confirmed that A and B play and finish normally. This verifies fixed previews; the
A/B/A rewrite and cache-refresh experiment below remains open. Player tuning
is always manual. A play command is rejected while either owned scene or the selected
listener's native sound is busy, or its state is uncertain. The corrected stop
candidate stops both owned scenes and releases their real listener handles through
the radio manager. Disable additionally disables our transmitter and receiver.

Every processed command logs one copied `synth.radio.probe.v1` JSON receipt in
SYNTH's native log. Observe requests a fresh receipt. The native
`SYNTHRadioProbeNative.Observation()` method also returns the latest copied JSON
for an original Papyrus caller. Receipts distinguish player tuning, scene state,
phase, raw action flags and their low-two-bit status. The action's `sound_id` is
a descriptor FormID, not an audio handle. Ending an action resets that ID and its
duration and completion fields; these fields alone do not establish silence.
The corrected native candidate queries the engine's locked radio manager using
the exact owned scene and transmitter. It reports `native_descriptor_id`, its
expected owned ID, consistency and ownership, `sound_busy` (pending, playing or
paused), and `sound_paused`. These describe the selected listener, not every world
receiver. `sound_playing` is null because the busy query cannot distinguish strictly
playing audio. `native_duration_milliseconds` names the audited native unit;
`handle_duration_units` remains as a legacy alias. The corrected candidate still
needs an in-game observation test. No receipt grants permission to rewrite live
media or declares audible cache refresh proven.

For a physical receiver test, place **only our owned receiver** in an ordinary
test location through an explicit console/reference operation. Its plugin-local
reference is `000805`; resolve the actual load-order ID first. The internal
`SYNTHRadioDiagnosticCell` stores original references and contains no floor or
arrival geometry. Do not teleport the player into it. Receiver positioning,
attenuation and native station timeline remain game tests.

## A/B/A reuse experiment

The default recordings are quiet original test tones: A is 440 Hz for two seconds,
B is 660 Hz for three seconds, and replacement A is 880 Hz for five seconds.
The replacement has a different hash and duration but the same eventual engine
path as A. Custom owned PCM16 mono clips can be supplied to the media builder.

1. Enable and manually tune to the diagnostic station. Play A, then B after A's
   native playback ends. Record audible output and diagnostic receipts.
2. Once A is independently confirmed idle, replace its media only as part of an
   explicitly authorized controlled test. The offline builder does not perform
   this operation, and scene inactivity alone is not proof of silence.
3. Play A again. Confirm that the already played slot now emits the changed pitch
   and five-second duration. Playing a previously unseen filename is insufficient.
4. Repeat tuning off/on, tuning away/back, Pip-Boy open/closed, owned world receiver,
   interrupted playback and save reload. Record actual sound termination separately
   from scene termination. An earlier save must discard queued future commands and
   require a fresh enable.

The corrected stop candidate releases real listener handles through the engine's
write-locked operation for only the two exact owned scene keys, including entries
remaining after scene completion. It never constructs an audio handle from the
action's descriptor ID. Release is asynchronous: the result is `stop_requested`,
not an assertion of silence. Before a controlled file replacement, independently
confirm audible completion, capture a during-playback witness from the corrected
manager query, explicitly stop both owned scene keys, and require fresh consistent
idle observations. Restored scene/audio state after load still needs explicit
observation; resetting the core mailbox alone does not prove sound release.

## Evidence and acceptance

The binary format was checked against primary xEdit definitions for
[FO4 scenes, activators and placed radio data](https://github.com/TES5Edit/TES5Edit/blob/dev-4.1.6/Core/wbDefinitionsFO4.pas)
and [standard descriptor sound filename fields](https://github.com/TES5Edit/TES5Edit/blob/dev-4.1.6/Core/wbDefinitionsCommon.pas).
Installed master identifier/field metadata was inspected read-only to verify the
radio category and referenced mesh path. No master records, music, dialogue,
proprietary scripts or meshes were copied. All generated gameplay records and
diagnostic code are original project source.

Offline/native tests cover closed record ownership, non-looping radio actions,
standard ANAM sound paths, disabled records, stale commands, bounded mailbox,
worker-thread rejection, generation cancellation, unsupported VR and receipt
distinctions. Build success cannot establish any item below.

- [x] Static native slots A and B are audible through the diagnostic station on the
      tested flat save, with manual tuning to 106.9.
- [ ] Radio menu volume, tuning and menu behavior match native radio expectations.
- [ ] Already played A returns changed bytes and correct duration after replacement.
- [ ] Scene stopping and actual audio stopping agree without an orphan.
- [ ] The owned world receiver follows a coherent station timeline and attenuation.
- [ ] Save reload discards future commands and restored sound ownership is understood.
- [ ] A real DCR interlude and clean vanilla handoff are independently established.
- [ ] Radio-mod compatibility is assessed in separate disposable profiles.

Production DCR insertion, generated host voice, public-news delivery and listener
exposure integration are subsequent work. No in-game result or VR radio support
is implied by this prototype.
