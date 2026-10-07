# SYNTH repository instructions

- Start every feature branch from origin/alpha. Open every PR as a draft targeting alpha.
- Never use the other Dwemer repos' unstable-first flow. See CONTRIBUTING.md and AGENTS.md.
- Preserve runtime adapters, game-thread ownership, save compatibility and protocol mirroring.
- Do not commit credentials, game files, generated binaries, logs or local configuration.
- Hosted CI builds and tests source; release packaging stays local.
- Report actual checks and distinguish builds from flat and VR gameplay evidence.
