# Server connection notice

Source and off-game tests only; nothing here has been checked in game.

The plugin tells the player when the SYNTH server stops answering and when it answers again. It works with no session
(a session cannot start while the server is down), so it is a separate owner, `ConnectionWatch` in
`src/client/connection_notice.hpp`, held by the flat and VR plugin entries.

## Behavior

- **Probe.** `GET /health.php?check=live` on the configured `[Server] BaseUrl`. The server answers it without a
  configuration, database or provider (server `docs/HEALTH-CHECKS.md`),
  so it is cheap. "Reachable" means an HTTP answer arrived; an unhealthy server is the health page's concern.
- **Interval.** Every 20 seconds while the server answers. After a failure, 5 seconds, doubling per failure to a 60 second
  cap, until it answers again. One probe runs at a time, on its own short-lived worker thread (3 second timeout, own
  WinHTTP transport), never the game thread, a session lane or the dialogue request lane.
- **Notices.** One notice when the server becomes unreachable and one when it returns, shown through the existing HUD
  notification (`present_notification`). An outage is reported after two failed probes in a row, so one dropped
  connection is silent. If the server was never reachable in this run the text says "cannot reach", otherwise "lost
  connection". Repeated failures and repeated successes show nothing.
- **Game load.** A new game or loaded save probes at once and, if the outage was already reported and is still going,
  shows one reminder. Notices queue until the game thread drains them during gameplay, so none is lost behind a loading
  screen.
- **Setting.** `[Server] ConnectionNotice=true` (also on the Behavior settings page). `false` stops the probe and every
  notice. Toggling it does not recreate the session; changing `BaseUrl` rebuilds the monitor.
- **Lifecycle.** The monitor follows the plugin lifecycle: it stops with the plugin and is rebuilt when the server
  settings change. The transport is separate from the session's, so loading a save never cancels a probe or leaves one
  behind.

## Source and tests

| Piece | Where |
| --- | --- |
| State machine (`ConnectionNotice`), probe worker (`ConnectionMonitor`), settings owner (`ConnectionWatch`) | `src/client/connection_notice.hpp` |
| Setting | `src/config/config.hpp`, `config/SYNTH.ini`, `src/ui/settings_catalog.hpp` |
| Wiring | `src/flat/plugin_entry.cpp`, `src/vr/plugin_entry.cpp` (pump after session adoption, lifecycle hooks) |
| Tests | `tests/tasks` (state machine, monitor over the fake HTTP transport, settings owner), `tests/config` |

A real WinHTTP client against a loopback PHP server that was stopped and restarted produced exactly one "lost" and one
"restored" notice. No in-game, flat or VR headset run was possible.
