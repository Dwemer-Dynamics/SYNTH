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
