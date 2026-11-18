# Confirmed NPC trade (client)

An NPC may offer to sell the player one stack of one item it carries for caps. The server validates
the offer and the player answers in a Yes/Cancel box; only Yes moves anything. Flat only: VR never
advertises or executes it. The wire contract is in [protocol/v2/README.md](../protocol/v2/README.md)
("Confirmed NPC trade"); the server half is in SynthServer `docs/CONFIRMED-TRADE.md`. Neither half has
been exercised in the game: see [ALPHA-ACCEPTANCE.md](ALPHA-ACCEPTANCE.md#confirmed-npc-trade).

Code: `src/trade/confirmed_trade.hpp` (offer binding, staleness, the two-step transfer with its undo,
the confirmation mailbox), `src/ui/trade_prompt.hpp` (the Menu Framework window), and the `barter`
branch of `src/client/action_execution.hpp`. Tests: `tests/actions/trade_cases.hpp` (run by the
actions suite).

## Advertising and negotiation

The flat client advertises `action.barter` and `action.trade_confirmation` only when the Menu
Framework is installed (`trade::prompt().installed()`), because that window is the only presenter.
`SynthClient::trade_ready()` is true only when the init acknowledgement carries
`trade_confirmation: true` over the negotiated player-payment contract. Anything else rejects an
offer without a box.

## The offer

`prepare_trade_offer` re-derives everything from the captured snapshot of the turn: the seller is an
NPC of this playthrough that is alive, enabled and out of combat; the buyer is the player; the
seller's complete inventory holds exactly one unequipped stack of that form and name (never caps) with
at least `count`; the player's observed caps cover the price. The wire names a stack and a price, never
an inventory. A failed binding is a `rejected` receipt and no box.

## The box and the answer

The worker opens `trade::prompt()` and waits at most 60 seconds (never past the offer's own deadline).
The render thread draws a copy and submits one answer against the box's token, so a late or repeated
click answers nothing. Cancel has focus when the box appears; Escape and closing the window cancel.
A second offer while a box is open is refused, not queued. A cancelled turn, a retired session, a load
or shutdown withdraws the box and sends nothing.

Yes is only a request to start. `run_trade` captures the scene again and requires, unchanged since the
box: the generation, the scene (cell), the seller (present, alive, out of combat), the seller's stack
count and the player's caps. Otherwise the receipt is `cancelled` with `stale_cell_changed`,
`stale_actor_unavailable`, `stale_inventory_changed` or `stale_caps_changed`, and nothing moves. A Yes
after the deadline is `timed_out` / `expired`.

## The transfer

Only existing paired requests of `IFalloutRuntime` are used, in this order: `give_item` (NPC to player,
the exact stack, `count`), then `take_caps` (player to NPC, `price`). After each step a fresh capture is
read. A clean sale is reported `succeeded` / `trade_completed` only when the item moved once and the
caps moved once between the two actors. If the payment failed and no caps moved, the item is returned
once with `take_item` and the starting inventories must be restored. Any other result that cannot be
proven (an unanswered step, a partial payment, a failed return) stops trading for the rest of the game
session (`trade::trading_stopped()`) and tells the player to check their inventory. Nothing is retried
and nothing is created or deleted to balance a result. The HUD shows the outcome.

The receipt is the existing paired `synth.action.transfer-result.v2`: the NPC's inventory, then the
player's, read after the answer. Every outcome sends one, including a refusal (with `unavailable`
observations).

## Not proven

Menu Framework rendering and input, the native transfers during a sale, behavior across a real cell
change, load or save, VR (unsupported by design) and gamepad focus have not been tested in the game.
The tests use a fake runtime and a fake mailbox; they prove the rules above, not the engine.
