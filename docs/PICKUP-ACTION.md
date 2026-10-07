# PickupItem implementation and evidence

## Current boundary

Source preparation, frame-state control, flat read-only inspection and whole-inventory
postconditions exist. No PickupItem capability, native dispatch, package installation,
wire receipt or server action issuance is enabled. Do not deploy these components
as a working pickup action.
The full behavioral target includes approaching a distant reference and restoring
owned movement state, not only immediate close-range transfer.

The frozen Dialectic server `907eb634fc49b47abe3490ea8229977ad0b0f6be`,
`functions/functions.php`, requires exact `RefID:ItemName` from nearby observations.
The frozen client `c6e92f375a44a680affc22c8ba378320650b92da`,
`Plugin/src/ActionManager.cpp`, approaches distant items with a package, verifies
inventory before/after transfer, and cleans up native packages on cancellation.

## Implemented preparation

- Once `PickupProgress` issues its single transfer command, late cancellation or
  deadline expiry cannot skip observation or erase the resulting success/failure.
  Owned cleanup still precedes publication. A generation change always discards
  the operation without inspecting the replacement world. Observation remains a
  bounded adapter obligation, not permission to retry transfer or block the pump.
- `select_pickup_item` accepts a canonical eight-digit hexadecimal reference ID
  plus its exact observed display name. No name-only, base-ID, nearest-item or
  substring fallback. Partial observations permit a specifically observed
  reference; cached/unavailable observations do not. Duplicate reference rows,
  invalid metadata and held references are rejected. The existing 255-byte
  action item argument limit is preserved; overlong selectors are not truncated.
- `prepare_pickup_action` binds one exact NPC in the immutable flat scene to
  that world reference, its base/plugin/cell identity, full count, generation,
  cancellation owner, capture time, context sequence and original deadline.
  Player performers, mismatched/duplicate actors, other playthroughs, stale
  generations, VR, expired-at-capture deadlines, extra arguments and target actors
  are rejected. Preparation does not grant native execution authority or extend
  the deadline. The native adapter must independently check the current deadline.
- The request has no amount override. The complete observed reference stack must
  be transferred, with a fresh native count/identity check immediately beforehand.
- `pickup_postcondition` checks complete actor inventories (up to512 rows per side)
  before and after, conserving all named totals plus the full selected count. It
  requires a positive `retired` reference observation: disappearance from a nearby
  list, missing lookup, unload or an unavailable read must not become retirement.
  The adapter must retain/revalidate the exact reference and provide complete
  actor inventories and the pre-established secondary-ammo expectation; the helper
  alone cannot prove those native preconditions.
- Equipment flags are not assumed unchanged. Native pickup can auto-equip a
  weapon, so a full post-action inventory receipt must report the actual equipment
  outcome, including automatic unequips outside the selected base form.
- The snapshot's `stealing` field concerns the player. It is retained as observed
  context, not treated as authorization or denial for a different NPC. Native
  actor-relative ownership/theft and quest/alias behavior still need implementation.

## Exact flat executable findings (2026-09-08)

All inspection read the installed executable on disk; no game function was called.
The exact EXE and Address Library hashes are recorded in
[WORLD-ITEM-CAPTURE.md](WORLD-ITEM-CAPTURE.md).

- Actor vtable relocation ID1455516 resolves to RVA `0x25683B0`.
  Slot `0xEC` resolves to `0xC7A000`; the pinned declaration is
  `PickUpObject(TESObjectREFR*, int32_t, bool)`. The entry copies R8D as count
  and R9B as the sound flag. Its observed body ends at `0xC7A572`.
- Calls at `0xC7A34C` and `0xC7A369` reach transfer routine `0x500760`, passing
  the selected reference and supplied count. That routine retains/copies native
  extra data and has additional ownership/reference branches. Its complete
  locking, notification and failure behavior has not yet been proven safe.
- Weapon form type `0x2B` branches at `0xC7A467` to the possible equip path.
  Call `0xC7A4E8` reaches ActorEquipManager::EquipObject, relocation ID2231392 /
  RVA `0xCE5E20`. This is why the postcondition does not require unchanged equip
  flags. No gameplay claim about exactly which weapons auto-equip is made.
- At `0xC7A50A`, the reference receives `SetDelete(true)` through virtual slot
  `0x28`. The outer routine has no success return value. Neither return from the
  call nor deletion alone can establish successful inventory transfer.
- Actor slot `0xFA` resolves to `0xC81C00`, matching the declared
  `PutCreatedPackage(TESPackage*, bool, bool, bool)` entry shape. Initial code
  branches by current process, furniture state and package flags, and dispatches
  to different process package paths. This is NOT proof of package allocation,
  ownership, target binding, interruption or restoration safety.

