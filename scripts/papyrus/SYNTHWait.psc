Scriptname SYNTHWait extends ScriptObject Hidden

; Native callbacks use copied scalars only, not borrowed game objects.
Bool Function Current(String ticket) Global Native
Function Finished(String ticket, Bool success) Global Native
Function Observed(String ticket, Int active, Int expected, Int evidenceFlags, Int sitState, Float distance) Global Native

; Native entry points accept only values. The engine resolves forms, not CommonLib's object packer.
; Keep the old typed functions below unchanged so saved stacks retain their signatures.
Function ApplyByFormIds(String ticket, Int slot, Int ownerId, Int subjectId, Int anchorId, Int cellId, Float x, Float y, Float z, Bool waiting) Global
    If Current(ticket)
        Quest owner = Game.GetForm(ownerId) as Quest
        Actor subject = Game.GetForm(subjectId) as Actor
        ObjectReference anchor = Game.GetForm(anchorId) as ObjectReference
        Cell startCell = Game.GetForm(cellId) as Cell
        Apply(ticket, slot, owner, subject, anchor, startCell, x, y, z, waiting)
    Else
        Finished(ticket, False)
    EndIf
EndFunction

Function ResetByFormId(String ticket, Int ownerId) Global
    If Current(ticket)
        Quest owner = Game.GetForm(ownerId) as Quest
        Reset(ticket, owner)
    Else
        Finished(ticket, False)
    EndIf
EndFunction

; Serialized by the native coordinator. Recheck the ticket after every latent operation.
Function Apply(String ticket, Int slot, Quest owner, Actor subject, ObjectReference anchor, Cell startCell, Float x, Float y, Float z, Bool waiting) Global
    Bool success = False
    If Current(ticket) && owner && slot >= 0 && slot < 256
        ReferenceAlias target = owner.GetAlias(slot) as ReferenceAlias
        ReferenceAlias restraintOwner = owner.GetAlias(256 + slot) as ReferenceAlias
        If target && restraintOwner
            If waiting
                If subject && !subject.IsDead() && !subject.IsDisabled() && Current(ticket)
                    If Current(ticket) && subject.GetParentCell() == startCell
                        target.ForceRefTo(subject)
                        subject.StopCombat()
                        success = HoldActor(ticket, subject, restraintOwner)
                        success = success && target.GetReference() == subject
                    EndIf
                EndIf
            ElseIf Current(ticket)
                Actor previous = target.GetActorReference()
                If ReleaseRestraint(ticket, restraintOwner) && Current(ticket)
                    target.Clear()
                    If previous
                        previous.EvaluatePackage(True)
                    EndIf
                    success = target.GetReference() == None
                EndIf
            EndIf
        EndIf
    EndIf
    Finished(ticket, success)
EndFunction

; Reused by install and refresh. False from SetRestrained means no state transition.
; Record only changes we made, so an already-restrained actor retains its original state.
Bool Function HoldActor(String ticket, Actor subject, ReferenceAlias restraintOwner) Global
    If !Current(ticket) || !subject || !restraintOwner
        Return False
    EndIf
    Actor previous = restraintOwner.GetActorReference()
    If previous && previous != subject
        Return False
    EndIf
    If subject.SetRestrained(True)
        ; A saved/cancelled stack must not assign a receipt to a reused alias.
        If !Current(ticket)
            subject.SetRestrained(False)
            Return False
        EndIf
        restraintOwner.ForceRefTo(subject)
        If restraintOwner.GetReference() != subject
            subject.SetRestrained(False)
            Return False
        EndIf
    EndIf
    ; The game-thread coordinator independently checks the resulting life state.
    Return Current(ticket)
EndFunction

; Reused by explicit release and load recovery; unrelated restraint is not ours to clear.
Bool Function ReleaseRestraint(String ticket, ReferenceAlias restraintOwner) Global
    If !Current(ticket) || !restraintOwner
        Return False
    EndIf
    Actor previous = restraintOwner.GetActorReference()
    If previous
        previous.SetRestrained(False)
        If Current(ticket) && restraintOwner.GetReference() == previous
            restraintOwner.Clear()
        Else
            Return False
        EndIf
    EndIf
    Return restraintOwner.GetReference() == None
EndFunction

; Check actual package ownership; comments/bump packages must not silently end the hold.
Function RefreshByFormIds(String ticket, Int ownerId, Int slot, Int subjectId) Global
    Bool success = False
    If Current(ticket) && slot >= 0 && slot < 256
        Quest owner = Game.GetForm(ownerId) as Quest
        Actor subject = Game.GetForm(subjectId) as Actor
        If owner && subject && !subject.IsDead() && !subject.IsDisabled() && Current(ticket)
            Int evidenceFlags = 1
            If owner.IsRunning()
                evidenceFlags += 2
            EndIf
            ReferenceAlias target = owner.GetAlias(slot) as ReferenceAlias
            ReferenceAlias restraintOwner = owner.GetAlias(256 + slot) as ReferenceAlias
            Package hold = Game.GetFormFromFile(0x900 + slot, "SYNTH.esp") as Package
            Int expectedId = 0
            If hold
                evidenceFlags += 8
                expectedId = hold.GetFormID()
            EndIf
            If target && target.GetReference() == subject
                evidenceFlags += 4
            EndIf
            If target && target.GetReference() == subject && Current(ticket)
                success = HoldActor(ticket, subject, restraintOwner)
                If restraintOwner && restraintOwner.GetReference() == subject
                    evidenceFlags += 128
                EndIf
            EndIf
            ObjectReference anchor = Game.GetFormFromFile(0xA00 + slot, "SYNTH.esp") as ObjectReference
            Float distance = -1.0
            If anchor
                evidenceFlags += 16
                If anchor.GetParentCell() == subject.GetParentCell()
                    evidenceFlags += 32
                    distance = anchor.GetDistance(subject)
                    If distance <= 64.0
                        evidenceFlags += 64
                    EndIf
                EndIf
            EndIf
            Package active = subject.GetCurrentPackage()
            Int activeId = 0
            If active
                activeId = active.GetFormID()
            EndIf
            If Current(ticket)
                Observed(ticket, activeId, expectedId, evidenceFlags, subject.GetSitState(), distance)
            EndIf
        EndIf
    EndIf
    Finished(ticket, success)
EndFunction

; New-process/load cleanup completes before any new wait. No repeating Papyrus timer.
Function Reset(String ticket, Quest owner) Global
    Bool success = False
    If Current(ticket) && owner
        If !owner.IsRunning()
            owner.Start()
        EndIf
        If Current(ticket) && owner.IsRunning()
            Int slot = 0
            While slot < 256 && Current(ticket)
                ReferenceAlias target = owner.GetAlias(slot) as ReferenceAlias
                ReferenceAlias restraintOwner = owner.GetAlias(256 + slot) as ReferenceAlias
                If !ReleaseRestraint(ticket, restraintOwner)
                    Finished(ticket, False)
                    Return
                EndIf
                If target
                    Actor previous = target.GetActorReference()
                    If previous
                        target.Clear()
                        previous.EvaluatePackage(True)
                    EndIf
                EndIf
                slot += 1
            EndWhile
            success = slot == 256 && Current(ticket)
        EndIf
    EndIf
    Finished(ticket, success)
EndFunction
