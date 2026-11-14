# Auto Chat

Auto Chat (`AUTOCHAT`) is an explicitly selected targeted mode. Typed text, or voice that the
server transcribes, is an instruction for what the player says. The server rewrites it into one
player line, sends that line immediately as player speech and then returns the selected NPC's
ordinary targeted reply. There is no preview, no shorthand, no NPC initiation and no approach.
The server contract is Synthserver `docs/AUTO-CHAT.md`. Open checks are in
[ALPHA-ACCEPTANCE.md](ALPHA-ACCEPTANCE.md#auto-chat).

## Selection and admission

- Auto Chat is one more value in the Control menu's existing mode list (`src/ui/settings_catalog.hpp`,
  Conversation group). No configuration key, setting or schema field is added; the v2 control
  schema and manifest mirror the server byte-for-byte.
- The mode travels as a `control` mode write and as `payload.control.mode` on `input_text` and
  `input_audio`. The other modes, their actions and the Audience row behave as before.
- `/talk` is separate and is refused locally in Auto Chat with a notification
  (`PlayerDialogue::admit_text`); the codec also refuses `intent: "conversation"` with `AUTOCHAT`.
- Everyone stays a manual choice on the single existing root Audience row and is valid only in
  Standard or Shout. The codec rejects an Everyone selection with `AUTOCHAT`.
- Only typed and voice turns are admitted. External Ask and Papyrus prompts are refused visibly
  in Auto Chat, never rerouted to another mode (`PluginSession::queue_text_for_actor`).
- Auto Chat does not start conversation holds or approach movement.

## Presentation

The reply carries one `dialogue` line whose speaker is the player and whose listener is the
selected NPC, followed by the NPC's reply. When the server has a configured player TTS connector
it synthesizes the line's audio before buffering (synthesis, not playback); otherwise the line is
text-only. A `provider_failed` status is a terminal refusal;
the client never retries it with a new request ID. Speech receipts are unchanged: queueing or
buffering a line is never reported as spoken or played.

## Departure watch

After a successful typed or voice admission, `PlayerDialogue::watch_autochat` records the exact
target (form, base form, plugins, playthrough), the turn token, the admission frame and time.

- The flat and VR plugin entries poll it on the game thread every 250 ms through
  `IFalloutRuntime::observe_conversation_actor`. The CommonLib adapter
  (`conversation_actor_presence`) reports `present`, `departed` (dead, disabled, deleted, or in a
  different interior or worldspace) or `unavailable`.
- Only the first fresh `departed` frame after admission, in the same generation and for the same
  watch, cancels the turn, and only if that turn is still the owner. It then discards
  already-cancelled network and speech work and notifies the player.
- Unknown, unloaded, unresolved, stale or `unavailable` observations never cancel. A later turn,
  Stop, load, new game or session end releases the watch.
- The watch ends once the flow is idle: no responses, actions, speech jobs, playback or queued
  captions, and rechat idle.

## Saves and replay

Server history is scoped by saved ancestry, so an older save sees only its own Auto Chat rows.
Raw intent and the generated player history row stay separate on the server. For typed input the
server keeps the original text; for voice it keeps the original audio media reference and freezes
its trusted transcript separately, without adding text to the audio event. A duplicate request
replays the stored result or answers `request_pending`; the rewrite is not invoked again.

## Automated checks

- `tests/protocol_native`: mode encoding, `/talk` and Everyone refusals.
- `tests/fake_server`: admission, `/talk` refusal, the departure watch and its stale, replacement
  and caption-idle fencing.
- `tests/adapters` and `tests/build/test_build_gates.py`: the observation seam and admission gates.
- `tests/loopback_dialogue` `autochat` scenario with the paired server
  ([README](../tests/loopback_dialogue/README.md#auto-chat-scenario-opt-in)).

Run these on the exact commits under review. None of them is flat or VR gameplay evidence.
