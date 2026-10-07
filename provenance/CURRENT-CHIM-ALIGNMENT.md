# Current CHIM and LORKHAN alignment baseline

Baseline capture date: 2026-08-30.

| Repository | Ref | Commit | Use |
| --- | --- | --- | --- |
| `Dwemer-Dynamics/CHIM` | `unstable` | `165b21c11f5005ca270f4711bc1c3b8770571902` | Authoritative client-visible dialogue and lifecycle behavior reference. |
| `Dwemer-Dynamics/HerikaServer` | `unstable` | `3a5b79e262c2a9256fa8af1c67fa0205dd23e7d9` | Authoritative server contract and feature reference. |
| `RANGROO/LORKHAN` | `main` | `e45f621218a4249999913d8dd8cef116e0bc9b96` | Shared-core, protocol and evidence-workflow reference. |
| `RANGROO/LORKHANserver` | `main` | `bf7b946a2be7dca2a268f9ea680b0d74f8ef9d0f` | Strict contract and management-validation reference. |

The implementation continues from SYNTH commit
`a9e263f62cb216bde4901ea0763e347dc86e9aa8`. CHIM is a behavior reference: its Skyrim ABI,
offsets, Papyrus, PEX, ESP, BA2, SWF, media and game assets are not imported. Fallout 4 and
Fallout 4 VR retain separate pinned runtime adapters and build artifacts.

Open draft pull requests are not part of the authoritative baseline. Every adapted behavior must
remain in the engine-independent core where possible and must have a flat or VR capability gate
where the runtime implementation differs.
