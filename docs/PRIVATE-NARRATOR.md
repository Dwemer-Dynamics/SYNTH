# Private narrator

Create named definitions under **Private Narrators** on the server Narrator page. A definition has a name,
personality, background, speaking style, voice and optional profile. Saving an edit creates an immutable revision.
The profile supplies the model and voice provider; a blank profile uses the server's default narrator profile.

Open SYNTH chat and type `/narrator` for help:

- `/narrator list` lists available definitions.
- `/narrator select <number or name>` applies the library's current revision and selects it.
- `/narrator talk` directs typed chat and push-to-talk to the selected private narrator.
- `/narrator npcs` returns to ordinary NPC input.
- `/narrator scene` requests private commentary on the captured scene.
- `/narrator transcript` opens the latest transcript page in chat; `/narrator older` opens the previous page.
- `/narrator cancel` stops the current private reply.

An acknowledged context checkpoint captures the applied revisions, selection and transcript branch. The next
game save stores that existing checkpoint. A save made before the latest acknowledgment keeps its older state.
Loading restores the saved state; newer library edits remain available to apply explicitly. A save without private
narrator state starts empty. A save can apply up to 16 definitions; earlier applied revisions are never discarded.

Player input is recorded before the model request. A narrator reply enters the transcript only after the client
reports completed audio for the exact session, turn, definition revision and parent state. Cancellation, editing,
selection changes and session replacement invalidate pending output. Text-only presentation is not proof of audio.
Provider settings and profile contents are read live. Voice input uses the definition, private history and authored
player background; typed turns may add copied scene facts. Private content stays outside NPC history and journals.

This capability is separate from the legacy **Narrator** chat mode. Older servers keep that mode and refuse the
private controls. Flat gameplay, VR headset presentation and actual microphone/provider quality require separate
acceptance; builds and disposable HTTP tests do not establish those results.
