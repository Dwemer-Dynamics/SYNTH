# Fallout 4 flat picked-reference binding audit

Date: 2026-09-06. Status: guarded observer and selection implemented; in-game acceptance pending.

## Implementation checkpoint

The flat adapter now forwards the exact typed observer after checking the runtime
and original slot. A revision/epoch-fenced atomic mailbox copies only its raw
activation handle. Every publication changes revision, including repeated values;
an older callback cannot pass an A-to-B-to-A compare/exchange. Load/retirement/menu
boundaries clear it. The callback never resolves a reference or calls the dispatcher.

The game-pump capture resolves the handle, checks a loaded same-playthrough actor,
and reserves its immutable snapshot independently of nearest-64 discovery. A
concurrent observation/load change rejects the captured pick. Existing enrichment,
capture batching and explicit API requests preserve the picked identity.

Flat manual selection now requires that exact captured pick. The prior nearest
fallback in PluginSession is disabled for manual flat requests; automatic background
activation retains its own eligibility/distance fallback. Empty, unavailable,
non-actor, out-of-range and rejected actor picks do not select a substitute.
VR controller/HMD paths remain separate. There is no new wire field or dependency.

Controlled checks: targeting/mailbox110, adapter285 and actual observer header23
(stubbed relocation/original handler, not a game). Both native builds and package/
deployment evidence are recorded in the retired parity ledger ([handoff](HANDOFF.md#retired-ledgers)). Subsequent client9a089f2
bounds flat discovery to256 process-list handles plus64 fresh cached resolutions;
incomplete observations stay partial and exact picks remain independent. VR discovery
is still exhaustive. Manual stationary/moving/obstructed pick, menu/save/load,
text/voice, long-session and VR acceptance remain required.

## Outcome

The next targeting implementation can use Fallout's ViewCaster activation-picked
reference instead of geometric selection over the nearest 64 actors. Do not truncate
the actor scan until explicit selection is independent of discovery. No runtime
offset is authorized for VR by this audit.

At the start of the audit, installed client3c01cb1 still used geometric flat selection.
This audit supersedes the earlier statement that no concrete gameplay-pick binding
had been found. It does not prove event cadence, picking distance, occlusion or
successful hook delivery in a running SYNTH session.

## Primary source trail

- [PCL callback, commit 00fb6c4](https://github.com/LucaDotGit/PapyrusCommonLibraryF4/blob/00fb6c4f3457e16f04cf2ab85fabbfbc323d86b9/src/Internal/Callbacks/CrosshairRefHandler.cpp)
  consumes ViewCasterUpdateEvent and retains its activation-picked handle, clearing
  it when the optional value is absent. Its pinned CommonLib revision is
  71ee347ab938bccf6712b25f6909e53b5f95e653, not current main.
- [Pinned event declarations](https://github.com/LucaDotGit/CommonLibF4/blob/71ee347ab938bccf6712b25f6909e53b5f95e653/include/RE/Bethesda/Events.hpp)
  distinguish activation, magnetism, telekinesis and dialogue handles. Its old/NG
  singleton IDs are absent from the installed AE address database; do not copy them.
- [Current library IDs, commit e337150](https://github.com/LucaDotGit/CommonLibF4/blob/e337150ac993daaa9d089fcddf7a6ce9a6bad42f/include/RE/IDs.hpp)
  supplies the separate Anniversary Edition singleton ID 4801601.
- [Current event layout](https://github.com/LucaDotGit/CommonLibF4/blob/e337150ac993daaa9d089fcddf7a6ce9a6bad42f/include/RE/V/ViewCasterUpdateEvent.hpp)
  and its linked data declarations preserve the optional 0x38-byte data payload.

Both source licenses were read: PCL MIT copyright LucaDotGit 2025, CommonLib MIT
copyright ryan-rsm-mckenzie 2019. No third-party implementation was copied this turn.
Any later reuse must retain the relevant notices. No dependency was updated.

## Exact local evidence

Read-only PE/address-library inspection used these existing files; neither was run
or modified. PE means Windows Portable Executable; RVA is relative virtual address.

- `<Fallout4-install>/Fallout4.exe`, version 1.11.240.0,
  image base 0x140000000, SHA256
  fdcef37ac1230af6d0b0050eb2142b139ef3a867b37b9211fb6edfcc646072f8.
- `<MO2-mods>/Address Library for F4SE Plugins/F4SE/Plugins/version-1-11-240-0.bin`,
  652306 records, SHA256
  65985cc2259384a13cffb538d74776e422e62b0cd3766485a000620242b72b06.
- SYNTH pinned CommonLibF4 6266ecc9014b473fc6b6efd04abac324477c63cd and
  commonlib-shared c4379b2082a464f99c45618f79b920daf6d15dd3.
- Scratch reader: `<scratch>/viewcaster-binary-audit.py`.
  It validates version, V0 mapping length, PE sections and selected RIP-relative
  cross-references; objdump supplied the subsequent instruction-level review.

| Meaning | Address Library ID | Verified RVA |
| --- | --- | --- |
| AE global event-source pointer | 4801601 | 0x3273868, writable .data |
| Global ViewCaster event-source vtable | 4824240 | 0x252bc08, .rdata |
| Existing typed value-event sink vtable | 736650 | 0x2532ac0, .rdata |
| Old/NG singleton IDs | 1536643 / 2694310 | Both absent |

The typed sink vtable's second slot resolves to RVA 0xA11BC0. That function forwards
the event to RVA 0xA11960 and returns continue. The latter checks the event-present
byte at +0x38, copies four 32-bit fields at +0/+4/+8/+0xC, two pointers at +0x10/+0x18,
a 32-bit field at +0x20, a byte at +0x28, and the optional life-state at +0x2C/+0x30.
This independently agrees with the published activation-handle-at-zero layout.

RVA 0xA0C090 initializes the singleton's static storage and installs the vtable
resolved by ID 4824240. Engine subscription/unsubscription adds +0x10 for the
BSTEventSource base. RVA 0x999E10 destroys and clears the singleton; stale cached
source pointers cannot be assumed safe during shutdown. The native value sink has
its own lock, so SYNTH must not introduce a callback/pump lock dependency.

## Original implementation and acceptance gates

1. Follow the existing flat rest-observer pattern: review a guarded forwarding hook
   on the exact typed sink's ProcessEvent slot. Require supported runtime and the
   exact expected native function; refuse a changed slot instead of replacing
   another mod's observer chain. Do not create a global source during save loading.
2. Copy only event presence and the raw activation handle into an epoch-fenced,
   nonblocking latest-value mailbox. Preserve native forwarding and return value.
   Do not resolve actors, allocate scene data, log per event, call providers or
   enter the runtime dispatcher from the callback. Invalidate on load/menu/retirement.
3. On the authorized game-pump lane, resolve the handle and shallow-capture that
   exact loaded actor. Preserve canonical plugin/base/playthrough ownership and
   same-frame/generation checks. Non-actors, missing handles and unavailable
   observation must never choose a nearby substitute.
4. Carry picked-reference observation separately from the discovery audience in
   immutable snapshots. Reserve the exact target even outside the nearest 64,
   preserving the audience's truthful partial quality. Explicit API targets remain
   explicit; VR controller/HMD selection remains independently implemented.
5. Route ordinary flat selection through that captured identity, then implement
   bounded/staged discovery without changing the selected actor. Do not market
   a first-N scan or stale nearest list as parity.
6. Validate empty/non-actor picks, hook refusal/forwarding, epoch races, callback
   pressure, actor changes/reuse, target outside audience, text/voice ownership,
   and both native builds. Manual tests must cover stationary targets, moving
   between targets, obstructed targets, menus, save/reload and long sessions.

No fixed event-expiry interval is justified yet: establish whether a stationary
pick is republished or only changes produce events before relying on timestamps.
Static layout proof cannot establish that runtime delivery contract.
