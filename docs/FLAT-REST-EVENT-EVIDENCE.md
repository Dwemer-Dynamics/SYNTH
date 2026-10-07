# Flat rest-start observation

Flat observation now feeds automatic diary dispatch; gameplay acceptance is still outstanding.
No ESP/Papyrus, game record, polling animation inference or game-time-jump heuristic is added.

## Exact runtime evidence

Read-only inspection of the installed Fallout4.exe1.11.240.0 and its matching
version-1-11-240-0.bin resolved the pinned CommonLibF4 vtable IDs. The executable's own
MSVC complete-object locators and base-class descriptors confirm the following layout:

Inspected SHA256 values:
- Fallout4.exe: FDCEF37AC1230AF6D0B0050EB2142B139EF3A867B37B9211FB6EDFCC646072F8.
- Address Library: 65985CC2259384A13CFFB538D74776E422E62B0CD3766485A000620242B72B06.

| Handler | Address Library ID | Vtable RVA | Subobject | ProcessEvent slot1 RVA |
| --- | --- | --- | --- | --- |
| GameScript::SleepEventHandler / TESSleepStartEvent sink |1533444|0x025B7278|0|0x010DABB0|
| GameScript::SleepEventHandler / TESSleepStopEvent sink |4826623|0x025B7290|8|0x010DAC60|
| GameScript::WaitEventHandler / TESWaitStartEvent sink |95246|0x025DB958|0|0x011A7620|
| GameScript::WaitEventHandler / TESWaitStopEvent sink |4826726|0x025DB970|8|0x011A76D0|

The inspected type names are the GameScript SleepEventHandler/WaitEventHandler classes;
their offset0 bases are `BSTEventSink<TESSleepStartEvent>`/`BSTEventSink<TESWaitStartEvent>`.
The separate stop sinks are at offset8 and are not patched. The pinned BSTEventSink interface
defines a virtual destructor at0 and ProcessEvent at1, returning BSEventNotifyControl.
No event payload layout is assumed: self/event/source are forwarded unchanged as opaque pointers.
Do not infer these addresses from Skyrim, older Fallout builds, RTTI names alone or the VR library.

Installed-code guards require exact executable version and both original slot targets before
writing either hook. A changed slot, including an earlier mod hook, disables this observer.
This avoids claiming compatibility with an unknown handler chain. A later mod can still replace
the slot; manual hook/log and compatibility acceptance remain mandatory.

## Runtime boundary

Installation is at F4SE post-load, outside save-load/frame capture. Each callback takes a mailbox
epoch before invoking the original handler, returns its original result, and enqueues one
sleep/wait reason afterward. No game-object access, allocation, HTTP, logging, filesystem work or
provider dispatch occurs inside the hook. Event pointers are never retained.

The mailbox is a lock-free atomic word with lifecycle epochs, bounded callback CAS retries and
a packed 16-entry FIFO. Repeated starts are distinct and retain successful-enqueue order. A full
buffer drops the newest start, never an older entry. Overflow or exhausted callback contention
increments a separate process-wide diagnostic counter; this counter is never interpreted as a
rest event or attributed to a new save. Logging occurs only at the safe pump, not in the callback.
Invalidation clears/retire-fences pending entries immediately at lifecycle delivery and again when
deferred invalidation is applied. Settings-driven session replacement/reconnection invalidates it
too, including explicit session retirement/Halt. Re-arming cannot accept an old captured epoch or
turn the lifetime drop counter into events. Arming requires an adopted non-halting session at the
stable world-ready engine pump. Consumption follows settings reload/adoption/retirement checks and
occurs only when menus are closed. Starts before session readiness are not observed or inferred later.
This proves observation of an engine start callback, not that the menu-free pump is the start
instant or that starts beyond the explicit buffer/contention bounds have been retained.

The flat game thread verifies that both slots still point to these hooks before freezing sleep/wait
capabilities into session initialization. The shared coordinator never reads engine vtables. A later
slot replacement disarms observation at the safe pump. VR does not advertise these capabilities.
When menu/dialogue gates permit, the pump drains observed starts and captures one immutable frame
for that batch. This is explicitly safe-pump context, not exact start-time context. Each start retains
its reason and copied frame in a separate session-owned queue (16 entries, reject newest when full).
Frames expire two minutes after capture; load, halt and session replacement cancel their ownership.
No cached frame is substituted when capture fails. Pre-capture events remain bounded by the mailbox,
not by this copied-frame timeout; prolonged menus can delay their capture until menus close.

The background lane admits at most one diary batch while existing manual/automatic work is tracked.
Saturation retries no faster than five seconds, only until expiry. An accepted lane task can expire
before execution, reported by task-lane health; no completion is fabricated. The worker publishes the
copied context and uses exactly its published NPC audience, plus Player and Narrator. The server's
existing per-role settings and cooldowns remain authoritative and are not enabled by deployment.
An unprofiled but valid visible NPC is disabled only for automatic admission; forged/ambiguous actors
still reject the transaction. Admitted work uses immutable receipts and partial-aware status, followed
by a fresh game-thread checkpoint before the next-save completion notice.

## Proof limits

Native mailbox checks cover disabled state, repeated starts, ordered distinct reasons, drain-once,
16-entry capacity, newest-drop overflow, lifetime diagnostics, invalid reasons, pending-load/session
replacement discard and late pre-invalidation callback rejection after re-arm.
Exact-binary inspection and a successful DLL build do not prove the engine calls this hook in
the user's mod stack, cancellation behavior or save-load stability. Follow MANUAL-DIARY-TEST.md.
VR requires a separate runtime-specific observation implementation and independent headset gate.
