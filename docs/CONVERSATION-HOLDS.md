# Conversation movement and hearing

The testing candidate no longer installs a temporary Wait Here package just because
an NPC is generating or speaking a reply. Native followers retain their follow AI,
and speaking alone does not restrain another NPC. Speech facing remains
an optional one-shot yaw only for stationary, available actors; moving actors keep
their movement heading. Conversation-only leases already owned in the current
session drain through the existing checked release path. Explicit player Wait Here
commands retain their independent ownership and restraint behavior.

Ordinary typed and voice conversation can address the nearest eligible loaded NPC
inside the configured interior/exterior hearing radius without aiming or facing.
An explicit picked NPC still wins. A retained input never silently retargets on
submission. The same physical-area, identity, availability, hostility, combat,
class, cooldown and save-generation checks remain. Hearing is proximity based;
visual LOS is not an acoustic occlusion measurement. Commands retain their own
pointed/explicit target selection. The ordinary automatic audience includes the
base game's Handy, Protectron, Assaultron, Eyebot and Sentry Bot races, so a nearby
conversational robot does not require a crosshair pick. Feral and animal races
remain excluded by default.

The paired server bounds every NPC rechat responder to 784 game units from the
previous speaker using finite positions in the owned capture, independently of
whether both NPCs were within the player's audience. Missing positions stop the
exchange. Initial idle exchanges use the same mutual bound. This establishes
proximity, not an NPC-to-NPC sight ray or sound propagation through walls.

The optional v2 actor-state boolean `conversation_held` remains supported for
compatibility and recovery. Ordinary restrained actors remain ineligible; this
change does not authorize their movement or speech.

For an ordinary NPC engaged by explicit player text/voice, the flat candidate can
acquire a separate temporary conversation navigation package. The original
`SYNTH_ConversationApproachOwnership` quest (local 0xB00) has eight performer and
eight listener aliases; each performer owns its own original Follow package
(local 0xB10 through 0xB17). The original unconditional template (local 0xB20)
uses the engine's audited Follow procedure, repeats on completion, and follows
the exact live listener alias between 128 and 192 game units. It has no companion
state conditions, warp branch or Go Home branch. Native refresh observes package
ownership without restarting AI or the path each second. Existing native
followers retain their AI without acquiring this package.
NPC rechat and combat barks cannot recruit an actor into player approach.

The globals in the existing source-bound SYNTHWait script drive this separate
quest with scalar IDs and immutable native tickets. Old Wait and Attack entry
points retain their signatures. The approach wrapper verifies exact actor and
listener alias ownership, checks calm/non-follower status before acquire, and
reports actual active package identity. Proximity alone never confirms package
execution. Release and load recovery clear only
these separate aliases and ask the engine to reevaluate native AI. A vanished
subject can still release its exact owned aliases. No script timer, teleport,
teammate/faction edit, restraint, or StopCombat call is used for approach.

Exterior walking may cross loaded cells in the same worldspace while both actors
remain nearby; interiors require the same cell. Combat, unavailability, scene
change, cancellation, Stop, completion and the bounded lease expire navigation.
The engine still decides whether a valid path exists; unreachable actors must
time out and resume their native package rather than teleport.

The deterministic ESP authoring preserves the original Wait records and existing
approach quest, replacing only the eight approach packages and adding the
unconditional template and separate social visit records. The release audit
verifies the exact authored plugin and source-bound PEX. This remains
candidate behavior until flat gameplay proves approach, walking conversation,
Stop/combat/load cleanup, explicit Wait isolation and exterior cell continuity.
VR has no approach adapter and remains separately unproven.

An explicit follower errand uses the separate `SYNTH_SocialVisitOwnership` quest
(local 0xB30) and packages 0xB40 through 0xB47. It follows a canonical NPC listener,
never the player. The game-thread social visit adapter requires a native teammate,
fresh loaded calm actors in the same physical area within 2000 game units, exact
task/execution/save ownership, and a fixed deadline of at most 120 seconds.
Arrival requires the active owned package and distance within 256 game units.
The navigation lease remains owned during the exchange until explicit release;
the adapter itself cannot start NPC dialogue. Client task persistence and backend
authoritative task state own that separate decision.

Visit aliases and callbacks are independent of Wait and player approach. Stop,
combat, scene/area change, timeout, load and an actual player action on the same
performer drain the visit before another action proceeds. A VM timeout revokes
further mutation and waits for its matching completion before alias reuse. Menu
pause does not create a false VM timeout. These contracts need flat gameplay proof,
including deferred return, save branching, unreachable targets and cleanup.
