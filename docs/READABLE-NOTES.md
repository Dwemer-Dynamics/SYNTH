# Readable paper notes (flat only)

When the player opens a paper note on flat Fallout 4, SYNTH records it privately for the player.
The player can then type `/share-note` to share their last note with the NPC they are talking to.
Nothing is shared automatically. The native client never writes a body to its diagnostic log. The
server still stores the typed canonical event and its catalogue row, and a shared body reaches the
recipient's authorized provider prompt.

Status: native capture, client delivery and the protocol are done. Off-game tests cover them, and
both lanes compile for x64. The compiled client core has run against the real server over loopback
HTTP with disposable PostgreSQL, using scratch callbacks and injected reads; that is not native or game
proof. Nothing has run in the game. Combined runtime integration with the observed vanilla dialogue
capture is untested and held.

## Why not `TESBookReadEvent`

A static audit of flat 1.11.240 found two producers of that event. One is the BookMenu
open path. The other is add-to-container (pickup/AddItem), which runs while BookMenu
is closed. So the event cannot tell an open from a pickup and is not used.

## Binding (flat 1.11.240 only)

`src/adapters/flat_readable_notes.hpp` rewrites the only two direct callers of the
BookMenu open-population function (ID 2248510) that the audit found:

- world activation: call at RVA 0x4657F0 inside ID 2198604
- inventory/Pip-Boy read: call at RVA 0xAE252B inside ID 2224168

Before patching it verifies the runtime version, the address-library RVAs of the
function, both callers and the BOOK/body statics (IDs 2701336/2701339), the exact
call bytes at both sites and room for one 14-byte branch in the shared 64-byte trampoline.
Before writing any game code it reserves one stub that both sites share. The pinned
`allocate_branch5` keys it by our thunk. The stub must start at the trampoline cursor
reported just before the allocation, grow the allocation by exactly 14 bytes and hold the
complete known `jmp [rip+0]` stub bytes and destination. Both `call rel32` encodings must
reach it. Only then are the sites written.

If a write fails or does not read back as our call, rollback looks at every site written so far.
It restores a site only when that site still holds our exact call and the stub still holds our
exact bytes. A site holding anything else is kept, and the log reports the rollback as incomplete.
Each restore is read back. Capture is never offered after a failed install.

Ownership means the complete recorded 5-byte call at each site plus the complete 14-byte stub.
Arbitrary jump targets are never followed. The first pump that sees different bytes disables
capture for the rest of the process. From then on the hook only forwards. The wrapper keeps the
original eight arguments and calls the original exactly once on every path. Callers added by mods,
or indirect callers, are not captured. Those reads are missed, never misattributed.

The two callsites are known, but the hook does not authenticate which one ran, or the
reference argument. So the admitted read contains only the BOOK form ID and body.

## Admission (`src/core/readable_note_gate.hpp`, engine-free)

1. Every hooked open attempt advances an atomic epoch first, on any thread, before
   touching any engine object. This also covers attempts that are then rejected.
   Lifecycle invalidation and ownership loss advance the same epoch.
2. Only the main thread copies. Its ID is cached by a verified game-thread pump, so a
   wrong-thread hook needs no engine query to decide. The menu sink is registered in that
   same pump. Attempts before registration are not captured.
3. At the attempt, the hook checks the actual `BookMenu` stack state. If the menu is
   already on stack, the state is ambiguous and nothing is staged. The hook copies the
   BOOK form ID (0 or 0xFF-prefixed is rejected) and the displayed body (at most 16 KiB of
   strict UTF-8, read from its stored length). It also snapshots the open/close counters.
   It then calls the original. It stages only afterwards, and only if no newer attempt,
   invalidation or generation change happened meanwhile. A nested open therefore always
   wins. No pointer is kept.
4. The menu sink may run off the game thread. It only bumps open/close counters.
5. On a later game-thread pump, the candidate is emitted once if all of these hold:
   exactly one BookMenu open since the snapshot and no close, the same epoch and
   generation, the menu `OnStack()` and `IsMenuDisplayEnabled()`, and the current static
   BOOK and body exactly equal to the candidate. The statics are read only while the menu
   is visibly on stack. Otherwise it is dropped. A 2 s timeout also drops it.

