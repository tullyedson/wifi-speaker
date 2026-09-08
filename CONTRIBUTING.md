# Contributing

Build from the repository root using the [README](README.md). Changes should keep the hub usable with unrelated OpenAI-compatible endpoints and custom webhook apps. Deployment-specific URLs, credentials, device identities, model selections and routing conventions belong in local settings.

## Pull requests and owner approval

All changes to the hosted repository's `main` branch, including features, fixes, documentation and ownership/workflow files, require a pull request. `.github/CODEOWNERS` names the repository owner for every path, including itself.

The active branch rules require:

- At least one approving review and approval from the designated code owner.
- Fresh review after commits change an approved diff; stale approvals are dismissed.
- A successful `windows` check from GitHub Actions in the **Verify** workflow, tested with the latest base branch.
- Resolution of all review conversations before merging.
- No direct pushes, force pushes, branch deletion or automatic administrator bypass.

The ruleset is configured on GitHub. Copying CODEOWNERS to another repository does not enable these restrictions there; maintainers of forks manage their own rules.

GitHub [does not allow PR authors to approve their own PRs](https://docs.github.com/en/pull-requests/how-tos/review-pull-requests/approving-a-pull-request-with-required-reviews). Keep a contribution or automation account separate from the owner who will review. A PR opened under the owner's login cannot receive that owner's approval. Do not use the owner's credentials to approve contributions automatically or weaken the branch rules to get a PR merged.

## Contribution workflow

1. Fork the repository and create a focused branch from current `main`.
2. Implement the change and run the relevant checks described below.
3. Review the complete diff for private data and preserve dependency notices.
4. Open a pull request against `main`. Describe the concrete problem, resulting behavior and validation. Include limitations or hardware that still needs testing.
5. Address review comments, update from `main` if needed, and wait for the required checks and owner approval. New changes can require another review.

Draft PRs are useful while work is incomplete. Mark the PR ready for review when the change and evidence are ready to assess.

## Validation and private data

Run the relevant checks in [docs/verification.md](docs/verification.md). Rust audio and routing changes need behavioral coverage through the public protocol. UI changes should be exercised in the native Tauri app. Firmware changes require the correct board and physical audio verification before claiming hardware support. Documentation-only changes need accurate commands and working links.

Use fictional test names and generated tokens. Keep reports, recordings, screenshots, private settings, flash backups and build outputs out of commits. Run `python scripts/check-public.py` and inspect `git diff --cached` before committing. Never weaken authentication to make a test pass.

Preserve third-party license and attribution files. New board support should document its pins, codec, flash layout, provisioning method and tested limitations.
