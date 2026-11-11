# Branch policy

Start topic work from `unstable`. Every topic pull request, from this repository or a fork,
targets `unstable` and opens as a draft.

| Branch | Purpose |
|---|---|
| unstable | Integration lane and required topic PR target |
| dev | Tested-development lane; receives promotions from `unstable` only |
| synth | Main/release lane and GitHub default; receives promotions from `dev` only |
| alpha | Legacy development lane; no longer a PR target or promotion source |

Promotions are maintainer PRs from this repository only: `unstable` -> `dev` and `dev` ->
`synth`. The PR branch-policy check rejects reverse promotions, skipped promotions, fork
promotions and any PR from a maintained branch (including `alpha`) that is not one of those two.
The maintained-branch restriction applies only to heads in this repository; a fork PR is always
a topic PR and may target `unstable` whatever its head branch is named, but never `dev` or
`synth`. No branch name implies gameplay acceptance.

```powershell
git clone --branch unstable https://github.com/Dwemer-Dynamics/SYNTH.git
cd SYNTH
git switch -c feature/my-change
```

Read CONTRIBUTING.md, AGENTS.md and docs/HANDOFF.md before changing code.
