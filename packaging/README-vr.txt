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
menu. Every setting and hotkey is available through SYNTH_custom.ini, with no MCM config
and no ESP.

Do not put secrets in SYNTH.ini. Use a local SYNTH_custom.ini user override and keep it
outside source control and release archives.
