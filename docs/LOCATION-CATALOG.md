# Location catalog export (client)

**Status: protocol, codec, negotiation and the engine-free export logic are implemented and tested without the game. The
game reader, the trigger and the capability advertisement are not. Nothing here has run in either lane.**

The design, the Fallout 4 data mapping and the server storage are in the paired SynthServer change
(`docs/LOCATION-CATALOG.md` there). This note says what the client owns.

## What exists

- **Wire contract**: the `location_catalog` v2 event, `world.location_catalog` capability and `location_catalog` init flag, mirrored
  byte-for-byte in `protocol/v2` (`scripts/verify_protocol.py` knows the payload schema). An unbound event (`context_sequence` 0).
- **Model**: `src/core/location_catalog.hpp`, engine-free, with the schema's bounds (50 locations per batch, 32 markers, 64
  keywords, 255-byte names, finite coordinates within +/-10,000,000, plugin-local forms, interior markers without a worldspace and
  exterior ones with). `valid()` is checked before a record is sent, so one bad record never fails a batch at the server.
- **Codec**: `protocol_native::encode_location_catalog` (`src/protocol_native/v1_codec.hpp`). It refuses an unnegotiated session,
  a context binding, an over-large batch and an invalid record. `Status::location_catalog` carries the init acknowledgement.
- **Negotiation**: `SynthClient::location_catalog_ready()` is true only for a flat v2 session that listed the capability and
  whose server acknowledged it. `SynthClient::send_location_catalog()` is the one network call; it is never retried.
- **Export state**: `src/client/location_export.hpp` (`LocationExport`). It runs on the game thread, builds the next batch only
  after the previous answer (so the server paces the export), drops unreadable or out-of-bounds records into the refused count,
  marks the last batch `final`, stops after the batch on its way, and de-duplicates the first-entry upsert per session.
  `location_catalog_answer()` accepts only the exact three-line acknowledgement.
- **Tests**: `tests/protocol_native` (wire equals the shared fixture, bounds, batching, pacing, counts, stop, first entry) and
  `tests/fake_server` (negotiation by lane and capability, one real send and answer).

## What does not exist yet

1. **The game reader.** A `Reader` that, on the game thread, lists named `BGSLocation` records and builds a `CatalogLocation` from
   each: plugin-local identity through the loaded-plugin manifest (as `location_ancestry` does), the nearest named parent, keyword
   editor IDs, `IsCleared()`, map-marker discovered state, and the marker references with cell, worldspace, position and disabled
   flag. It belongs in the runtime adapter behind `IFalloutRuntime`. The marker sources (special refs, persistent refs, the
   workshop reference) and the keyword editor IDs are the **(confirm)** items of the server design; they need the pinned
   CommonLibF4 headers and a real export, and none was available here, so no engine code was written on a guess.
2. **The trigger.** A way to start the export (a Utilities-style control or a console/Papyrus entry) and the first-entry hook,
   which must send the upsert before the context that names the location. The export state already supports both.
3. **The advertisement.** `ContextPublication::capabilities(..., location_catalog_available)` defaults to false, so the
   capability is not offered and the wire behaves exactly as before. Pass true from the flat adapter only once a reader exists.
4. **VR.** The fields are lane-neutral, but the client never advertises the capability for VR.

## Checks that ran

`scripts/verify_protocol.py --peer` against the server tree, `tests/protocol_native` and `tests/fake_server` built and run with
the engine-free targets. No flat or VR game library was built, so the adapter is untouched; no in-game check was possible.

## Behavior notes

- **Not-stored answers.** The acknowledgement's `stored` flag is parsed. An accepted answer with `stored: false` counts nothing as
  accepted, ends the run with "Server not storing locations yet" (`Progress::server_not_storing`), and the export can be started
  again. The server also withholds the init flag until it stores, so a real session does not negotiate the capability before then.
- **Request lock.** `send_location_catalog` takes its own mutex, not the dialogue request mutex, so a 15 s export batch cannot stall
  dialogue.