Scratch `<scratch>/pickup-native-audit.py` verifies input
hashes, vtable/function mappings and exact instruction anchors. Observed span
SHA256 values: pickup body `0xC7A000`, length `0x573`:
`0595E99C97446717018F319DB812B91DE6D687B0DCC9D361F03B83F74E3EEAE8`;
created-package prefix `0xC81C00`, length `0x1C0`:
`0231358C193C416FBF3FC017529614F029656FF977BC1B521C1D1EC56633A9CA`.
No flat addresses/layouts are transplanted into the VR adapter.

## Validation and next implementation

### Live-reference inspection preparation (fdeb641, 2026-09-08)

flat_pickup_inspection.hpp compiles a targeted read-only flat1.11.240.0 inspector.
It resolves the exact request actor/reference, retains both for this call only,
requires a stable matching playthrough/current cell, loaded 3D/process/inventory,
normal actor state outside combat/scenes, and an unheld live item. Existing
inventory_metadata/statistics readers supply the current name/count; reference,
base and cell plugin identities must match the immutable selection. It measures
the NPC-to-item distance rather than reusing player-relative snapshot distance.
Expiry, cancellation, generation/observer-epoch change and2ms elapsed budget
produce unavailable. No object handle creation, package installation or pickup
call is present. Native getters themselves are not preempted by that time budget.

The copied-data matcher permits position, value/weight and player-facing/crime
presentation changes, but rejects reference/base/cell/plugin/name/type/count or
held-state changes. This is deliberately not NPC theft/quest authorization;
permission and final native revalidation are still required before mutation.
Native inspector behavior is not exercised in a game or mocked-engine fixture;
the source gate and flat compiler validate its structure/bindings. Actions520
native/ASan/UBSan checks cover the copied matcher and existing action logic.
Flat/VR17 suites each, gates39, packaging11, protocol32/188/200 and both lane
archives pass. Inspector remains unconnected to the disabled pickup host.

### Private travel-package construction (2026-09-08)

Cleanup follow-up audit (same executable hash, no engine execution):

- ActorPackage clear at CE9C00 acquires the recursive holder lock. Existing
  same-thread ownership uses the increment branch at CE9C23; other-thread
  contention enters a retry/yield loop. An external nonblocking acquisition
  would avoid that loop for this holder only, not prove all downstream work bounded.
- Non-null holder data at +10 invokes virtual callbacks at CE9CA0 and CE9CC4.
  CEAA60 can invoke another callback at CEAA86 before its own recursive lock.
  It releases package +C4 at CEAB12 and uses the temporary-bit deletion rule.
  CEAB80 replaces data +10 and virtually deletes the old data at CEAC10.
- Package-data factory CEAC70 returns null for kind6: its subtract/compare
  chain selects none of the allocating cases and reaches CEAEA6 with RDI=0.
  This narrows the candidate owned travel cleanup path, but does not prove
  the holder data remains null after engine updates. Require a current locked
  data check and exact owned package match; do not generalize to other kinds.
- on_f4se_message immediately invalidates observer epochs and queues lifecycle
  actions. Runtime generation invalidation runs later in apply_lifecycle from
  the Main::Update pump. Therefore deferred cleanup cannot be called proven
  pre-teardown cleanup. Before retaining installed packages across frames,
  audit a safe pre-teardown boundary or prove engine-owned retirement and a
  no-dereference stale-drop strategy; never resolve old handles after loading.

Scratch pickup-cleanup-audit.py passes ten exact byte anchors plus the kind6
branch check. Complete observed span SHA256 values: CE9C00/111
`b2277b314ded47da805f289de551db595234ff1904b81050fd03a9eb475e8d43`,
CEAA60/112 `4543550881fee094fe5307454f95c93fb631cbbd66d831b869f5070d60b27a4d`,
CEAB80/C6 `a8d2dad8f88903522438824eccd4930f2cbec2fefdaf268ea64c253da7719fee`,
CEAC70/24B `07a04e4a2c326da653eb539389e15a437b868f73f2eaa30a95de67d2506752e4`.
These are binary inspection evidence, not permission to enable the writer.

Installed F4SE boundary follow-up (2026-09-08):

- Installed f4se_1_11_240.dll reports0.0.7.9; SHA256
  `25759ED8FB110FF3CE79F174D2ACF15AA82834179D9E69C53D3FB9F59176A2EF`.
  Its load wrapper RVA E990 dispatches message2 at E9DD, calls the original
  load trampoline through CFFE8 at E9EF, then dispatches message3 at EA0F.
  Pinned CommonLib's message enum identifies2/3 as pre/post-load.
- Dispatcher5C4F0 invokes each broadcast listener directly at5C62B and advances
  its listener loop at5C637. Thus the pre-load callback is synchronous before
  this original-load call; queuing work from it does not retain that ordering.
  This matches upstream ianpatt/f4se Hooks_SaveLoad.cpp and PluginManager.cpp,
  inspected at https://github.com/ianpatt/f4se/tree/master/f4se . Installed
  binary evidence, not a moving upstream branch, establishes the ordering here.
