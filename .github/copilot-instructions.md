# SYNTH repository instructions

- Start every feature branch from origin/unstable. Open every topic PR as a draft targeting
  unstable. Promotions are only same-repository unstable -> dev and dev -> synth; never open a
  PR from alpha or a reverse/skipped promotion. See BRANCHES.md, CONTRIBUTING.md and AGENTS.md.
- Preserve runtime adapters, game-thread ownership, save compatibility and protocol mirroring.
- Do not commit credentials, game files, generated binaries, logs or local configuration.
- Hosted CI builds and tests source; release packaging stays local.
- Report actual checks and distinguish builds from flat and VR gameplay evidence.
