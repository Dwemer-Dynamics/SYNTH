# Flat inventory-change observation

Scope: refresh notification only on Fallout 4 1.11.240. This is not an item-history,
trade, consumption or witness event. No offsets or observer are enabled on VR.

## Exact native evidence

- Executable SHA256: `fdcef37ac1230af6d0b0050eb2142b139ef3a867b37b9211fb6edfcc646072f8`.
- Address Library SHA256: `65985cc2259384a13cffb538d74776e422e62b0cd3766485a000620242b72b06`.
- Pinned CommonLibF4: `6266ecc9014b473fc6b6efd04abac324477c63cd`.
- `GameScript__InventoryEventHandler[2]`, Address Library ID4826614, table RVA
  `0x25B6188`, ProcessEvent slot1, native function RVA `0x10C1150`.
- Pinned `TESContainerChangedEvent` is 0x18 bytes. Old container, new container,
  base item are uint32 fields at offsets0/4/8. The observer copies only this
  12-byte scalar prefix. It does not retain the event, self, source or game pointers.
- Native function keeps event in RSI at0x10C1182. It resolves `[rsi]`, `[rsi+4]`
  and `[rsi+8]` through the form-ID resolver at0x10C123B,0x10C1253 and0x10C1273.
  Reference ID+0x10 and unique ID+0x14 also match the pinned container-event layout;
  SYNTH does not consume those fields. The other adjacent InventoryEventHandler
  slots have different pointer-bearing layouts and are not used for this observer.
- Read-only inspection used `inventory-native-audit.py` in
  `<scratch>`; it verifies both binary hashes before
  disassembling and never starts the game.

## Behavior and safety

Installation requires the exact runtime and untouched expected slot. A mismatch
leaves periodic reconciliation available. The original handler receives the same
three arguments exactly once; its return value is forwarded. After it returns,
an armed, same-owner callback may publish one scalar revision if either container
is Player0x14 and the scalar IDs are valid. No game lookup, inventory capture,
allocation, HTTP, queue dispatch or new engine lock occurs in the callback.

The core reuses the bounded scalar mailbox with a dummy payload. Every successful
publication changes its stamp, even repeated notifications. A single CAS prevents
an older callback from replacing newer evidence or publishing after load/session
invalidation. Notifications coalesce; this is not an exhaustive mutation journal
or a proven version counter for multi-frame inventory pagination.

The stable host arms independently of actor-history transport settings. It polls
the latest observed stamp and marks the existing player refresh cadence dirty.
Inventory menus may continue to produce signals, but actual inventory capture
and HTTP delivery remain menu-free. Retirement, unavailable world, halting and
immediate F4SE invalidation disarm the source. Existing 30-second reconciliation
remains the fallback for missed callbacks or paths that do not emit this event.

## Evidence boundary

Both native builds and suites compile/validate the source. Lifecycle tests cover
pickup/drop/container directions, malformed/nonplayer IDs, same-container changes,
late callback rejection, invalidation/rearm and a 1000-notification burst. A scratch
probe of the actual observer body verifies argument/result forwarding, prefix copy
before the original callback and late-invalidation rejection (21 checks under GCC
address/undefined sanitizers). The lifecycle suite passes1534 assertions, including
the corrected other-container fixture (`0x200`, not decimal20/Player0x14).

These tests do not prove the hook is installed in a real session, that every
pickup/drop/trade/modded inventory path emits this event, or that inventory reaches
an in-game response. Manual pickup, drop, container transfer, barter and save/reload
tests with matching installed DLL/log/context evidence remain required.