- This resolves notification ordering only. It does not prove thread identity,
  that teardown has not begun earlier in the caller, or safe actor mutation.
  A future direct cleanup must verify runtime.is_game_thread(), exact live
  ownership and nonblocking holder availability without rebinding this callback
  as a trusted game thread. Wrong-thread/busy and main-menu/new-game paths still
  require an engine-retirement/stale-drop policy before enabling installation.

Scratch f4se-load-boundary-audit.py passes five exact anchors. Wrapper E990/A3
span SHA256 `8fa56865e01f5b059196f7cf24d7d03c406714dacbdd9bb33fde10576616695b`;
dispatcher5C4F0/159 span
`77e6014b2719b95bc6406a0daa0c40cc25dc6dd567cb8ac328114e16b33acaf6`.
No F4SE DLL or game executable was loaded or executed by these audits.

Flat preparation now creates native kind6, binds a copied destination handle to
its PackageLocation, sets radius128 and travel flags6, and initializes procedure0.
It checks the exact 1.11.240.0 runtime, five relocated function addresses and seven
entry/dispatch anchors before any native call. Kind1 is a follow-target package,
not the travel path. No actor is resolved, moved or given a package by this code.

Factory2211661 maps to RVA757DB0; location type2211916 to764AB0, handle4480239
to764D10, radius2211917 to764B50, and initialization2211735 to75D3B0. Native kind6
dispatch at75DA00 targets75D407, which sets procedure0 without using an actor.
Scratch pickup-package-binding-audit.py verifies executable/address-library
hashes, seven bindings (including package/location vtables), source anchors and
the native kind6 travel caller. No VR addresses are inferred from these results.

The private zero-reference result has scoped cleanup. An unexpectedly referenced
result is not deleted. Post-initialization missing location, cancellation, stale
generation or expired deadline fail closed. Extracted production source with
modeled native calls passes60 ASan/UBSan checks; the actual engine is not executed.
Both lane builds and17 suites each, build gates38, packaging11, protocol32/188/200
and both archive audits pass. The constructor remains preparation-only and is
not deployed as an enabled PickupItem action.

Installation remains separate: PutCreatedPackage changes actor/controller state
and does not accept an expected previous package. SetRunOncePackage(nullptr)
clears the current holder unconditionally, so it cannot serve as unconditional
SYNTH cleanup. The writer must establish and verify exact ownership, retain the
necessary package references, and define pre-load versus stale-generation cleanup
before installation is enabled. Never block ActionCompletion waiting for travel.

### Package lifetime correction deployed 2026-09-08

Product `f2886c1` fixes an existing capture prerequisite, not the pickup writer.
The earlier effective-package capture dropped the holder locks before reading
metadata through a raw TESPackage pointer. Native owner retirement can delete
that pointer during the gap. A FlatPackageReadLease now increments the native
reference count while the selected holder is locked, then permits metadata
reads outside both locks. One failed CAS defers observation without a retry;
zero and maximum counts are refused. Destruction releases the lease on every
exit, outside holder locks, following the native temporary-package flag.

Exact flat executable evidence (same hashes as WORLD-ITEM-CAPTURE.md):

- Regular process assignment at `0xCF167A` selects AIProcess+`0x18` and calls
  ActorPackage assignment `0xCE9E60` at `0xCF1687`. Run-once assignment uses
  that same routine. At `0xCE9FDB`, it atomically increments TESPackage+`0xC4`
  before storing the holder's package pointer.
- Release at `0xCE9DBC` loads minus one, then uses atomic XADD on+`0xC4`.
  If the old count was one, it calls predicate `0x758640`: package flags+`0x20`,
  bit11 (`0x800`). This differs from TESForm::IsCreated's FF FormID test.
- For a last temporary reference, `0xCE9DE8` dispatches the deleting virtual
  destructor through slot0. Pinned BaseFormComponent declares that virtual
  destructor and the engine heap operators; deleting the dynamic TESPackage
  pointer uses its native virtual destructor rather than freeing engine memory
  with a SYNTH allocator. Non-temporary packages are not deleted at zero.

Scratch `package-lifetime-native-audit.py` verifies both input hashes and five
instruction anchors. Scratch `package-lifetime-probe.cpp` includes the extracted
production lease/capture, not a rewritten implementation. Under ASan/UBSan,
the pre-fix capture produces heap-use-after-free after forced holder retirement;
the fixed flat path passes245 checks. These exercise both holders, precedence,
contention/zero/overflow, metadata exceptions, unknown plugin, native deletion
versus FormID, non-final release and100 concurrent owner retirements. The mock
engine validates lifetime/control flow, not actual game scheduling or destructor
side effects. VR's7 baseline checks pass without importing flat lifetime fields;
its native package lifetime still requires independent evidence.

