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
        ReferenceAlias legacyOwner = owner.GetAlias(256 + slot) as ReferenceAlias
        If target && legacyOwner && legacyOwner.GetReference() == None
            ObjectReference previous = target.GetReference()
            If waiting
                If subject && !subject.IsDead() && !subject.IsDisabled() && Current(ticket)
                    If subject.GetParentCell() == startCell && (!previous || previous == subject) && Current(ticket)
                        target.ForceRefTo(subject)
                        If Current(ticket) && target.GetReference() == subject
                            subject.EvaluatePackage(True)
                            success = Current(ticket) && target.GetReference() == subject
                        EndIf
                    EndIf
                EndIf
            ElseIf (!previous || (subject && previous == subject)) && Current(ticket)
                target.Clear()
                If Current(ticket)
                    If subject
                        subject.EvaluatePackage(True)
                    EndIf
                    success = target.GetReference() == None
                EndIf
            EndIf
        EndIf
    EndIf
    ObserveWaitState(ticket, owner, slot, subject)
    Finished(ticket, success)
EndFunction

; Release uses the original numeric identity even after the subject becomes unavailable.
Function ReleaseByFormIds(String ticket, Int slot, Int ownerId, Int subjectId) Global
    Bool success = False
    Quest owner = None
    Actor subject = None
    If Current(ticket) && slot >= 0 && slot < 256
        owner = Game.GetForm(ownerId) as Quest
        subject = Game.GetForm(subjectId) as Actor
        If owner && Current(ticket)
            ReferenceAlias target = owner.GetAlias(slot) as ReferenceAlias
            ReferenceAlias legacyOwner = owner.GetAlias(256 + slot) as ReferenceAlias
            If target && legacyOwner && legacyOwner.GetReference() == None
                ObjectReference previous = target.GetReference()
                If (!previous || previous.GetFormID() == subjectId) && Current(ticket)
                    target.Clear()
                    If Current(ticket)
                        If subject
                            subject.EvaluatePackage(True)
                        EndIf
                        success = target.GetReference() == None && legacyOwner.GetReference() == None
                    EndIf
                EndIf
            EndIf
        EndIf
    EndIf
    ObserveWaitState(ticket, owner, slot, subject)
    Finished(ticket, success)
EndFunction

; Copied exact alias evidence. The native game-thread pump independently checks package and position.
Function ObserveWaitState(String ticket, Quest owner, Int slot, Actor subject) Global
    If !Current(ticket) || !owner || slot < 0 || slot >= 256
        Return
    EndIf
    Int evidenceFlags = 1
    If owner.IsRunning()
        evidenceFlags += 2
    EndIf
    ReferenceAlias target = owner.GetAlias(slot) as ReferenceAlias
    ReferenceAlias restraintOwner = owner.GetAlias(256 + slot) as ReferenceAlias
    If !target || !restraintOwner
        Return
    EndIf
    ObjectReference currentTarget = target.GetReference()
    ObjectReference currentOwner = restraintOwner.GetReference()
    If currentTarget
        evidenceFlags += 256
        If subject && currentTarget == subject
            evidenceFlags += 4
        EndIf
    EndIf
    If currentOwner
        evidenceFlags += 512
        If subject && currentOwner == subject
            evidenceFlags += 128
        EndIf
    EndIf
    Int activeId = 0
    Int sitState = 0
    If subject
        Package active = subject.GetCurrentPackage()
        If active
            activeId = active.GetFormID()
        EndIf
        sitState = subject.GetSitState()
    EndIf
    If Current(ticket)
        Observed(ticket, activeId, 0, evidenceFlags, sitState, -1.0)
    EndIf
EndFunction

; Legacy signature retained so old saved functions remain resolvable.
Bool Function HoldActor(String ticket, Actor subject, ReferenceAlias restraintOwner) Global
    ; Retained only for saved function signatures. New waits never create generic restraint.
    Return False
EndFunction

