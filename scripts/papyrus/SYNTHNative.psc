Scriptname SYNTHNative Extends ScriptObject Hidden

; Queue availability is not a promise that a particular request can execute.
Bool Function IsAvailable() Global Native

; Pass the intended live actor's GetFormID(). Never persist the ID across saves.
; Result: 0 queued, 1 invalid, 2 unavailable, 3 stale epoch, 4 busy queue.
Int Function SpeakExact(Int aiActorFormID, String asText) Global Native
Int Function Comment(Int aiActorFormID) Global Native
Int Function React(Int aiActorFormID, String asDirection) Global Native
Int Function Ask(Int aiActorFormID, String asQuestion) Global Native
Int Function OpenPrompt(Int aiActorFormID) Global Native
