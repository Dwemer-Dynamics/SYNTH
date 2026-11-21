# Wake-up channel (optional)

The server can push a content-free hint that stored speech state changed, so the client's wait for ready speech ends
early instead of running out its poll interval. It is opt-in and never required: polling is the default and the
fallback, and with the channel off, absent or broken the client behaves exactly as it did before.

- **Setting.** `[Server] WakeChannel=false` by default. Set it to `true` in `SYNTH_custom.ini` to opt in. The client then
  lists the v2 capability `notify.wake` in its init.
- **Negotiation.** The server acknowledges with init status `notify_wake: {"port": N}` only while its notification
  channel runs. No acknowledgement (old server, channel stopped, or an old client that never asked) means no connection
  is ever attempted. Wire text: `protocol/v2/README.md`; schema `protocol/v2/schemas/notify-hint.schema.json`.
- **Connection.** One background thread (`NotifyChannel`) connects a WebSocket to the host of `BaseUrl` on `N` with a
  bounded connect timeout, reads hint frames and signals a shared `WakeSignal`. It sends nothing. It never touches game
  state and never blocks the game thread or a protocol request; it uses its own synchronous WinHTTP session
  (`src/client/winhttp_notify_transport.hpp`). Frames are hints only: an unknown, malformed or oversized frame is ignored,
  and a `wake` before the `welcome` is ignored.
- **What it changes.** The speech readiness poll (`SpeechDelivery::prepare_speech_media`, shared by flat and VR through
  `PluginSession`) waits on the signal instead of sleeping. While the channel is up its interval is 250 ms instead of
  100 ms and a hint ends the wait at once; the moment the channel drops the interval returns to 100 ms. The epoch is read
  before each status request, so a hint that lands during it is never lost. Everything still arrives over HTTP.
- **Reconnect.** Delay starts at 1 s, doubles per failed or welcome-less attempt up to 60 s, and restarts from 1 s only
  after a connection proved itself with a welcome frame. There is always a wait between attempts, so there is no busy
  loop. One on-screen line reports an unavailable channel and one reports a lost one; retries are silent. A connection
  that is lost wakes the poll once on reconnect, so anything missed while it was down is found.
- **Queued speech.** A `message_available` hint names the queued message that just completed; it ends the wait of the line
  that is fetching that message ([QUEUED-SPEECH.md](QUEUED-SPEECH.md)).
- **Not changed.** The protocol, response lines, generation and ownership rules, and every other request path.
- **Remote hosting.** The plugin connects to the host of `BaseUrl` on the acknowledged port with plain `ws://`; frames
  carry nothing sensitive. A remote installation must forward that port as for the HTTP port, or leave the setting off.

Tests: `tests/transport` (hint parsing, signal, absent channel, backoff, drop and reconnect, stop), `tests/protocol_native`
(status acknowledgement) and `tests/config`. Not run in game: the WinHTTP transport was exercised against the server's
channel on loopback only.