; Migration/load recovery only: clear an exact receipt from the retired restraint implementation.
Bool Function ReleaseRestraint(String ticket, ReferenceAlias restraintOwner) Global
    If !Current(ticket) || !restraintOwner
        Return False
    EndIf
    Actor previous = restraintOwner.GetActorReference()
    If previous
        If !Current(ticket) || restraintOwner.GetReference() != previous
            Return False
        EndIf
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
    Quest owner = None
    Actor subject = None
    If Current(ticket) && slot >= 0 && slot < 256
        owner = Game.GetForm(ownerId) as Quest
        subject = Game.GetForm(subjectId) as Actor
        If owner && subject && !subject.IsDead() && !subject.IsDisabled() && Current(ticket)
            ReferenceAlias target = owner.GetAlias(slot) as ReferenceAlias
            ReferenceAlias legacyOwner = owner.GetAlias(256 + slot) as ReferenceAlias
            Package hold = Game.GetFormFromFile(0x900 + slot, "SYNTH.esp") as Package
            If target && legacyOwner && hold && target.GetReference() == subject && legacyOwner.GetReference() == None && Current(ticket)
                If subject.GetCurrentPackage() != hold && Current(ticket)
                    subject.EvaluatePackage(True)
                EndIf
                success = Current(ticket) && target.GetReference() == subject
            EndIf
        EndIf
    EndIf
    ObserveWaitState(ticket, owner, slot, subject)
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
                If !target || !restraintOwner
                    Finished(ticket, False)
                    Return
                EndIf
                If !ReleaseRestraint(ticket, restraintOwner)
                    Finished(ticket, False)
                    Return
                EndIf
                If target && Current(ticket)
                    Actor previous = target.GetActorReference()
                    target.Clear()
                    If previous && Current(ticket)
                        previous.EvaluatePackage(True)
                    EndIf
                EndIf
                If !Current(ticket) || target.GetReference() != None || restraintOwner.GetReference() != None
                    Finished(ticket, False)
                    Return
                EndIf
                slot += 1
            EndWhile
            success = slot == 256 && Current(ticket)
        EndIf
    EndIf
    Finished(ticket, success)
EndFunction

; Bounded action wrapper, independent of wait aliases. Existing saved functions retain their signatures.
Bool Function CurrentAttack(String ticket) Global Native
Function FinishedAttack(String ticket, Bool success) Global Native

Function AttackByFormIds(String ticket, Int actorId, Int targetId) Global
    If !CurrentAttack(ticket)
        Return
    EndIf
    Actor subject = Game.GetForm(actorId) as Actor
    Actor target = Game.GetForm(targetId) as Actor
    If subject && target && subject != target && subject != Game.GetPlayer() && target != Game.GetPlayer()
        If !subject.IsDead() && !target.IsDead() && !subject.IsDisabled() && !target.IsDisabled()
            If subject.IsPlayerTeammate() && (target.IsHostileToActor(subject) || target.IsHostileToActor(Game.GetPlayer())) && subject.GetDistance(target) <= 2000.0 && CurrentAttack(ticket)
                subject.StartCombat(target, False)
                FinishedAttack(ticket, True)
                Return
            EndIf
        EndIf
    EndIf
    FinishedAttack(ticket, False)
EndFunction

; Conversation navigation has separate original quest/alias ownership from saved waits.
; Primitive callbacks carry no borrowed engine pointers. Native tickets expire/cancel on load.
Bool Function CurrentApproach(String ticket) Global Native
Function FinishedApproach(String ticket, Bool success) Global Native
Function ObservedApproach(String ticket, Int activePackageId, Bool aliasOwned, Bool listenerOwned) Global Native

