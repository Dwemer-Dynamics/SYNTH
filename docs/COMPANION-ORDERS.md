# Companion orders: Follow, Stop Following and Take a Seat

Source and off-game tests only. Nothing here was run in the game, and the flat adapter that applies the
orders is not written yet, so the capabilities are never advertised.

## Audit against Wait (before this change)

| Action | Server | Client | Protocol |
| --- | --- | --- | --- |
| `wait_here`, `release_wait` | typed intent, owner check, JEV hand-off, physical-receipt memory | `wait_action.hpp`, `flat_wait_package.hpp` (owned quest, aliases 0-255, 90 s lease, Release verified) | `wait-action.schema.json`, fixtures |
| `follow` (targeted) | legacy catalogue row, resolves a captured target | no capability, never executed | closed v1 enum only |
| `follow_player`, `stop_following`, `take_a_seat` | catalogue rows (`FollowPlayer`, `StopFollowing`, `TakeASeat`) with free-text parameters and no validation | none: not advertised, not executed | closed v1 enum only |
| conversation approach | n/a | original Follow template and 8 approach aliases, used only to walk an NPC to the player | n/a |

The names existed in the catalogue and schema enum, but no client could apply them, so they were never
negotiated. The approach quest is conversation-owned (bounded, listener alias), so it is not reused as an order.

## What this change adds

- **Applied by the game.** The server names the order and nothing else. `prepare_order_action` accepts no
  target and no arguments; the runtime adapter (`RuntimeActionName::follow_player`, `stop_following`,
  `take_a_seat`) owns every package, alias and seat.
- **Bound to the exact reply line.** The action carries `reply_line_id`. `OrderReplyBinding`
  (`src/client/order_action.hpp`) records each accepted NPC dialogue line and claims it once, only for the same
  speaker, request, turn and a live cancellation token. A missing, replayed, cancelled or mismatched reply drops
  the order. The replay guard that protects Wait also covers orders.
- **Verified receipt.** `RuntimeOrderObservation` must show an owned game package (Follow, Sit with a seat) or no
  owned package (Stop). `verified_order_result` emits `synth.action.order.result.v1`, at most 1024 bytes.
- **Release.** `stop_following` releases only SYNTH's own order and does not dismiss a native follower.
- **Unreachable seat cooldown.** `SeatOrderWatch` (`src/runtime/seat_order.hpp`) ports the CHIM 4 Sit supervision:
  no-progress, walk-timeout and arrived-but-not-seated retry the next seat; after three seats the NPC sits on the
  ground, then waits. A seat that could not be reached is skipped for ten minutes; an occupied seat is retried
  later. It takes copied observations and an injected clock.
- **Capabilities.** `action.follow_player`, `action.stop_following`, `action.take_a_seat` and
  `action.order_verified`, flat only, and only when `IFalloutRuntime::orders_ready()` is true (default false).

Paired server change: Synthserver `docs/COMPANION-ORDERS.md`.

## Not done (needs the game and its master data)

1. **Flat adapter** (`orders_ready()` is false): authored Follow and Sit packages and aliases in `SYNTH.esp`
   (never renumber existing records), their fill and clear through the existing owned-package pattern, seat
   search, and the observations above. Sit needs a Fallout4.esm template and a seat finder that has not been
   audited. `commonlib_actions.hpp` returns `unsupported_runtime` for the three orders until then.
2. In-game acceptance: Follow through load doors and combat, Stop, a stood-up Sit, save/load, and foreign
   followers.
3. VR: no adapter, no capability.

## Tests

`tests/actions` (`order_action_cases.hpp`): capability gating, reply binding, preparation refusals, receipts,
seat cooldown and the ground and wait fallback. `scripts/verify_protocol.py` covers the new schema and fixtures.