## Delivery (`src/client/readable_notes.hpp`)

- Negotiation: `note.read.v1` and `note.share.v1` are advertised only when the hook is
  installed and owned. They are used only after the init reply grants `readable_notes`. That
  grant needs flat, protocol v2, both note capabilities, `context.saved_anchor` and
  `context.loaded_plugins`. A server that does not grant it leaves notes off, and
  `/share-note` says it is unavailable.
- An admitted read is a copied value. The flat host keeps up to 4 reads until a session of
  the same runtime generation takes them. That covers BookMenu holding off session startup and
  the server still connecting. When full, the oldest read is dropped, so the latest one is never lost.
  Reads from another generation are discarded, never replayed. While the world is ready, capture
  runs every frame, also during the startup dwell; dwell and unready-world frames keep buffered reads.
- The session queues up to 4 reads and one share, in the order they happened. A read that does
  not fit still counts as the latest read, so a later share is refused and never falls back to an
  older note.
- Each item publishes its own fresh scene context first, then sends its event bound to that
  acknowledgement. The event is encoded once. The read's original body is sent and is never
  re-read from the game.
- Only the exact `note_read_accepted`/`player_private` or `note_share_accepted`/
  `shared_with_target` reply counts as a receipt. A receipt marks a save checkpoint as due.
- A transport failure or HTTP 408/409/429/5xx resends the same bytes, at most 3 attempts.
  Any other reply is final. Loading, a new game, the main menu or shutdown drops everything.

## `/share-note`

- The command is caught before narrator routing and ordinary chat text. It never becomes
  speech or history. Anything after it, including a new line, is refused, not read as a name.
- It shares only with the chat's retained listener. Admission fixes that exact actor, its base
  form and plugin, and the confirmed mode, which must be Standard, Whisper or Close.
- It is refused while the private narrator is active, while a mode change is pending, or when
  the chat's saved mode differs from the confirmed one.
- The share is fixed to the latest read before the command. It is sent after every earlier read
  has a final reply and before any later read. So a note opened afterwards stays private unless
  the player shares again. If that latest read was not accepted, the player is told to read it again.
- Before it is first sent, the fresh scene must still contain the exact retained listener and
  pass the same selection checks as the chat target, including the 4096-unit limit. There is no
  nearest-actor fallback. A resend after an uncertain failure keeps the same bytes and goes ahead
  of later reads. The player then sees "it may still have been delivered".
- Server visibility of private bodies is owned by the server. The client always sends a fresh
  context before each read and share, and never assumes the server already sees a body.

## Limits

- Opens the gate rejects are not reported: bodies over 16 KiB, invalid UTF-8, mod or
  indirect callers, and opens while the menu is already up. In those cases `/share-note`
  shares the server's latest recorded read, which can be an older note.
- If a session fails to connect, the runtime generation changes and buffered reads are dropped.
- After a hard halt the replacement session keeps the generation but may bind an older saved
  anchor, because the halt can suppress the saved ACK covering a read. So a retiring session that
  saw or inherited any read, acknowledged or not, makes its replacement refuse `/share-note`
  until a new read is acknowledged. This repeats across further halts. The player must reopen a
  note even when the last read was persisted. A session that never saw a read leaves nothing,
  and a new generation clears it. No body is replayed across saves.
- Everyone is not merged and its branch is held. The share check reads the authoritative
  `current_controls()` selection and refuses any audience other than Normal; Shout, the narrator
  and a pending mode change are refused too. Combined integration with Everyone is untested.

## VR

No capture. The VR lane does not compile this adapter and advertises nothing.

Evidence is static disassembly, off-game unit and fake-server tests, protocol fixtures, a
loopback HTTP run of the compiled client core, and x64 compiles of both lanes. No gameplay
matrix has run. It is untested whether pages render
correctly, overlays and other BookMenu or caller hooks coexist, or scripted and "can't take"
notes reach these two callers. VR support is not inferred from any of this.