Function ApproachByFormIds(String ticket, Int questId, Int slot, Int subjectId, Int listenerId, Bool acquire) Global
    Bool success = False
    If CurrentApproach(ticket) && slot >= 0 && slot < 8
        Quest owner = Game.GetForm(questId) as Quest
        Actor subject = Game.GetForm(subjectId) as Actor
        Actor listener = Game.GetForm(listenerId) as Actor
        If owner && listener == Game.GetPlayer() && (subject || !acquire) && subject != listener
            If !owner.IsRunning() && CurrentApproach(ticket)
                owner.Start()
            EndIf
            If owner.IsRunning() && CurrentApproach(ticket)
                ReferenceAlias performerAlias = owner.GetAlias(slot) as ReferenceAlias
                ReferenceAlias listenerAlias = owner.GetAlias(8 + slot) as ReferenceAlias
                If performerAlias && listenerAlias
                    ObjectReference previous = performerAlias.GetReference()
                    ObjectReference previousListener = listenerAlias.GetReference()
                    If acquire
                        If (!previous || previous == subject) && (!previousListener || previousListener == listener)
                            If !subject.IsDead() && !subject.IsDisabled() && !subject.IsInCombat() && !subject.IsPlayerTeammate()
                                If ApproachSameArea(subject, listener) && subject.GetDistance(listener) <= 2000.0 && CurrentApproach(ticket)
                                    Bool newOwner = previous != subject || previousListener != listener
                                    If newOwner
                                        listenerAlias.ForceRefTo(listener)
                                        If CurrentApproach(ticket) && listenerAlias.GetReference() == listener
                                            performerAlias.ForceRefTo(subject)
                                            If CurrentApproach(ticket) && performerAlias.GetReference() == subject
                                                subject.EvaluatePackage(True)
                                            EndIf
                                        EndIf
                                    EndIf
                                    ; An owned refresh observes continuous Follow; it must not reset its AI/path each second.
                                    success = CurrentApproach(ticket) && performerAlias.GetReference() == subject && listenerAlias.GetReference() == listener
                                EndIf
                            EndIf
                        EndIf
                    ElseIf (!previous || previous.GetFormID() == subjectId) && (!previousListener || previousListener == listener) && CurrentApproach(ticket)
                        ; Never clear another actor's alias or change its vanilla package stack directly.
                        performerAlias.Clear()
                        If CurrentApproach(ticket) && performerAlias.GetReference() == None
                            listenerAlias.Clear()
                            If CurrentApproach(ticket)
                                If subject
                                    subject.EvaluatePackage(True)
                                EndIf
                                success = performerAlias.GetReference() == None && listenerAlias.GetReference() == None
                            EndIf
                        EndIf
                    EndIf
                    If CurrentApproach(ticket)
                        Int activeId = 0
                        If subject
                            Package active = subject.GetCurrentPackage()
                            If active
                                activeId = active.GetFormID()
                            EndIf
                        EndIf
                        Bool aliasOwned = False
                        ObjectReference current = performerAlias.GetReference()
                        If current
                            aliasOwned = current.GetFormID() == subjectId
                        EndIf
                        ObservedApproach(ticket, activeId, aliasOwned, listenerAlias.GetReference() == listener)
                    EndIf
                EndIf
            EndIf
        EndIf
    EndIf
    FinishedApproach(ticket, success)
EndFunction

; Called before admitting a new lease after load/new game. Only our separate aliases are cleared.
Function ResetApproachByFormId(String ticket, Int questId) Global
    Bool success = False
    If CurrentApproach(ticket)
        Quest owner = Game.GetForm(questId) as Quest
        If owner
            If !owner.IsRunning() && CurrentApproach(ticket)
                owner.Start()
            EndIf
            If owner.IsRunning() && CurrentApproach(ticket)
                Int slot = 0
                While slot < 8 && CurrentApproach(ticket)
                    ReferenceAlias performerAlias = owner.GetAlias(slot) as ReferenceAlias
                    ReferenceAlias listenerAlias = owner.GetAlias(8 + slot) as ReferenceAlias
                    If !performerAlias || !listenerAlias
                        FinishedApproach(ticket, False)
                        Return
                    EndIf
                    Actor previous = performerAlias.GetActorReference()
                    If CurrentApproach(ticket)
                        performerAlias.Clear()
                    EndIf
                    If CurrentApproach(ticket) && performerAlias.GetReference() == None
                        listenerAlias.Clear()
                        If previous && CurrentApproach(ticket)
                            previous.EvaluatePackage(True)
                        EndIf
                    Else
                        FinishedApproach(ticket, False)
                        Return
                    EndIf
                    If performerAlias.GetReference() != None || listenerAlias.GetReference() != None
                        FinishedApproach(ticket, False)
                        Return
                    EndIf
                    slot += 1
                EndWhile
                success = slot == 8 && CurrentApproach(ticket)
            EndIf
        EndIf
    EndIf
    FinishedApproach(ticket, success)
