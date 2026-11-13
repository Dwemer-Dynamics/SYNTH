SYNTH Fallout 4 VR package

Install this package with a mod manager into Fallout 4 VR Data. It contains only the
SYNTHVR.dll client, a safe loopback-only sample configuration, the GNU GPL v3.0 project
LICENSE, dependency license notices,
and a deterministic file manifest. When built, it also includes the original
SYNTHNative.pex native API bridge, its source, minimal flags and build provenance.
No game scripts or other mods' compiled scripts are included. Shared PEX structure
does not establish VR VM or headset compatibility; those checks remain separate.

Required external installs are not included: Fallout 4 VR 1.2.72, F4SEVR 0.6.21, and VR
Address Library v1.13.1. Runtime compatibility has not been proven until the corresponding
Windows and in-headset acceptance matrix succeeds.

F4SE Menu Framework does not support Fallout 4 VR, so this lane ships no in-game settings
menu, chatbox window or SYNTH Control menu, with no MCM config and no ESP. Every setting
and hotkey is set only in SYNTH_custom.ini.

Hotkeys

All eight keys are DirectInput (DIK) scan codes in the [Hotkeys] section and ship as 0,
which means unbound. The VR client polls the desktop keyboard for them. There is no
controller or headset rebinding surface, and SYNTH does not check INI keys for duplicates
or for conflicts with the game or other mods.

  TalkToNPC       Sends 1-4096 characters of clipboard text
  ToggleVoice     Hold to Talk; also needs [Input] PushToTalkEnabled=true
  StopTalking     Halt AI Actions
  SYNTHControl    Opens the server dashboard in the desktop browser
  OpenMicMute     Open mic mute for this session only; [OpenMic] Muted is the saved value
  ManualActivate  Manual AI Activate for the targeted NPC
  PipVision       Tap, double-tap or hold gesture is dispatched; whether a VR capture is
                  accepted and usable is not yet verified
  WriteDiary      Shared INI key with no VR dispatch; pressing it does nothing in VR

No controller, HMD or in-headset behavior of these keys has been verified.

Do not put secrets in SYNTH.ini. Use a local SYNTH_custom.ini user override and keep it
outside source control and release archives.
