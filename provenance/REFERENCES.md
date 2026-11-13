# Native reference provenance and clean-room boundary

## Dialectic

- Repository: `https://github.com/Dwemer-Dynamics/Dialectic.git`
- Authoritative implementation baseline: `de9c172e3b12743d20c82527f0fc7810e691798a` (`unstable` at implementation start)
- License inspected: MIT, retained at `LICENSES/DIALECTIC-MIT.txt`
- Exact history attachment merge: `67ef30d`

The exact Dialectic history is attached for reviewable provenance. Fallout: New Vegas/xNVSE ABI declarations, offsets, vendor SDK, scripts, package assets, voice transcript data, and compiled ESP are not built or copied into the SYNTH release tree. The engine-independent core and separate Fallout 4/F4SE and Fallout 4 VR/F4SEVR adapters use their independently pinned target toolchains.

## CHIM

- Repository: `https://github.com/Dwemer-Dynamics/CHIM.git`
- Pinned commit: `77c73ffb6bb32c226340bbda93b3aac5a7ad49f8`
- Local Git reference: `refs/claudex/reference/chim`

CHIM is design-reference-only. No CHIM source or binary/game assets are imported because its aggregate license scope and tracked DLL, ESP, PEX, SWF, and media assets are not suitable for the clean native client boundary.

## Import policy

Any future source reuse requires a separate exact-blob import commit, retained copyright/license notice, an explicit path ledger, a mechanical-change commit, and only then semantic changes. FNV/Skyrim ABI types, runtime offsets/layouts, form IDs, scripts, game assets, compiled plugins, and generated/user data remain forbidden.

## Engineering guidance references

Checked on 2026-11-13 for the code, comment and documentation conventions in `CONTRIBUTING.md`
and the index in `docs/README.md`. No text or source was copied.

| Repository | Ref | Commit | Files read |
| --- | --- | --- | --- |
| `Dwemer-Dynamics/CHIM_4` | `unstable` | `3098fb103dbe2cbbb06f2a5753f4c69c06e5b6d4` | `AGENTS.md`, `coding_standards.txt`, `README.md`, `docs/build-and-deploy.md`, `docs/design/speech.md`, `docs/design/world-observations.md` |
| `abeiro/chevo` | `unstable` | `fd04bcb08f6aac31dc1ca9438469b37342f26710` | `AGENT.md`, `.github/copilot-instructions.md`, `managers/README.md` and the ContextManager, MemoryManager, SaveState and PromptManager READMEs, `drivers/llm/PLAN.md` |

- **Reused:** a file-purpose comment where the name does not explain the file; intent over
  restating code; descriptive names, readable flow and one owner per responsibility; no
  gratuitous short helpers; follow local conventions; explicit failure handling; thread and
  game-thread safety; a docs index that maps components to notes; feature notes that state
  status and evidence limits.
- **Adapted:** the per-method comment quota became "comment where the role or intent is not
  obvious"; component index rows point to existing notes and source rather than one README per
  folder.
- **Excluded:** Skyrim, SKSE and engine library/UI/speech integration specifics; the singleton
  preference; camelCase method naming (SYNTH uses `snake_case` functions); the
  CHIM 4 folder layout; the reference ORM/autoloading architecture and chevo manager rules
  (server-side and not used here); any architecture migration.
