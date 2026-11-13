SYNTH Fallout 4 package

Install this package with a mod manager into Fallout 4 Data. It contains only the
SYNTH.dll client, a safe loopback-only sample configuration, the GNU GPL v3.0 project
LICENSE, dependency license notices,
an original SYNTH.esp NPC wait package, and a deterministic file manifest. Enable
SYNTH.esp for Wait Here and Release Wait. When built, it also includes the original
SYNTHNative.pex native API bridge, its source, minimal flags and build provenance.
No game scripts or other mods' compiled scripts are included. Script VM calls
still require separate in-game validation.

Required external installs are not included: a matching Fallout 4 runtime and matching
F4SE runtime. Runtime compatibility has not been proven until the corresponding Windows
and in-game acceptance matrix succeeds.

F4SE Menu Framework is optional. When it is installed, SYNTH registers its own native
Hotkeys, Auto Activate, Behavior, Sound, and Tools pages, the native chatbox with its To:
NPC picker, and the SYNTH Control menu. SYNTH ships no MCM config and never appears under
"MCM Mod Configs (Legacy)". Without the framework, every setting and hotkey remains
available through SYNTH_custom.ini, the Chatbox key sends clipboard text instead of
opening a window, and the SYNTH Control menu is unavailable.

Hotkeys

All eight keys are DirectInput (DIK) scan codes in the [Hotkeys] section and ship as 0,
which means unbound. Change them on SYNTH's own Hotkeys page or in SYNTH_custom.ini.
SYNTH reads only its INI chain: editing a SYNTH entry on the framework's generic Hotkeys
page does not change the key SYNTH uses. The framework refuses a key on SYNTH's page only
when another framework hotkey already uses it; duplicate keys typed into the INI, and
keys used by the game or other mods, may not be detected.

  TalkToNPC       Chatbox: framework window, or clipboard text without the framework
  ToggleVoice     Hold to Talk; also needs [Input] PushToTalkEnabled=true
  StopTalking     Halt AI Actions
  SYNTHControl    SYNTH Control menu; requires the framework
  OpenMicMute     Open mic mute for this session only; [OpenMic] Muted is the saved value
  ManualActivate  Manual AI Activate for the targeted NPC
  PipVision       Tap, double-tap or hold PipVision gesture
  WriteDiary      Request a diary entry for the NPC under the crosshair

Narrator and Injection Log chat modes address no NPC, so the chatbox, Hold to Talk and
open mic do not select one in those modes.

Do not put secrets in SYNTH.ini. Use a local SYNTH_custom.ini user override and keep it
outside source control and release archives.
