Scriptname SYNTHRadioProbe extends Quest

; Optional console controls for our original diagnostic station only.
; No OnInit/OnLoad start, timer loop, forced player tuning or DCR edits.
Bool Function EnableProbe()
    String requestGeneration = SYNTHRadioProbeNative.Generation()
    If !StartProbeAtGeneration(requestGeneration)
        Return False
    EndIf
    If SYNTHRadioProbeNative.Generation() != requestGeneration
        ReportProbe("SYNTH radio: generation changed before enable. No enable request was queued.")
        Return False
    EndIf
    Bool queued = SYNTHRadioProbeNative.Request(0, 0, requestGeneration)
    If queued
        ReportProbe("SYNTH radio: enable request queued. Close the console.")
    Else
        ReportProbe("SYNTH radio: enable request rejected. Close the console and check ShowStatus.")
    EndIf
    Return queued
EndFunction

Bool Function StartProbe()
    Return StartProbeAtGeneration(SYNTHRadioProbeNative.Generation())
EndFunction

Bool Function StartProbeAtGeneration(String requestGeneration)
    If requestGeneration == "" || SYNTHRadioProbeNative.Generation() != requestGeneration
        ReportProbe("SYNTH radio: native generation is unavailable or changed. No enable request was queued.")
        Return False
    EndIf
    Quest expectedQuest = Game.GetFormFromFile(0x800, "SYNTHRadioDiagnostics.esp") as Quest
    ObjectReference expectedTransmitter = Game.GetFormFromFile(0x803, "SYNTHRadioDiagnostics.esp") as ObjectReference
    Activator expectedTransmitterBase = Game.GetFormFromFile(0x801, "SYNTHRadioDiagnostics.esp") as Activator
    If !expectedQuest || expectedQuest != Self || !expectedTransmitter || !expectedTransmitterBase
        ReportProbe("SYNTH radio: installed quest, transmitter or base is missing or mismatched. No enable request was queued.")
        Return False
    EndIf
    If expectedTransmitter.GetBaseObject() != expectedTransmitterBase
        ReportProbe("SYNTH radio: installed transmitter has an unexpected base. No enable request was queued.")
        Return False
    EndIf
    ReferenceAlias transmitterAlias = GetAlias(0) as ReferenceAlias
    ObjectReference currentTransmitter = None
    If transmitterAlias
        currentTransmitter = transmitterAlias.GetReference()
        If currentTransmitter && currentTransmitter != expectedTransmitter
            ReportProbe("SYNTH radio: transmitter alias contains another reference. No enable request was queued.")
            Return False
        EndIf
    EndIf
    If SYNTHRadioProbeNative.Generation() != requestGeneration
        ReportProbe("SYNTH radio: generation changed before quest startup. No enable request was queued.")
        Return False
    EndIf
    If !IsRunning()
        ReportProbe("SYNTH radio: starting diagnostic quest. Close the console.")
        ; Start is latent and returns after quest startup, including startup fragments.
        If !Start()
            ReportProbe("SYNTH radio: quest Start failed. No enable request was queued.")
            Return False
        EndIf
    EndIf
    If SYNTHRadioProbeNative.Generation() != requestGeneration
        ReportProbe("SYNTH radio: generation changed during quest startup. No alias was assigned or enable request queued.")
        Return False
    EndIf
    If !IsRunning()
        ReportProbe("SYNTH radio: quest is not running after Start. No enable request was queued.")
        Return False
    EndIf
    ; Resolve the alias again after latent startup. An optional, empty alias can start
    ; without engine fill; assign only our exact installed transmitter afterwards.
    transmitterAlias = GetAlias(0) as ReferenceAlias
    If !transmitterAlias
        ReportProbe("SYNTH radio: transmitter alias is missing after Start. No enable request was queued.")
        Return False
    EndIf
    currentTransmitter = transmitterAlias.GetReference()
    If currentTransmitter && currentTransmitter != expectedTransmitter
        ReportProbe("SYNTH radio: transmitter alias contains another reference. No enable request was queued.")
        Return False
    EndIf
    If SYNTHRadioProbeNative.Generation() != requestGeneration
        ReportProbe("SYNTH radio: generation changed before alias assignment. No alias was assigned or enable request queued.")
        Return False
    EndIf
    If !currentTransmitter
        transmitterAlias.ForceRefTo(expectedTransmitter)
    EndIf
    If SYNTHRadioProbeNative.Generation() != requestGeneration
        ReportProbe("SYNTH radio: generation changed after alias assignment. No enable request was queued.")
        Return False
    EndIf
    If !IsRunning() || transmitterAlias.GetReference() != expectedTransmitter
        ReportProbe("SYNTH radio: quest or owned transmitter alias is not ready after assignment. No enable request was queued.")
        Return False
    EndIf
    ReportProbe("SYNTH radio: quest is running and the owned transmitter alias is ready.")
    Return True
EndFunction

Bool Function ShowStatus()
    String questState = "stopped"
    If IsRunning()
        questState = "running"
    ElseIf IsStarting()
        questState = "starting"
    ElseIf IsStopping()
        questState = "stopping"
    EndIf
    Bool aliasReady = False
    ReferenceAlias transmitterAlias = GetAlias(0) as ReferenceAlias
    ObjectReference expectedTransmitter = Game.GetFormFromFile(0x803, "SYNTHRadioDiagnostics.esp") as ObjectReference
    If transmitterAlias && expectedTransmitter
        aliasReady = transmitterAlias.GetReference() == expectedTransmitter
    EndIf
    ReportProbe("SYNTH radio: quest " + questState + "; owned transmitter alias ready=" + aliasReady + ".")
    Debug.Trace("SYNTH radio native observation: " + SYNTHRadioProbeNative.Observation(), 0)
    Return IsRunning() && aliasReady
EndFunction

Bool Function PlayA()
    Return SubmitProbe(1, 0, "play A")
EndFunction

Bool Function PlayB()
    Return SubmitProbe(1, 1, "play B")
EndFunction

Bool Function StopProbe()
    Return SubmitProbe(2, 0, "stop")
EndFunction

Bool Function DisableProbe()
    Return SubmitProbe(3, 0, "disable")
EndFunction

Bool Function Observe()
    Return SubmitProbe(4, 0, "observe")
EndFunction

Bool Function SubmitProbe(Int operation, Int slot, String label)
    Bool queued = SYNTHRadioProbeNative.Request(operation, slot, SYNTHRadioProbeNative.Generation())
    If queued
        ReportProbe("SYNTH radio: " + label + " request queued. Close the console.")
    Else
        ReportProbe("SYNTH radio: " + label + " request rejected. Close the console and check ShowStatus.")
    EndIf
    Return queued
EndFunction

Function ReportProbe(String messageText)
    Debug.Notification(messageText)
    Debug.Trace(messageText, 0)
EndFunction
