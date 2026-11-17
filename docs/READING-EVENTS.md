# Player reading events (flat candidate)

Flat-only, negotiated `context.reading_events`: when the player reads a book, note, magazine or a
terminal entry, or a terminal launches a holotape, the client copies its title and text (bounded)
and publishes it as one `payload.reading_event` on its own context, so NPCs near the player can
refer to what the player read. It is copied game text and the fact that the player opened it. It never
claims that anyone else read, heard or understood it. VR does not offer the capability.

Source: `src/adapters/flat_reading_events.hpp`, `src/core/reading_event.hpp`,
`src/client/reading_event_delivery.hpp`; schema `protocol/v2/schemas/reading-event.schema.json`.

## Contract

- The capability is advertised by the flat client and used only after the server's init
  acknowledgement carries `status.reading_events: true`. Older servers never acknowledge it, so no
  `reading_event` key ever reaches their closed context schema.
- One reading per context: `event_id` `reading:<n>`, `kind` (`book`, `note`, `magazine`, `holotape`,
  `terminal`), `title` (at most 128 code points), `source` (form ID and origin plugin of the book,
  terminal or holotape), `text` (optional, at most 4000 code points), `text_truncated`, `age_ms`.
  Audio holotapes and anything whose text could not be read carry the title only.
- The client strips Scaleform markup and alias placeholders, turns `[pagebreak]` into a blank line,
  drops control characters and malformed UTF-8, and cuts at a code-point boundary. The server
  re-checks every bound. Text beyond 4000 code points is cut and flagged, never summarized.
- The same item (kind, form, title) read again within 10 minutes is not a new reading. Four captures
  queue per session; overflow drops and is logged as a loss count, never waits on the game thread.
  A retry reuses the original capture, `event_id` and scene, so a lost response never duplicates a row.
- The server stores one `reading_event` row in the existing observation timeline (no table of its
  own), tagged to the player, and quotes the text as game content in the prompt history.
  See the server's `docs/READING-EVENTS.md`.

## Capture limits (not checked in game)

- Books, notes and magazines are found by pairing the player's activation of a book reference with the
  book menu opening within ten seconds. Books, magazines and holotapes opened from the Pip-Boy
  inventory expose no item identity to this adapter and are not captured. This is the largest gap.
- A terminal entry is reported only when the terminal menu's own last-selection record names a
  text or holotape entry of the current list. Anything else reports nothing rather than guessing. That
  record's meaning is read from the CommonLibF4 layout and has not been confirmed on a running game.
- Magazine and note kinds rely on the vanilla `ObjectTypeMagazine` / `ObjectTypeNote` keywords when
  present; otherwise a record is a `book`.
- The adapter registers an ordinary activation event sink, not an address-audited hook. It fails
  closed (no registration, no capability) when the event source is unavailable.

No gameplay evidence exists. Everything above the adapter is covered by native unit tests and
the golden fixtures; the adapter compiles in the flat lane only.
