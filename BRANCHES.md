# Branch policy

All development starts from `alpha`. Every pull request targets `alpha` and opens as a draft.
SYNTH does not use the other Dwemer mods' unstable-first workflow.

| Branch | Purpose |
|---|---|
| alpha | Current development and default PR target |
| unstable | Reserved integration lane |
| dev | Reserved tested-development lane |
| synth | Main/release lane |

Maintainers explicitly control promotions. No branch name implies gameplay acceptance.

```powershell
git clone --branch alpha https://github.com/Dwemer-Dynamics/SYNTH.git
cd SYNTH
git switch -c feature/my-change
```

Read CONTRIBUTING.md, AGENTS.md and docs/HANDOFF.md before changing code.
