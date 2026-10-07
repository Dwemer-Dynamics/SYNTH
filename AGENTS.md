# SYNTH engineering instructions

## Scope

This repository owns the Fallout 4 client: the F4SE plugin, client configuration, optional
interface assets, packaging scripts, unit tests, and client-side protocol fixtures. Synthserver
owns PHP, Apache, PostgreSQL, prompts, connectors, memory, TTS/STT services, and the management UI.

## Hard boundaries

- Add original gameplay records, packages and scripts when needed for the requested feature.
  Keep their source/build reproducible, audit masters and record ownership, and preserve saves
  and unrelated mods. Gameplay records do not require a separate authorization phase.
- Do not copy runtime offsets from a different Fallout build or from flat Fallout into VR. Declare
  supported runtimes and fail closed on every other build.
- Keep all `RE::*`, `REL::*`, F4SE/F4SEVR, hook, address, HMD/controller/node, and game-object
  access behind the selected `IFalloutRuntime` adapter and execute it on the game thread. Workers
  receive immutable copied snapshots only.
- Every asynchronous request, response, speech item, and action carries a runtime generation.
  Loading a save, starting a new game, returning to the main menu, or shutting down invalidates
  stale work.
- Never block the game thread on HTTP, audio decoding, filesystem scans, or model work.
- The protocol is versioned JSON. Reject unknown schemas, oversized payloads, stale generations,
  unknown actions, malformed form IDs, and server responses for a different request.
- Secrets never enter this repository, logs, diagnostics, fixtures, or command-line arguments.
- Do not push, publish, create releases, deploy to a game install, or make external production
  calls unless the assignment explicitly authorizes it.

## Build direction

- Native targets: Windows x64 C++23 flat `SYNTH.dll` with pinned `libxse/CommonLibF4`, and VR
  `SYNTHVR.dll` with pinned `ArthurHub/CommonLibF4VR`, F4SEVR, and VR Address Library.
- Put protocol, configuration, tasks, queues, media, conversation, and typed action logic in an
  engine-free shared core. Flat and VR adapters must not leak conditional ABI code into that core.
- VR is first-class. Use HMD-effective position/view, semantic controller actions, handedness, and
  a VR-safe game-thread presentation path. Do not assume Windows virtual keys, flat camera/player
  transforms, flat UI hooks, or FRIK availability.
- FRIK, Fallout4 VR Tools, Buffout, Idle Hands, OpenComposite, and Wabbajack stacks are optional
  compatibility surfaces, not source to copy or hard dependencies. Review licenses before copying
  API headers or linking a framework.
- Keep dependency pins reproducible. Do not track generated Visual Studio or build output.
- Native unit and protocol tests must run without Fallout. Windows CI must compile both DLLs
  separately; each lane is packaged and audited locally. Only the corresponding Windows flat/VR game can satisfy its in-game evidence.
- Preserve the Dialectic invariants: bounded workers, game-thread dispatcher, cancellation,
  generation invalidation, strict response envelopes, exact action ownership, and observable
  queue health.

## Workflow

- Start from `docs/HANDOFF.md`. It records the current baseline, runtimes, deferred work and the
  retired ledgers. Track open in-game items in `docs/ALPHA-ACCEPTANCE.md`.
- Start all work from `origin/alpha` and target every draft PR at `alpha`. Do not use
  the other Dwemer mods' unstable-first workflow. Follow `CONTRIBUTING.md`.
- Do not rebuild a chronological progress ledger. Update the relevant feature doc, the handoff
  and the acceptance checklist. Keep workstation paths, deployment receipts and private backup
  locations out of tracked files.
- The project license is GNU GPL v3.0 (root `LICENSE`). Preserve `LICENSES/DIALECTIC-MIT.txt`
  and every third-party notice. See `docs/PUBLICATION-CHECKLIST.md`.
- Hosted CI builds and tests source only. Release packaging and its audit are mandatory local
  steps; do not add hosted packaging that needs game script imports.

## Validation

For every change, run the narrowest relevant tests plus formatting/static checks. Before a
checkpoint, require the full native unit suite, protocol fixtures, release-tree audit, x64 build,
package manifest verification for both artifacts, and a diff/scope review. Report flat and VR
game-dependent requirements independently until each actual Windows matrix passes.
