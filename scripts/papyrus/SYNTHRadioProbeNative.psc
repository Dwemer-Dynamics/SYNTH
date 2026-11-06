Scriptname SYNTHRadioProbeNative extends ScriptObject Hidden

; Copied diagnostics and a bounded primitive mailbox; no actor/scene handles cross this API.
String Function Generation() Global Native
Bool Function Request(Int operation, Int slot, String generation) Global Native
String Function Observation() Global Native
