# NPC combat identification and witnessed enemy awareness

The prepared client/server candidate gives a response speaker its own current
native combat target. Nearby hostility to the player cannot identify what that
NPC is fighting. The captured opponent has a canonical actor reference, displayed
name and optional native race label. Names and labels are data, not instructions.

## Capture and compatibility

Flat and VR use their separately pinned CommonLib actor handle APIs. The game
thread copies at most one `currentCombatTarget` handle per bounded observed actor,
including shallow scene actors that may become a prepared rechat responder. No
target inventory, additional discovery scan or engine pointer reaches a worker.
A loaded, live opponent in the same interior cell or adjacent exterior cells of
the same worldspace can be identified. An unresolved, unloaded, dead, disabled,
foreign-area or nameless target is unavailable. An empty current handle is `none`;
neither outcome proves combat ended or an enemy was defeated.

Optional v2 capability `context.combat_targets` must be offered and explicitly
acknowledged with `combat_targets: true` in a complete, correctly correlated init
stream. The client omits the optional field from copied wire states for an older
server. v1 fields remain unchanged. Invalid owner, self/foreign-save identity,
unsupported field and capability combinations are rejected. No historical rows
or enemy identities are backfilled.

The opponent observation never adds a listener, witness, participant or history
reader. It is independent of the bounded player/audience actor-state collection.

## Prompt ownership

The dedicated current-target prompt matches exactly one actor state to the actual
response speaker. This includes a prepared NPC whose identity differs from the
scene focus or parent speaker. Other actors' combat targets are removed from
generic prompt states. All combat-target fields are removed from structured
internal/history payload copies; the immutable source and request-local states
are preserved. Missing, ambiguous and older observations cannot supply a guessed
opponent. Technical form IDs and raw race labels are internal context; the NPC
should use the opponent's captured display name naturally.

The same owned current observation is repeated after history/cues and before the
equipment observer and accepted current input. An identified target takes
precedence over conflicting earlier species guesses. `none` and unavailable
observations explicitly leave the current attackers unidentified: old dialogue,
lore, location, inventory and another actor's selected target cannot supply the
missing identity. An attributed player report remains a report. This does not
infer that the responder saw the player's opponent. The separate flat awareness
extension below provides explicitly witnessed current and recent identities.

## Flat visual enemy awareness

Optional v2 `context.enemy_awareness` requires `context.combat_targets` and
`dialogue.turn_ownership`, and explicit correlated init `enemy_awareness: true`.
Older servers and VR omit this extension. Existing `combat_contacts` retains its
incoming-attacker rules and independent acknowledgement.

The selected live NPC captures at most eight current visible hostile actors,
including threats before they attack and visible enemies attacking a verified
native teammate. Capture requires that NPC's positive visual detection, a live
loaded actor within 2000 units and a compatible area. Neither a nearby reference
nor combat membership by itself proves visual awareness. The current list and
the separate recent list are disjoint and never enroll enemies in the audience,
history readership or conversation participants.

A bounded native cache keeps at most eight recent identities for 30 seconds per
observer, session and generation. `seen_hostile` means previously visually seen
hostile; `seen_dead` requires an earlier known hostile subsequently visually seen
dead. Neither attributes a fight, kill, defeat or victory. The last selected
speaker receives a small background capture once per second while the player is
in combat and for ten seconds afterward; this capture does not read inventories
or quests or send requests. It cannot establish encounters before that NPC was
selected for tracking. Stop, retirement and save/load generation changes clear
the tracking owner and cache; another selected interaction re-arms capture.

The dedicated speaker-owned prompt separates current from recent observations
and is repeated after historical dialogue for normal and prepared NPC routes.
All awareness fields are stripped from generic actor states and structured
history copies, while the immutable snapshot remains unchanged. Current exact
identification questions use the current frame's canonical names without a model
call. The exact recent questions “What were those creatures?”, “What were those
things?” and “What did we just fight?” use recent observations and describe only
what was witnessed; frame elapsed time also counts against the 30-second bound.
Missing, stale, foreign, ambiguous or unsafe evidence remains unidentified.

The observation describes its captured frame. It does not establish other
opponents, faction, weaknesses, kills, victory or current positions. Combat-end
scalar events retain their existing limits.

## Validation and remaining acceptance

Focused engine-free native tests cover target identity, immutable enrichment,
negative identities, both lane encoders, listener isolation and negotiated init
failure/legacy omission. Disposable backend tests cover ingress, exact prepared
speaker ownership, unknown/ambiguous observations, label escaping and generic
state filtering. Alternate trigger history copies retain no target field while
the dedicated current speaker can still use its own observation. The versioned
schema/fixtures are mirrored byte-for-byte.

Synthetic composition tests cover earlier radroach dialogue followed by current
unknown or an own-target `Mole Rat` observation, compact/expanded history,
current-input ordering, supported routes and a prepared responder different from
the scene focus. These checks establish prompt content and ownership; gameplay
must still verify that the provider follows the current evidence.

Gameplay remains open, independently for flat and VR. After reviewed installation:

1. During an NPC's real combat encounter, confirm the reply names that NPC's
   current human or creature opponent from the captured display name. Compare the
   exact captured actor identity with the opponent visible in the game.
2. Let a prepared NPC reply when it differs from the scene focus/parent speaker.
   Confirm it uses its own opponent, including when the player's target differs.
3. Change combat targets and verify a fresh observation follows the change.
   An unresolved target must produce truthful uncertainty; no nearby substitute.
4. Confirm the opponent observation does not expand the conversation audience or
   disclose another NPC's target through generic context, private dialogue or a
   saved/older branch. Save/load and Stop must discard stale generation work.
5. After an earlier encounter with a different species, ask “What are these
   things?” With an own target identified, the responder must name that captured
   target. With `none`/unavailable, it must admit it has not identified the
   attackers. It must not reuse the earlier species or borrow the player's target.
6. In flat, ask about visible hostiles before they attack and while attacking a
   companion. Turn away or obstruct the replying NPC's view and verify identities
   are not invented. Immediately afterward ask one exact recent question, then
   repeat after 30 seconds and after Stop/load. Recent observations must expire
   and must never imply that the player or NPC killed anyone.

No installation, provider call, in-game result or VR headset result is established
by the focused automated checks.