EndFunction

Bool Function ApproachSameArea(Actor subject, Actor listener) Global
    If !subject || !listener
        Return False
    EndIf
    Cell source = subject.GetParentCell()
    Cell target = listener.GetParentCell()
    If !source || !target
        Return False
    EndIf
    If source == target
        Return True
    EndIf
    Return !source.IsInterior() && !target.IsInterior() && subject.GetWorldSpace() && subject.GetWorldSpace() == listener.GetWorldSpace()
EndFunction

; Explicit saved follower errands use a separate original quest and exact NPC listener aliases.
Bool Function CurrentVisit(String ticket) Global Native
Function FinishedVisit(String ticket, Bool success) Global Native
Function ObservedVisit(String ticket, Int activePackageId, Bool aliasOwned, Bool listenerOwned) Global Native

Function VisitByFormIds(String ticket, Int questId, Int slot, Int subjectId, Int listenerId, Bool acquire) Global
    Bool success = False
    If CurrentVisit(ticket) && slot >= 0 && slot < 8
        Quest owner = Game.GetForm(questId) as Quest
        Actor subject = Game.GetForm(subjectId) as Actor
        Actor listener = Game.GetForm(listenerId) as Actor
        If owner && (!acquire || (subject && listener && subject != listener && subject != Game.GetPlayer() && listener != Game.GetPlayer()))
            If !owner.IsRunning() && CurrentVisit(ticket)
                owner.Start()
            EndIf
            If owner.IsRunning() && CurrentVisit(ticket)
                ReferenceAlias performerAlias = owner.GetAlias(slot) as ReferenceAlias
                ReferenceAlias listenerAlias = owner.GetAlias(8 + slot) as ReferenceAlias
                If performerAlias && listenerAlias
                    ObjectReference previous = performerAlias.GetReference()
                    ObjectReference previousListener = listenerAlias.GetReference()
                    If acquire
                        If (!previous || previous == subject) && (!previousListener || previousListener == listener)
                            If !subject.IsDead() && !subject.IsDisabled() && !subject.IsInCombat() && subject.IsPlayerTeammate() && !listener.IsDead() && !listener.IsDisabled() && !listener.IsInCombat() && !subject.IsHostileToActor(listener) && !listener.IsHostileToActor(subject)
                                If ApproachSameArea(subject, listener) && subject.GetDistance(listener) <= 2000.0 && CurrentVisit(ticket)
                                    Bool newOwner = previous != subject || previousListener != listener
                                    If newOwner
                                        listenerAlias.ForceRefTo(listener)
                                        If CurrentVisit(ticket) && listenerAlias.GetReference() == listener
                                            performerAlias.ForceRefTo(subject)
                                            If CurrentVisit(ticket) && performerAlias.GetReference() == subject
                                                subject.EvaluatePackage(True)
                                            EndIf
                                        EndIf
                                    EndIf
                                    ; An owned refresh observes continuous Follow; it must not reset its AI/path each second.
                                    success = CurrentVisit(ticket) && performerAlias.GetReference() == subject && listenerAlias.GetReference() == listener
                                EndIf
                            EndIf
                        EndIf
                    ElseIf (!previous || previous.GetFormID() == subjectId) && (!previousListener || previousListener.GetFormID() == listenerId) && CurrentVisit(ticket)
                        ; Never clear another actor's alias or change its vanilla package stack directly.
                        performerAlias.Clear()
                        If CurrentVisit(ticket) && performerAlias.GetReference() == None
                            listenerAlias.Clear()
                            If CurrentVisit(ticket)
                                If subject
                                    subject.EvaluatePackage(True)
                                EndIf
                                success = performerAlias.GetReference() == None && listenerAlias.GetReference() == None
                            EndIf
                        EndIf
                    EndIf
                    If CurrentVisit(ticket)
                        Int activeId = 0
                        If subject
                            Package active = subject.GetCurrentPackage()
                            If active
                                activeId = active.GetFormID()
                            EndIf
                        EndIf
                        Bool aliasOwned = False
                        ObjectReference current = performerAlias.GetReference()
                        If current
                            aliasOwned = current.GetFormID() == subjectId
                        EndIf
                        Bool listenerOwned = False
                        ObjectReference currentListener = listenerAlias.GetReference()
                        If currentListener
                            listenerOwned = currentListener.GetFormID() == listenerId
                        EndIf
                        ObservedVisit(ticket, activeId, aliasOwned, listenerOwned)
                    EndIf
                EndIf
            EndIf
        EndIf
    EndIf
    FinishedVisit(ticket, success)
