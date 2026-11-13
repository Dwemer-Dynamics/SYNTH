# SYNTH Control menu - local implementation

Source: `codex/response-flow-parity`, preserving the response-flow, subtitle and voice changes already present in this worktree. No publishing or version bump.

## In-game surface

In the flat lane the `Hotkeys.SYNTHControl` scan-code binding opens SYNTH's own F4SE Menu Framework window. Like every SYNTH hotkey it ships as 0 (unbound) in `config/SYNTH.ini`. It no longer launches a browser. An explicit Open Server Dashboard row remains available.

The menu exposes Chat Mode, Model Slot (server-configured Standard/Fast/Powerful/Experimental slots), Dynamic Profiles, Wait Here, Release Wait, Write NPC Diary, Open Server Dashboard and Close. The existing Hotkeys, Auto Activate, Behavior, Sound and Tools settings pages remain separate. The renderer owns framework registration/rendering; it passes bounded, generation-tagged requests to the game thread. Save/load or retirement invalidates drafts and queued actions.

Mode/model selectors wait for a validated server acknowledgement. Profile refresh waits until the menu closes. Wait Here and Release Wait close the menu automatically and execute on the next menu-free game pump, revalidating the originally displayed NPC. They cannot switch to a new target if that actor disappears.

## Bindings and lanes

All eight `[Hotkeys]` scan codes (`TalkToNPC`, `ToggleVoice`, `StopTalking`, `SYNTHControl`, `OpenMicMute`, `ManualActivate`, `PipVision`, `WriteDiary`) ship as 0, meaning unbound. SYNTH's INI chain is authoritative and the game thread polls it in both lanes.

| Surface | Flat with Menu Framework | Flat without framework | VR |
|---|---|---|---|
| Rebinding | SYNTH Hotkeys page or `SYNTH_custom.ini` | `SYNTH_custom.ini` | `SYNTH_custom.ini` only |
| Conflict check | Framework registry only, on SYNTH's page | None | None |
| `TalkToNPC` | Native chatbox with To: NPC picker | Clipboard text | Clipboard text |
| `SYNTHControl` | This control menu | Unavailable | Opens the server dashboard |
| `WriteDiary` | Dispatched | Dispatched | Not dispatched |
| `PipVision` | Dispatched | Dispatched | Dispatched; VR capture acceptance unverified |

SYNTH registers its hotkeys in the framework registry for conflict awareness only; their callbacks are inert and the registry is never read back into SYNTH's configuration (its binding lookup is read only to validate an explicit key selection). Editing a SYNTH entry on the framework's generic Hotkeys page therefore does not change the key SYNTH uses, and the SYNTH Hotkeys page says so. Duplicate keys typed into the INI and game or other-mod key conflicts are not universally detected. VR has no framework, MCM, ESP or controller rebinding surface and polls the desktop keyboard. No controller or headset behavior has been verified.

## Settings and response ownership

The coordinator loads local configuration without accessing game objects. Valid settings remain visible without an AI connection. Hotkey-only edits use a game-thread snapshot without retiring the session. Other settings wait for an idle, menu-free reconnect. Input captured by the menu must not submit a microphone recording.

`control.menu` adds a closed v2 control RPC with no dialogue context sequence. Mode/model preferences reuse `conf_opts`. Text/audio requests carry their copied selection. Old or malformed control requests are rejected by the initialized-session/generation checks. Existing dialogue cancellation and response envelopes remain authoritative.

## Limits requiring manual verification

- Wait Here uses the original `SYNTH.esp` aliases and matching HoldPosition package for 90 seconds of unpaused gameplay. Terminal success requires the active owned package and stable stopped-position proof; Release Wait requires empty owned aliases and fresh package absence. Existing foreign restraint is preserved. See `WAIT-ALIAS-CHAT.md`; `WAIT-HERE.md` records historical implementations.
- Native Director sends an instruction through the selected NPC's structured response/action pipeline. It does not yet provide Dialectic's full multi-NPC scene orchestration or legacy global role-command queue.
- Whisper/Close publish a private, 200-unit audience; Shout expands the normal hearing radius. Provider-specific vocal whisper/shout effects are not added.
- Model slots must be configured in the server. A native selection does not silently substitute another empty profile slot. In-flight text/audio requests retain their selected slot; automatic follow-up model inheritance needs a separate audit.
- Native menu visual behavior, all nine provider-backed modes, NPC waiting and save/load acceptance require manual Fallout 4 testing. No game was launched for this implementation. VR wait-package support is not enabled by the flat deployment.

## Manual acceptance

1. Start Fallout 4 manually, load a save, and press your configured SYNTH Control hotkey. Confirm an in-game menu opens rather than a browser.
2. Rebind SYNTH Control on SYNTH's Hotkeys page while settings are open; confirm the pages remain populated and the AI connection is not restarted. Close, release the key, and reopen.
3. Select a chat mode/model. Wait for confirmation, close the menu, and send a new message.
4. Exercise Target/Nearby/Narrator refresh separately. Select Wait Here with Preston and a non-companion; the menu closes automatically. Verify each stops, then use Release Wait to resume. Also test a hostile/combat NPC and the expiry described in `WAIT-ALIAS-CHAT.md`.
5. Reload a save with a menu open or an action pending. Confirm no stale action executes. Recheck voice identity, player TTS, real subtitles and dialogue interruption.
