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
Hotkeys, Auto Activate, Behavior, Sound, and Tools pages and publishes its hotkeys to the
framework hotkey registry, so they take part in framework-wide conflict detection. SYNTH
ships no MCM config and never appears under "MCM Mod Configs (Legacy)". Without the
framework, every setting and hotkey remains available through SYNTH_custom.ini.

Do not put secrets in SYNTH.ini. Use a local SYNTH_custom.ini user override and keep it
outside source control and release archives.