EndFunction

; Called before admitting a new lease after load/new game. Only our separate aliases are cleared.
Function ResetVisitByFormId(String ticket, Int questId) Global
    Bool success = False
    If CurrentVisit(ticket)
        Quest owner = Game.GetForm(questId) as Quest
        If owner
            If !owner.IsRunning() && CurrentVisit(ticket)
                owner.Start()
            EndIf
            If owner.IsRunning() && CurrentVisit(ticket)
                Int slot = 0
                While slot < 8 && CurrentVisit(ticket)
                    ReferenceAlias performerAlias = owner.GetAlias(slot) as ReferenceAlias
                    ReferenceAlias listenerAlias = owner.GetAlias(8 + slot) as ReferenceAlias
                    If !performerAlias || !listenerAlias
                        FinishedVisit(ticket, False)
                        Return
                    EndIf
                    Actor previous = performerAlias.GetActorReference()
                    If CurrentVisit(ticket)
                        performerAlias.Clear()
                    EndIf
                    If CurrentVisit(ticket) && performerAlias.GetReference() == None
                        listenerAlias.Clear()
                        If previous && CurrentVisit(ticket)
                            previous.EvaluatePackage(True)
                        EndIf
                    Else
                        FinishedVisit(ticket, False)
                        Return
                    EndIf
                    If performerAlias.GetReference() != None || listenerAlias.GetReference() != None
                        FinishedVisit(ticket, False)
                        Return
                    EndIf
                    slot += 1
                EndWhile
                success = slot == 8 && CurrentVisit(ticket)
            EndIf
        EndIf
    EndIf
    FinishedVisit(ticket, success)
EndFunction


; A general gift opens the engine's normal free inventory exchange. Admission is not a completed gift.
Bool Function CurrentGiftMenu(String ticket) Global Native
Function FinishedGiftMenu(String ticket, Bool admitted) Global Native

Function GiftMenuByFormIds(String ticket, Int subjectId, Int cellId) Global
    Bool admitted = False
    If CurrentGiftMenu(ticket)
        Actor subject = Game.GetForm(subjectId) as Actor
        Actor player = Game.GetPlayer()
        Cell startCell = Game.GetForm(cellId) as Cell
        If subject && player && startCell && subject != player && CurrentGiftMenu(ticket)
            If !subject.IsDead() && !subject.IsDisabled() && !subject.IsInCombat() && !player.IsInCombat()
                If subject.GetParentCell() == startCell && player.GetParentCell() == startCell && CurrentGiftMenu(ticket)
                    subject.OpenInventory(True)
                    admitted = True
                EndIf
            EndIf
        EndIf
    EndIf
    FinishedGiftMenu(ticket, admitted)
EndFunction
