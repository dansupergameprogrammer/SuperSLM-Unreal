# Branch protection: required configuration

GitHub does not let a workflow configure branch protection on itself, so the setting is
recorded here, and a repository admin applies it under **Settings > Branches > Branch
protection rules** for `main`.

## Rule: `main`

- **Require a pull request before merging**: enabled.
- **Require conversation resolution before merging**: enabled.
- **Do not allow bypassing the above settings**: enabled (includes administrators).
- **Require status checks to pass before merging**: **not enabled**, deliberately. See below.

## Why no status check is required

Every workflow in this repository is manual (`workflow_dispatch` only). The maintainer runs CI
when he chooses to, not on every push or pull request. A required status check that only runs
when someone dispatches it would block every merge that nobody dispatched it for, so requiring
one would either stall the branch or train everyone to bypass it.

The checks are therefore a **release checklist, not a merge gate**. Before a release pull request
into `main` is merged, the maintainer dispatches them on the release branch and records the
result in the pull request:

| Workflow | Job | What it proves |
|---|---|---|
| `coherence-checks` | `vendored-coherence` | The vendored SuperSLM source is byte-for-byte the pinned upstream tag and matches its manifest. |
| `coherence-checks` | `vendored-wrappers` | Every upstream source the build needs is compiled into the plugin. |
| `coherence-checks` | `version-identity` | The `.uplugin` version, the CHANGELOG, and the SuperSLM pin agree. |
| `plugin-build` | `editor-build`, `automation-tests`, `shipping-build`, `package-plugin` | The plugin builds and its automation suite passes on the self-hosted UE 5.8 Windows runner. |

`release.yml` re-runs the coherence checks itself before it cuts a tag, and refuses to tag if they
fail, so a release cannot be cut on a tree that fails them even if the checklist was skipped.

If the maintainer later makes a check automatic (a `push` or `pull_request` trigger), add it to
the required list here and in the branch rule in the same change.

## Tags: no ruleset, deliberately

No tag protection rule is configured. On a user-owned repository, a ruleset's bypass list offers
only roles that all resolve to the sole maintainer, plus deploy keys; the Actions app that cuts
release tags cannot be granted bypass. Any such rule would be advisory against the only person it
applies to. Release tags are cut by `release.yml`, after its own coherence gate.
