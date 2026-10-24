# Temporary conversation holds

The flat client requests temporary movement holds only for canonical NPC identities engaged in its current dialogue.
The selected NPC is held while the reply is pending; an explicitly addressed NPC listener joins when the owned response line is admitted.
Bystanders and the player are not held. The existing wait controller supplies saved alias ownership, bounded script tickets,
verification and recovery after a load. An explicit player Wait hold is preserved independently of a conversation hold.

Conversation completion, Stop, cancellation and the existing 90-second ceiling release temporary holds.
A requested action first releases conversation holds and waits for that release to finish before dispatch.
Combat and unavailable or scene-owned actors do not acquire a hold. The wait controller is pumped after connection readiness,
including while reply generation, speech or rechat is active; it no longer waits for the entire conversation pipeline to be idle.

The optional v2 actor-state boolean `conversation_held` identifies a verified SYNTH conversation restraint.
True requires a live, enabled, restrained NPC. An ordinary restrained actor remains ineligible for rechat.
The marker is removed from reusable history projections. Deploy the matching server before enabling the new client.
VR has no conversation-hold adapter and needs separate implementation and headset acceptance.

Gameplay acceptance remains open: both NPCs stay nearby, take turns without overlap, resume normal AI at completion,
and release before an action. Also verify Stop, combat, save/load and unrelated explicit waits.
