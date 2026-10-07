# Publication checklist

The repository is **not** ready for public release. The human already authorized mechanical
content hygiene (workstation paths, scratch locations and private backup destinations in tracked
docs), and that pass is done. The project license is chosen. The remaining history and
publication decisions below need human direction. The Synthserver repository keeps its own
checklist.

## License

- [x] The human chose the GNU GPL v3.0 for the project (2026-10-06), as for the other mods.
- [x] Root `LICENSE` is the unmodified GPLv3 text (SHA-256
      `3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986`, identical to the
      pinned CommonLibF4 GPL notice). The README has a License section.
      `scripts/package_release.py` ships it at the root of both packages, and
      `tools/audit_release_tree.py` rejects a package where it is missing or altered.
- [x] `LICENSES/DIALECTIC-MIT.txt` and every notice in `packaging/licenses/` are kept and
      still shipped per lane.
- [ ] The pinned libxse/CommonLibF4 (`6266ecc`) is GPL-3.0-or-later with the Modding and
      Linking Exceptions (upstream relicense `0d96d54`). The flat `SYNTH.dll` compiles it in.
      Distributing that DLL requires offering the corresponding source under GPL-3.0-or-later
      terms. Confirm the corresponding-source offer at publication. CommonLibF4VR (VR) is
      still MIT.

## Third-party terms

- [ ] F4SE Menu Framework publishes no license. SYNTH fetches its consumer header at build
      time and does not redistribute it. Get clarification before shipping any of its files.
- [x] Linked dependency notices verified against the build inputs and shipped per lane
      (`THIRD_PARTY_NOTICES.md`). `rapidcsv` is used: CommonLibF4VR `REL/IDDB.cpp` includes it.
- [x] spdlog notices per version: the official v1.16.0 `LICENSE` differs from the vcpkg 1.17.0
      port text (copyright line and trailing blank line), so the flat package ships
      `spdlog-1.16.0-MIT.txt` and the VR package keeps `spdlog-MIT.txt`.
- [x] VR statically links spdlog and fmt (`x64-windows-static-md`); a rebuilt `SYNTHVR.dll`
      imports neither DLL, and its package passed the release audit (see `HANDOFF.md`).
- [ ] `SYNTH.esp` and the Papyrus scripts are original. The Papyrus build uses the user's own
      game script imports, which must never be committed or shipped.

## Repository history

- [ ] History contains the attached Dialectic history (merge `67ef30d`), which includes
      `Dialectic.esp`, an `xnvse-sdk` vendor tree and reverted `upstream/**` imports.
      History also exposes author email addresses.
- [ ] Choose between publishing the reviewed history and a fresh public snapshot. That
      choice needs human direction. History rewriting and public repository
      creation are out of scope for this cleanup.

## Content review

- [x] Workstation paths, scratch probe paths and private backup destinations in the feature
      docs are replaced with placeholders. Dated deployment receipts are kept and marked
      historical (see [HANDOFF.md](HANDOFF.md#historical-receipts-and-placeholders)). Git
      history still contains the original paths (see Repository history).
- [ ] Secret scanning: a baseline Gitleaks 8.30.1 all-ref scan reported only reviewed
      non-credential matches (cache/idempotency fixture strings and voice-ID validation
      comparisons). A scanner only covers its own rules. Re-run on the final head and
      history.

## Release readiness

- [ ] Hosted source CI passes both DLL builds and the portable contracts job. Hosted CI does
      not package (policy chosen 2026-10-06).
- [ ] Both release packages are built and audited locally from the exact release DLLs and
      the compiled original Papyrus scripts.
- [ ] [Alpha acceptance](ALPHA-ACCEPTANCE.md) passes independently for flat and VR.
- [ ] Open PRs #2–#7 are reviewed, and the promotion target is decided.
