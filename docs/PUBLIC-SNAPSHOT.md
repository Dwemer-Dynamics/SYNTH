# Public source snapshot

The maintainer chose a clean public snapshot on 2026-10-06. Original development history and
private PRs remain in the private RANGROO/SYNTH repository. The public source starts from the
reviewed client cleanup at 618e9240001e3595c2c91f141c92c53b68f34883, with public handoff and
alpha-first contribution instructions added. Historical commit references in feature docs may
refer to the private archive; they are evidence receipts, not public checkout requirements.

Keep LICENSE, LICENSES/DIALECTIC-MIT.txt and all third-party notices. A clean Git history does
not change authorship or third-party terms. Corresponding build source and dependency pins are
in this repository; no game imports or generated binaries are supplied.

Branches: synth is the default/main lane; every development branch and PR starts from/targets
alpha. dev and unstable are reserved lanes. All four initially contain the same alpha source;
none is a tested stable release. See ALPHA-ACCEPTANCE.md for open gameplay checks.

Publication scan review: five generic-key detections are cache-key fixtures, not credentials.
The scan is evidence for this snapshot, not a guarantee against every possible secret.
