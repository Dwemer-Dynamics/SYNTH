# Papyrus dialogue bridge: verified build and packaging

The separate flat-only persistent wait controller is documented in
[WAIT-ALIAS-CHAT.md](WAIT-ALIAS-CHAT.md). It uses original executable `SYNTHWait.psc`/PEX,
not additions to the six-function native-only dialogue bridge described here. Build it explicitly
with `scripts/build_wait_script.py` before flat packaging; game imports are never distributed.

This is not yet script-runtime-ready. The DLL bindings and original
`scripts/papyrus/SYNTHNative.psc` now have a reproducibly compiled and structurally
verified `SYNTHNative.pex` in ignored build output. The original bridge is packaged
and locally installed for flat; the parity ledger records the exact hashes.
Compilation and installation do not prove VM registration or in-game calls.

## Reproducible native-only build

`scripts/build_papyrus.py` pins the official [Caprica v0.1.5 compiler](https://github.com/Orvid/Caprica/releases/tag/v0.1.5)
by SHA256. It accepts a local directory of real Fallout 4 binary imports; it does
not download, alter, synthesize or distribute game scripts. The original source
explicitly extends ScriptObject, avoiding compiler-specific implicit-parent behavior.
The tracked minimal SYNTHNative.flg declares only the script's Hidden flag.

```powershell
python scripts/build_papyrus.py --compiler C:\path\to\Caprica.exe --imports C:\path\to\binary-imports
python scripts/verify_papyrus_pex.py out/papyrus/SYNTHNative.pex
```

The importer directory must contain only bounded, flat Fallout 4 PEX 3.9 files.
The verified local set contains 29 F4SE 0.7.9 scripts plus 32 missing vanilla types
from the user's installed Fallout4 - Misc.ba2. These imports stay outside Git and
are never packaged. The output manifest records every import hash, compiler hash,
source/flags hashes and PEX hash, without machine-local paths.

Two isolated compiles must produce identical canonical output. The inspector checks
the entire native-only binary: Fallout 4 3.9/game ID 2, exact script/parent, Hidden
flag, six matching Global Native signatures, one empty state, no properties,
variables, structs, executable instructions or trailing bytes. Only compilation
time, source path and compiler user/machine metadata are normalized; object data
is not rewritten. Fatal compiler text or missing/malformed output fails even when
the compiler incorrectly returns exit code zero. Language/speculative extensions
and debug output are disabled. This is not a general-purpose PEX validator.

The normal DLL builds still run only the source/native contract audit. This separate
build must be invoked explicitly with its locally owned dependency inputs.

## Packaging and local deployment

The normal package commands automatically include a present `out/papyrus` build.
An incomplete, stale or malformed build fails packaging; it is not silently omitted.
Without that directory, DLL-only packaging remains supported. Python packaging also
accepts an explicit `--papyrus-build` directory. The release audit validates the full
binary and its source/flags/compiler provenance independently of the ZIP manifest.

Only these four files are permitted as the native script bundle:

- `Data/Scripts/SYNTHNative.pex`
- `Data/Scripts/Source/User/SYNTHNative.psc`
- `Papyrus/SYNTHNative.flg`
- `Papyrus/build-manifest.json`

The local SYNTH deployment helper re-audits the extracted package, verifies the
installed PEX hash and refuses to remove an existing bridge when a build is missing.
It installs the Data files only; the build receipt/flags stay in the audited archive.
Additional original gameplay content is allowed with corresponding build and release auditing.
The flat release now includes the independently audited `SYNTH.esp` wait package. Imported
game scripts and unreviewed artifacts are not included.
Flat and VR archives pass the same structural checks; VR VM/headset compatibility
does not follow from that shared file format. No game or Creation Kit launch is needed
for these build/package/deploy steps.

The bridge itself defines no ESP forms, quests, aliases, polling loops or file
transport. It exposes six global functions through the existing native queue:

| Function | Arguments | Result |
| --- | --- | --- |
| `IsAvailable` | None | Bool: queue is open, not a guarantee of execution. |
| `SpeakExact` | Int actor FormID, String text | Int admission status. |
| `Comment` | Int actor FormID | Int admission status. |
| `React` | Int actor FormID, String direction | Int admission status. |
| `Ask` | Int actor FormID, String question | Int admission status. |
| `OpenPrompt` | Int actor FormID | Int admission status; flat presenter only, VR execution rejected. |

Statuses match the native API: 0 queued, 1 invalid, 2 unavailable, 3 stale epoch,
4 busy queue. Queued does not mean executed, heard or persisted. Do not retry an
accepted request automatically. All exact-target, age, lifecycle, scene, combat,
cooldown, one-shot and same-owner Ask gates in `docs/NATIVE-EVENT-API.md` apply.

## Value-only boundary

Callers obtain the intended live actor's current `GetFormID()` through Papyrus and
pass that Int. Signed high-bit values preserve their full 32-bit FormID pattern.
Never save/reuse a raw FormID across load boundaries. The callback takes no actor
pointer, reference handle, arbitrary URL, action name or script to execute.

The pinned CommonLib marshaller supplies primitive values/string views. The
callback bounds text to 1000 bytes, rejects embedded NUL, captures the queue's
current epoch and copies into the shared nonblocking queue before returning.
The borrowed string is not retained. VM/tasklet callbacks do no game-object
lookup, networking, filesystem work, model work or audio work. The ordinary safe
game pump resolves the exact actor later. The script does not encode a 64-bit
epoch in Papyrus Int; an epoch transition during submission can still reject it.

Native bindings register through each runtime's pinned F4SE Papyrus interface.
Registration failure is optional and does not disable existing native dialogue.
The callback logs whether all six methods bound; a DLL build does not prove that
this callback actually ran in Fallout.

## Completion gate

1. COMPLETED FOR BUILD: pinned Caprica v0.1.5 plus exact local binary imports.
   Bethesda's compiler remains unavailable in the checked install/PATH locations.
2. COMPLETED FOR BUILD: compile the original declarations twice, validate and
   anonymize the full native-only binary, and record exact input/output hashes.
   Do not ship imported game/F4SE scripts or treat compiler success as gameplay proof.
3. COMPLETED FOR PACKAGING: only the verified owned PEX and its source/provenance
   bundle are permitted. The ledger records installed hashes separately; do not
   infer VR acceptance from flat deployment.
4. Manually confirm registration and all five request paths using the exact actor.
   Include same-owner interruption, cross-owner rejection, busy one-shot rejection,
   high-bit/dynamic references, save/load, menus and halt. Correlate native logs.

Both build scripts run `scripts/verify_papyrus.py` to compare the original source
declarations with the actual C++ callback signatures and registration names.
That audit deliberately reports its source-only scope. OpenPrompt
has a native flat UI bridge and packaged script declaration; actual VM invocation
and gameplay remain unverified. Its VR presenter is unfinished. Follower APIs remain separate
work. Additional gameplay records and scripts follow the ordinary ownership/build audit.
