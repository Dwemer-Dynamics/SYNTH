# Flat caps transfer prerequisite

GiveCapsTo and TakeCapsFromPlayer are locally installed with flat client08f5d2d
and selectively ported compatible servercd59050/c020de9. Separate payment
acknowledgement, both builds and synthetic pipeline/race proofs pass. Game-dependent
conservation and save/load acceptance remain open. VR currency mutations remain
unsupported. See the retired parity ledger ([handoff](HANDOFF.md#retired-ledgers)) for exact hashes and current evidence.

The frozen Dialectic ActionManager at c6e92f375a44a680affc22c8ba378320650b92da
accepts GiveCapsTo recipient plus amount. SYNTH prepares a distinct give_caps
native request with an exact NPC donor, captured NPC/player recipient and required
JSON integer amount1..1000000, bounded by the observed unique unequipped caps stack.
There is no implicit amount or label-based currency lookup. Partial inventory can
select the explicitly identified observed caps record; unavailable cannot.

The installed Fallout4.esm record0x0000000F is MISC/EDID Caps001. Its localized or
custom display label is not identity. The native guard additionally requires that
the live initialized default Gold slot still points to that exact object, rejecting
uninitialized or custom replacement currencies rather than transferring a guess.
Existing single RemoveItemData transfer, live stack/actor/scene/epoch/deadline checks,
two-sided conservation and independent inventory receipts remain unchanged.

## Exact runtime access

The TakeCapsFromPlayer producer uses the same checked currency slot and single
RemoveItemData path, but requires the donor to be the actual PlayerCharacter and
the recipient to be the captured NPC. It never opens a generic player-item removal
path. On the wire, the NPC remains action.actor and the exact captured player is
action.target; native RuntimeActionRequest.actor is the donor instead.

| Contract | Dialogue performer | Inventory donor | Inventory recipient |
| --- | --- | --- | --- |
| GiveCapsTo | action.actor NPC | action.actor NPC | action.target NPC/player |
| TakeCapsFromPlayer | action.actor NPC | action.target player | action.actor NPC |

The continuation changes the player inventory and receiving NPC independently,
without replacing their original scene snapshot. Both observations require the
same action/generation/playthrough and their own exact actor identity. Missing
or mismatched evidence cannot be relabeled as a successful payment.

Frozen Dialectic ActionManager maps TakeCapsFromPlayer to action code10, reads its
amount and binds the target to the player. DialecticServer907eb634's description
requires the player's agreement. The server preserves that model instruction and
explicit integer amount; available funds alone are not agreement. There is no
separate machine-verified consent token or confirmation UI. It binds the player
donor/NPC recipient through original intent, accepted inventory ancestry, terminal
receipt, follow-up and cancel/replay paths. Client dispatch and receipt forwarding
require the separate player-payment acknowledgement, cleared on reinit/retirement.

### Currency identity evidence

Fallout4.exe1.11.240.0 SHA256:
fdcef37ac1230af6d0b0050eb2142b139ef3a867b37b9211fb6edfcc646072f8.
Address Library1.11.240 SHA256:
65985cc2259384a13cffb538d74776e422e62b0cd3766485a000620242b72b06.

- ID2192850 / RVA0x308100 is an indexed getter: ECX becomes RBX; it returns
  [manager+RBX*8+0x20]. The pinned no-argument GetSingleton declaration is incorrect
  for this executable. Do not invoke it as declared.
- The constructor clears0xDEC bytes from manager+0x20. Loader/resolver iterate396
  objects and use initialization flags at+0xC80, not the pinned+0xC70.
- ID4796209 / RVA0x30E78E0 is the static manager data, not a pointer to a pointer.
  Its concrete vtable is ID1436365 / RVA0x24AAF98.
- Metadata table RVA0x2EE4F90 + index3*32 identifies Gold/MISC/GOLD. The object's
  pointer is manager+0x38 and its initialization flag is manager+0xC83.
- SYNTH checks exact module version, relocation address, vtable and flag before
  copying that one pointer. It never calls the lazy constructor/indexed getter,
  adopts the stale struct layout, or reads these flat offsets in the VR lane.

Read-only repeatable evidence:
`<scratch>/caps-record-inspect.py` validates the base record,
exact EXE/library hashes, three relocations, metadata and nine instruction spans.
It does not execute game code or prove live pointer/read/mutation behavior.
Native build, tests, protocol negotiation and manual acceptance are separate gates.