Flat17/VR17 suites, build gates37, packaging11, both release audits and unchanged
protocol32/188/200 pass. The flat DLL was locally deployed and hash-verified;
both INIs and the Papyrus bridge remain unchanged. Full details and rollback
backup are in the retired parity ledger ([handoff](HANDOFF.md#retired-ledgers)). No pickup capability was enabled and no
game was started or controlled. This source-level race is not proof that it
caused all prior game freezes.

### Earlier exact-reference preparation validation

Existing action suite: 415 checks (74 added), passing both native build lanes and
AddressSanitizer/UndefinedBehaviorSanitizer. Cases include ambiguous/mismatched
actors and references, stale/cancelled owners, partial/cached observations,
whole-stack counts, split/merged named inventories, auto-equipment, wrong base or
plugin, missing retirement and unchanged source snapshots. Flat and VR each pass
all 17 suites; build gates 36, packaging tests 11, both audited packages and
unchanged protocol 32 schemas / 188 cases / 200 canonical files pass.

ActionCompletion provides begin(token, deadline) and finish(result) for the
eventual frame-spanning adapter. A successful begin claims exactly once; native
work must run only after that success. finish accepts only an executing owner
and preserves the first terminal result. It intentionally accepts a result
after cancellation so already-observed mutations are not erased. This does not
authorize further mutation after cancellation. The pump must enforce the original
generation/deadline, own cleanup and publish once on every exit; no movement wait
belongs inside execute. Existing execute remains the synchronous wrapper.
Native/ASan/UBSan action suite432 and separate flat/VR builds/tests pass.

The next required integration must:

PickupProgress (eb0ba3d) now supplies the engine-free per-operation phase control.
It owns a copied request and emits at most one command per increasing pump frame.
It latches installation cleanup before the native call and transfer-attempted
before transfer, so exceptions cannot silently authorize a repeat. The adapter
must acknowledge each command after its synchronous bounded native/read step;
it must never hold a command pending while native work runs on another thread.
Read inspection and mutation-time revalidation remain adapter responsibilities.

Only done() permits publishing outcome(). Successful inventory proof remains
pending while package cleanup is owed. Busy cleanup stays visible and can retry
on later frames even after the original action deadline; no new transfer is
permitted. A changed runtime generation emits discard without requesting native
cleanup and leaves cleanup_owed true when unresolved. The host must report and
retain that obligation according to the still-unproven native retirement policy,
not resolve the old actor in a new save or claim cleanup succeeded.

The host/native adapter is not implemented by this class. Exclusive actor slots,
completion begin/finish integration, before/after inventory storage, receipt
transport and proven engine-owned package retirement are still required.
Actions492 native/ASan/UBSan checks, flat/VR17 suites each and both package audits
pass. No engine is executed by these control-flow tests.

RuntimeBase now provides reserve_action_actors/release_action_actors (3959c14).
Existing execute_action reserves its performer/donor and optional recipient
before calling the adapter pump, and releases on normal or exceptional return.
The pickup host must keep its returned serial and generation until cleanup is
resolved. Cancellation/timeout is not a release trigger. Capacity is64 operations
with up to2 distinct nonzero FormIDs each; pair acquisition has no partial writes.
Within a runtime, even different identity labels cannot bypass a FormID conflict.
Actual actor/plugin/playthrough validation still belongs in native revalidation.

Runtime invalidation clears only copied reservation entries; it neither touches
nor proves retirement of engine packages. New generations can reserve new actors,
and an old serial/generation release cannot erase their slots. This ownership
gate is integrated for synchronous actions, not yet for the pickup coordinator
at this checkpoint. Adapters715 assertions pass in both descriptor variants
and ASan/UBSan, including reentrant calls and generation change during execution.

Follow-up762b609 also reserves the speaker during face_speech_listener. Listener
identity is read-only and is not reserved. Conflicting item actions and reentrant
facing calls reject before native mutation; a facing exception releases the
speaker reservation. Both flat/VR playback callers only log the result and
continue their playback/lip-animation path. No audio cancellation is added.
Adapters737 native/ASan/UBSan assertions, both builds and17 suites per lane,
protocol and package audits pass. The future pickup host still must acquire and
retain its own reservation; this does not implement its native cleanup policy.

1. Audit/create the exact package and target using supported native primitives;
   prove package lifetime and cleanup. Follow ESP-DEFERRED-SETUP.md if original
   forms are required. Never reuse another game's packages or invent offsets.
2. Add a bounded game-thread operation spanning frames: prepare, approach,
   revalidate, transfer once, observe, restore, terminal. Never wait for movement
   inside the synchronous `ActionCompletion::execute` callback. Preserve its
   original cancellation/deadline owner; prevent competing actions on the same
   NPC and retries after any uncertain mutation.
3. Restore only a package actually owned by this operation on success, timeout,
   hard halt, combat, target loss, cell unload or load boundary. Never overwrite a
   replacement game/mod package. Drop stale work without resolving old handles
   into a newly loaded save; define pre-load cleanup and late cleanup separately.
4. Negotiate the matched client/server capability and owned inventory plus world
   reference receipt. Update only the derived continuation; retain immutable
   original ancestry and truthful partial/unavailable observations. Require
   protocol fixtures, actual-main synthetic tests and race/replay checks.
5. Deploy only the completed matched flat path, then request manual approach,
   transfer/conservation, theft/quest behavior, interruption and save/load tests.
   Complete independent VR native and headset acceptance separately.

### Transfer ownership and secondary inventory audit (2026-09-08)

Read-only disassembly of the installed flat 1.11.240.0 executable continued through
the lower transfer routine at RVA 500760..500AD5. Executable SHA256:
FDCEF37AC1230AF6D0B0050EB2142B139EF3A867B37B9211FB6EDFCC646072F8;
Address Library SHA256:
65985CC2259384A13CFFB538D74776E422E62B0CD3766485A000620242B72B06.
The scratch inventory-native-audit.py verifies both before inspecting disk bytes;
it does not load the executable into a game process.

- PickUpObject at C7A209 calls IsAnOwner(reference, actingActor, true, false).
  Both ownership branches converge on transfer calls at C7A34C/C7A369. Ownership
  affects intervening behavior; false is not a native transfer-denial gate.
  IsCrimeToActivate instead uses the global player. Do not reinterpret the
  copied player-relative stealing flag as NPC permission, or add a blanket
  owner rejection and claim Dialectic/native behavior parity.
- IsAnOwner itself enters a native extra-data read lock at 562DD3. Reading an
  elapsed budget after it returns would not make that internal wait bounded.
- The transfer retains extra data, conditionally copies it at 5008A5, and has
  distinct branches for reference flag bit10. Preserve these native branches;
  direct base-item insertion is not an equivalent pickup implementation.
- The main inventory call at 500A45 has a second conditional call at 500A8D.
  The latter uses a weapon instance's ammo pointer at offset68 (matching the
  pinned TESObjectWEAP::InstanceData layout), or its base fallback, and a count
  multiplied by the requested world-stack count at 500813. This is evidence of
  an additional ammunition path, not proof of every quest/alias side effect.
  The current pickup_postcondition checks only the selected named base totals;
  it is not a full inventory receipt. The eventual native writer must capture
  secondary ammunition effects and auto-equipment, not silently filter them
  out of the authoritative post-action snapshot or authorize retries after an
  uncertain transfer. Exact ammo-count getter semantics still need verification.
- Tiny wrappers 28A920 and 28A970 pass extra-data types21 (Ownership) and24
  (Count) to 2A18D0. The target's semantics and quest/alias propagation remain
  to be audited; these wrappers alone do not prove safe extra-data removal.

Two source guards now reject the pinned header's actual spelling PickUpObject(
as well as the old PickupObject( spelling. Build gates39 pass; four in-memory
injections (both spellings into each guarded source) each fail the appropriate
guard. No native body changed, capability enabled, binary rebuilt or deployed.
Owned package restoration, quest/alias behavior, transfer receipts and host
integration remain required before manual game acceptance.

### Selective alias-copy evidence (2026-09-08)

The next read-only audit resolves the metadata-copy branch, not complete quest
semantics. Scratch pickup-extra-copy-audit.py pins the executable hash above,
instruction anchors and all six decoded dispatch results below. Jump tables are
decoded as data rather than interpreted as instructions.

Pickup calls the selective copy routine275C60, NOT the CommonLib CopyList
binding2190094 (275AF0). The selective routine acquires source read and target
write locks at275C8C/275C98, walks the extra-data chain, and calls272F30 only
for admitted types. Subsequent275F60(list,0) acquires the list write lock and
uses275E30(type,0) to delete unwanted extras. For the six inspected types:

| Extra type | Selective copy | Inner branch | Inventory filter |
| --- | --- | --- | --- |
| Ownership21 | admitted | 2730EE | retain |
| Count24 | admitted | 273347 | remove |
| AliasInstanceArray88 | admitted | 274CFA | retain |
| ReferenceHandle1C | admitted | 273DB1 | retain for mode0 |
| OriginalReference20 | skipped | 2740E5 (not reached here) | remove |
| Health25 | admitted | 273544 | retain |

The count wrapper28A970 runs before this copy, so admission in the table does
not imply Count survives pickup. Similarly, ownership may already have changed
in PickUpObject. These are stage-specific observations, not final item-state claims.

Alias branch274CFA clears/replaces destination type88, iterates source entries
with stride18 using array pointer+18 and size+28, and passes each entry's
quest(+00), alias(+08), instancedPackages(+10) to2AAD40. That helper obtains the
destination alias-array write lock at2AAD97, finds an insertion position, and,
if allocation succeeds, writes the24-byte entry at2AAE56..2AAE70. This is a copy
of three pointers, NOT proof of a deep copy of quests or package arrays. The
later inventory filter retains this extra type. Missing allocation success,
source alias-array concurrency/lifetime, quest event notification and persistent
reference behavior remain unproven. Do not publish quest-success from these
byte-level checks or manually reproduce a partial native copy.

The earlier source guards remain the only code changes; no native writer or new
capability is enabled. Current flat/VR builds and17 suites per lane, build gates39,
packaging11 and protocol32 schemas/188 cases/200 mirrored files pass. Full package
audit results are recorded in the goal ledger. No game execution or deployment.

### Whole-inventory pickup postcondition (2026-09-08)

pickup_postcondition now requires explicitly complete before/after inventory
observations, not selected-base subsets. It conserves all captured item totals
by FormID, plugin, form type and exact display name, adds the selected whole
world stack, and optionally adds a pre-transfer expected ammunition delta.
Unrelated loss/gain, wrong identity/name/type, unexpected or missing ammunition,
partial/unavailable/cached observations and oversized/invalid entries reject.
Positive reference retirement remains mandatory. Both arrays are capped at512
after the complete-baseline integration below.

Expected ammunition is permitted only for a selected weapon, must be a valid
unequipped ammo record with a distinct base ID and positive signed-int count,
and represents the TOTAL expected secondary count, not a per-weapon count.
Null means the native adapter verified no secondary ammo; it must not use null
for an unavailable read. The adapter must capture the expected identity/count
before mutation, never infer it from the observed after delta. Correct native
loaded-ammo reads and overflow-safe stack multiplication remain to be connected.

Split/merged stacks and auto-equipment remain permitted; the complete after
snapshot must report equipment changes. This predicate proves count conservation
of copied named records only. It does not prove quest/instance metadata, correct
actor/generation ownership, fresh native captures or package restoration. Those
are separate adapter/host gates. No native writer or capability is enabled here.
The existing action suite adds25 checks (545 total), including complete empty
inventory, unrelated records, expected/unexpected ammo, split ammo, observation
quality and invalid secondary expectations. Full validation is in the goal ledger.

### Bounded loaded-ammunition observation (2026-09-08)

Exact-executable audit of28DAF0..28DB0C shows type6A lookup, DWORD+18 read,
and zero return when absent. This matches pinned ExtraAmmo::count (uint32).
Pickup's lower transfer multiplies that value by the reference count in32 bits
at500813. A SYNTH preflight must reject values outside positive signed-int
transfer range before allowing mutation, rather than accepting native wraparound.

inventory_metadata now has an opt-in ammo-vtable argument and optional copied
reference_loaded_ammo. Unrequested reads leave it unset. Requested absent extras
return zero only after successful metadata inspection; lock/chain/type failures
still return no metadata. Ammo/count/name/instance are read under the same one
TryReadLock, using the existing512-entry cap, duplicate/presence checks and an
exact native vtable check before reading DWORD+18. No blocking ammo getter is
called. Existing callers, including VR, leave this extension off.

The flat1.11.240 pickup inspector opts in and widens the weapon ammo-count times
reference-count multiplication to64 bits, rejecting results above2147483647.
It still returns inspection state only. This does not yet supply an expected
ammo FormID/plugin/name, capture a full before inventory or authorize transfer.
Those must be captured and revalidated with the actor/reference owner before
the native writer; a changed extra count must not use an old expectation.

Scratch pickup-ammo-probe.cpp includes pickup-ammo-metadata.inc extracted from
the actual current InventoryMetadata/function source. Its fake extra-data and
lock primitives pass2208 instrumented checks under ASan/UBSan: absent/zero/full
uint32 counts, unchanged default callers, duplicate extras, wrong vtable,
presence mismatches, bounded cyclic-chain rejection, one-attempt busy lock and
copied-value stability. These are helper/mocked-lock checks, not engine behavior
or native-pointer validity proof. Native validation is separate compilation and
byte inspection. No deployment or game operation; pickup remains disabled.

### Copied ammunition identity (2026-09-08)

The flat inspector now returns PickupInspection: an inspection state plus an
optional copied InventoryItemSnapshot for expected ammunition. Only a fully
successful near/far result returns that expectation. Rejected/unavailable paths
discard it, including the final generation/epoch/deadline and identity checks.
No engine pointer is stored in the result. No ammo means a successful inspection
observed zero loaded rounds or a non-weapon; it is not inferred from read failure.

For positive weapon loaded counts, instance data is retained by the existing
metadata lease and takes precedence over base weaponData. Native27F090 finds
ExtraInstanceData(typeBA) and reads data+20; native500A61 chooses instance+68
when nonnull, falling back to weapon+200 only when absent. The pinned header
defines weaponData at198 and ammo at68, matching that200 base offset. The
inspector checks exact InstanceData/Data vtables before interpreting the pointer.
An existing instance with null ammo does not trigger a base-ammo substitution.
Missing/wrong ammo or unavailable labels/statistics return unavailable.

The copied expected record uses the actual ammo FormID, plugin, base display
name, form type, statistics and widened, prechecked loaded-times-stack total.
The eventual transfer owner must re-inspect immediately before mutation and
retain that copied expectation with its before inventory. This result alone
does not establish immutable engine instance contents across frames, quest
effects, native insertion success or cleanup; do not reuse it after mutation,
deadline or generation invalidation without the required owner checks.

Validation is exact native disassembly, header/layout review, native flat
compilation and strengthened existing source guards. The new native inspector
body has NOT been executed in a mock or the game. Full native suites exercise
the copied conservation helper separately, not this engine pointer path.
Separate flat/VR builds and17 suites each, gates39, packaging11 and unchanged
protocol32/188/200 pass. This flat-only inspector is still not host-wired or
capability-enabled; no deployment or game operation occurred.

### Complete near-range inventory baseline (2026-09-08)

PickupInspection now carries an optional copied inventory. Near-range success
requires inventory_snapshot to return complete; an observed empty vector is
engaged, while far-range results do not capture inventory. Unavailable/partial
inventory returns unavailable and discards the ammunition expectation. Capture
is performed only after selected-reference identity matches, with native
generation/epoch/cancellation/deadline checks repeated afterward.

inventory_snapshot accepts an optional outer deadline. Flat capture clamps both
its existing500us structure and2ms metadata-admission deadlines to that deadline
and rejects already-expired work before copying structure. Existing callers use
the unlimited default and retain their former local budgets. VR behavior is
unchanged. Pickup passes min(action deadline, inspection start+2ms), so its
inventory step does not start an independent2ms allowance. This bounds admission,
not preemption of an admitted engine getter or total measured frame time.

Current flat inventory capture and core ActorSnapshot support512 rows. The
earlier64-row pickup postcondition limit was inconsistent with that current
contract; it now accepts up to512 on both sides. An existing regression verifies
a complete512-row result and rejects513 rows. Actual capture may still be
partial due to time, metadata, per-item stack or total-row bounds; partial must
not authorize transfer. No merging of rows from different frames is introduced.

These are preparatory adapter changes, not a native pickup writer. The host
must use a new near-range result at the transfer boundary, keep its before
inventory and ammunition together with the action owner, and capture/verify
the after state before publishing success. No success follows merely from this
read-only inspection. Native cleanup, quest effects and manual acceptance remain.

### Run-once cleanup reaches the fallback holder (2026-09-08)

Further exact-executable inspection rules out treating SetRunOncePackage(null)
as a local pointer clear even when the owned kind6 holder has null data:

- D346A0 saves the old run-once package, calls ActorPackage clear, resets two
  high-process floats through D01AD0/D01B50, then calls CFFA00 if the old package
  was nonnull. Those float setters are null-checked straight-line writes at
  highProcess+378/+380. D029E0 similarly writes highProcess+579=0.
- CFFA00 resets multiple process fields and calls CF1220, CF2800, CF42C0,
  CF2A60, CF1330, CF1340, CF1350, CF1ED0 and D27BF0. These must not be omitted
  from a purported equivalent cleanup merely because the holder is now empty.
- CF1350 branches to D318E0 when middleHigh exists. D31933..D31948 selects
  middleHigh+60 when its run-once package pointer is nonnull, otherwise
  AIProcess+18 (currentPackage), and writes the invalid handle to holder+18.
  Thus after clearing run-once, this path can modify the ordinary current
  package's target. Locking/checking only the run-once holder does not establish
  ownership of every state touched by native cleanup. This is not a package
  pointer replacement and does not by itself prove a native bug.
- D318E0 resolves/releases the process+BC handle, can call D27BF0, and can
  invoke a reference-release virtual callback at D31976. D27BF0 also resolves
  several process handles and conditionally calls55EE40 before releasing them.
  Null kind6 data alone therefore does not prove callback-free cleanup.

Scratch pickup-cleanup-effects-audit.py verifies the exact executable SHA above
and nine instruction anchors. Span hashes: D34620/DC
899c19bd9a7b70c39ebfa11d6ad5d83ce22830503e2a3a826b3a8b89f8886231;
CFFA00/114 f6243cb45fe037afe80ae5965fcbef7be2e8f70cb8d8d969f05056dc5ae805e6;
D318E0/AF 9aab04b6fa0bdeb661d90b2b65f2285116893fe041f10aef04cfe41ea9696fa8.
The D27BF0 inspection covered its called branches through D27D47, not a full
transitive timing or lifetime proof. No cleanup mutation was executed.

The writer design must include the ordinary package/target in its ownership and
restoration analysis, distinguish expected native re-evaluation from a foreign
replacement, and establish callback/handle lifetime safety. Do not blindly
restore an old target or patch only the package pointer to bypass these effects.
Pre-load/stale retirement remains a separate gate. This audit changes the next
cleanup implementation requirement; it does not enable pickup or prove gameplay.

### Furniture reservation cleanup lock (2026-09-08)

The D27BF0 callback55EE40 is not a harmless notification: it obtains the
referenced object's extraRWLock for writing at55EE7F, calls AddChange through
the reference vtable at55EE8F, reads extra types12/7E (UsedMarkers/ReservedMarkers),
and retains the outer write lock until55F093. Conditional handle resolution,
reference-release virtual callbacks and reservation mutation run within that
scope. Cleanup calls pass null R8/false R9, which skip the optional type12
ownership branch but still reach the outer write lock and type7E path.
Do not assume a package-holder try-lock makes this downstream work nonblocking.

Pinned MiddleHighProcessData identifies the caller's currentFurniture(+3A4),
occupiedFurniture(+3A8), reservationSlot(+46C), and marker IDs(+478/+47C).
Native D27BF0 calls the reservation routine when the corresponding references
resolve; the reservation-slot branch also requires a nonnegative slot. The flat pickup inspector now requires
middleHigh and rejects current/occupied furniture handles or a nonnegative
reservationSlot, independently of standing posture. It reads copied fields and
does not resolve these handles or call the reservation routine. This prevents
starting in an already-reserved state; it does not prove reservations cannot
appear later or remove the need for interruption/cleanup ownership handling.

Scratch pickup-cleanup-effects-audit.py now passes12 pinned anchors, including
write-lock entry, type7E dispatch and write unlock. Full55EE40/263 span SHA256:
97241631c3f19dabd29b03e96395c0153fb00cc650b891fe46c80f14a17424ac.
The EndInterruptPackage alternative (ID2229892, C733E0) starts by changing actor
flags and process state, without an expected package argument; the inspected
entry does not establish it as an owned, nonblocking replacement cleanup API.

Next cleanup work must establish a safe engine-owned completion/retirement path
or a separately validated transaction rather than holding SYNTH package locks
across the full native cleanup callback chain. No writer is enabled by this
preflight guard. Its validation is native compilation/source gates and disk
inspection, not actual furniture/gameplay behavior.

### Frozen-reference implementation revalidation (2026-09-08)

Re-read the exact frozen Dialectic ActionManager.cpp and XNVSEAdapter.cpp from
commit c6e92f375a44a680affc22c8ba378320650b92da (git show, not its dirty unstable
working files). No Dialectic source or working-tree changes were made.

- BeginNativePickup checks actor/reference snapshots and base identity. Within
  radius128 it transfers directly; otherwise it rejects a pending pickup for
  the same actor and calls ExecuteNativePackageAction with action code7.
- UpdateNativePickupStates checks generation, loaded/deleted/dead actor state,
  item loaded/deleted/taken state, distance128 and an elapsed timeout of more
  than2 minutes. Removal invokes HaltNativeActor or queues cleanup when halt
  fails. Stale-generation failure is not sent as a normal funcret response.
- CompleteNativePickup checks the selected base inventory before/after and
  requires an increase. SYNTH's approved exact-stack/whole-inventory receipt
  retains stronger ownership/observation checks; a mere increase is insufficient
  for the current SYNTH protocol contract.
- Despite the Native names, ExecuteNativePackageAction and HaltNativeActor
  compile and call xNVSE script functions. They use authored Dialectic packages,
  RemoveScriptPackage, EvaluatePackage and product-specific faction/state edits.
  They do not establish that direct Fallout4 SetRunOncePackage(null) under held
  SYNTH locks is safe or equivalent. Do not transplant those scripts, forms,
  offsets or unrelated faction/weapon-state edits.

Behavioral completion remains distant approach, one exact transfer, truthful
result and owned halt/cleanup on success or interruption. A close-only transfer,
blind target/radius change, or forced package clear does not satisfy that target.
An engine-owned finish request remains a candidate, not a proven cancellation
mechanism; it must also cover a vanished destination, combat, timeout and load.

ESP-DEFERRED-SETUP.md was re-read. It gates original forms on in-game evidence,
editor/source prerequisites and separate flat/VR acceptance. No such evidence
is added by this audit, and the user prohibits automated game/editor control.
Therefore no ESP/assets are introduced or existing gate weakened. This is not
proof that a native solution is impossible; continue native completion/lifetime
work and other authorized roadmap work while manual prerequisites remain open.

No product capability, full parity milestone or gameplay acceptance is complete
from this preparatory source work.
