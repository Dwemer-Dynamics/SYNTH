# World-item capture prerequisite

## Scope and behavior

The flat Fallout 4 1.11.240 adapter now captures the actual world-reference stack
count, cached display name and retained instance statistics. Previously the nearby
item path always supplied count one and base-instance statistics, and queried the
display name through a blocking extra-data getter.

The existing inventory metadata helper accepts an optional expected ExtraCount
vtable. World-item capture supplies it; inventory-stack callers do not, so their
existing authoritative stack counts are unchanged. Under one non-blocking read
lease, the helper checks the bounded extra-data list and presence bits, rejects
duplicate records, validates the count vtable, and copies the unsigned count.
No count record means one. A zero count, inconsistent presence bits, unexpected
vtable, busy lock or unreadable metadata omits that row and marks the observation
partial; none is reported as a successfully observed empty collection.

The same lease copies the bounded cached name and retains the instance. Effective
weight/value calculation occurs after releasing the lease, using the existing
audited inventory statistics path. Nearby item rows bypass the irrelevant
map-marker getter; non-item point-of-interest behavior is unchanged. Existing
identity, distance, ordering, collection limits, theft and held-item fields remain.
No server contract, protocol, configuration or action capability changes are needed.

## Exact-runtime native evidence

Read-only on-disk inspection, not execution of game code:

- Fallout4.exe version 1.11.240.0, SHA256
  `FDCEF37AC1230AF6D0B0050EB2142B139EF3A867B37B9211FB6EDFCC646072F8`.
- `version-1-11-240-0.bin` SHA256
  `65985CC2259384A13CFFB538D74776E422E62B0CD3766485A000620242B72B06`.
- ExtraCount type is `0x24`; `RE::VTABLE::ExtraCount[0]`, relocation ID808121,
  resolves to RVA `0x24701A0`. BSExtraData size is `0x18` in the pinned flat library.
- ExtraDataList::SetCount, relocation ID2190125, resolves to RVA `0x27B530`.
  It compares the input WORD with one and removes the count record for values at
  or below one; its existing-record update writes a WORD at offset `0x18`.
- Getter RVA `0x2800F0` requests extra type `0x24`, returns
  `movzx eax, WORD PTR [rax+0x18]` when present, and returns one otherwise.
  Exact bytes:
  `4883ec28b224e8550202004885c074090fb740184883c428c3b8010000004883c428c3`.
- Constructor RVA `0x2A3EB0` sets type `0x24`, the above vtable and WORD at
  `0x18`. Comparator RVA `0x2A3EE0` also compares the two WORD fields.

Runtime code resolves the vtable through the Address Library; it does not invoke
the getter or embed these executable RVAs. This evidence is flat-only and must
not be reused as a VR offset/layout claim.

## Validation (2026-09-08)

- Flat build and all 17 native suites pass; independent VR build and CTest 17/17
  pass. Existing build gates 36 and packaging tests 11 pass.
- Exact current `InventoryMetadata` / `inventory_metadata` source matches the
  focused scratch probe. AddressSanitizer and UndefinedBehaviorSanitizer run
  passes 3,592 checks, including absent count, 1, 2, 32767, 32768, 65535, zero,
  wrong vtable, duplicate/missing presence, cycles, busy lock, retained instance
  lifetime, custom name and effective statistics. The probe uses mock engine
  structures; it does not execute the complete game capture function.
- Read-only binary audit verifies both input hashes, relocation mappings and
  the exact getter/setter/constructor/comparator instruction spans.
- Structural gates check that the flat item branch uses the metadata/statistics
  helpers, returns partial on failure and has no hard-coded one or blocking name
  getter. Both runtime builds validate the actual adapter wiring.
- Protocol remains 32 schemas / 188 fixture cases / 200 canonical files, matching
  the maintained server. Deployment and artifact hashes belong in the main goal
  ledger; source and synthetic checks are not gameplay acceptance.

Scratch evidence is under `<scratch>/`:
`world-count-audit.py`, `inventory-native-audit.py`,
`world-item-metadata-probe.cpp` and `world-item-metadata-sanitized`.

## Remaining acceptance and PickupItem parity

See [PickupItem implementation evidence](PICKUP-ACTION.md) for subsequent exact
reference binding/postcondition work and native package integration requirements.
This is a metadata prerequisite, not a PickupItem implementation or proof that
all nearby capture is non-blocking. Non-item map-marker, display-name and lock
queries, as well as theft queries, still need separate runtime-path review.
The VR item path retains its previous count-one/base-instance behavior and
blocking name getter; its correctness and performance remain unproven.

The frozen Dialectic client `c6e92f375a44a680affc22c8ba378320650b92da`
uses `BeginNativePickup`, `CompleteNativePickup` and `CancelNativePickups` in
`Plugin/src/ActionManager.cpp`: immediate transfer within 128 units, a native
movement package for more distant references, before/after inventory evidence,
and cancellation/package restoration. Full parity must preserve approach,
exact world-reference identity and stack semantics, theft/ownership handling,
inventory delta, reference retirement and generation-safe restoration. A direct
inventory increment or close-range-only substitute is not equivalent. PickupItem
remains unadvertised until its matched native/protocol/server path exists.

Manual flat acceptance must cover stacked drops, renamed/modified items, nearby
dialogue and InspectSurroundings, scene transitions, save/load and sustained
message responsiveness. Correlate actual logs and inventory observations. VR
needs its own implementation evidence and independent headset acceptance.
No game, editor or headset was launched or controlled for this work.
