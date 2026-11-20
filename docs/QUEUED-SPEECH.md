# Queued speech results (optional)

The server can keep a speech result in a durable queue and hand it to the client exactly once, instead of the client
polling a repeatable status. It is opt-in and the legacy path stays: with the setting off, a server that does not queue,
or a line the server did not queue, the client polls `GET /api/v1/messages/<cache key>` as before.

- **Setting.** `[Server] MessageQueue=false` by default. Set it to `true` in `SYNTH_custom.ini` to opt in. The client then
  lists the v2 capability `message.queue` in every event. The server names a message id only for a session that did, and
  only while its own queue is on (`SYNTH_MESSAGE_QUEUE`).
- **The id.** A voiced dialogue line's `speech` block carries `message_id`. `SpeechDelivery::await_queued_speech` fetches
  `GET /api/v1/messages/<id>` (`SynthClient::fetch_message`, schema `protocol/v2/schemas/message-result.schema.json`).
- **Exactly once.** The first fetch of a completed message answers 200 and the server never returns it again, so the
  client keeps what it gets: later fetches answer 409 `delivered`, and a client that lost the first answer shows the line
  as text only. 409 `pending` means keep waiting; 404 (`cancelled`, `expired`, `unknown`) ends the wait with text only.
- **Fetch on wake-up.** A `message_available` hint on the wake-up channel ends the wait at once, and the client fetches. The
  epoch is read before each fetch, so a hint that lands during it is never lost.
- **Poll fallback.** With the channel down (or never configured) the loop polls every 100 ms. While the channel is up it
  keeps a slow safety poll (1 s) beside the push, so a lost hint costs at most a second.
- **Stale results.** A delivered result carries its `session_id` and `generation`. A result whose session is not this
  runtime session, whose generation is not the line's, or whose cache key is not the line's is dropped
  (`protocol_native::judge_queued_speech`) and the line shows as text only; it is never played.
- **Tests.** `tests/protocol_native` (message decode and the stale judgement), `tests/transport` (the hint and the poll
  intervals), `tests/fake_server` (`fetch_message` outcomes, exactly-once and refusals) and `tests/config`.
