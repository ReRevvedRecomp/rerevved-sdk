# Maintaining the ReRevved fork

`main` is the maintained project branch and the repository's default branch.
`upstream-main` is a fast-forward-only mirror of `upstream/main`.

## Accepting upstream changes

1. Fetch `upstream` and identify the exact accepted upstream release commit.
2. Create a disposable integration branch from that commit.
3. Reconstruct the maintained delta as small, coherent landmark commits.
   Do not merge `upstream-main` or the previous maintained branch into the
   candidate.
4. Review the linear graph, complete diff, and every file deletion.
5. Build, test, and install the candidate SDK on Windows.
6. Validate consuming projects and update their exact commit and version pins
   only after the candidate is accepted.

Moving the published `main` reference requires explicit owner approval after
its consumers and collaborators have been accounted for. Do not place project
commits on `upstream-main`.

## Artifacts and releases

The `SDK artifacts` workflow accepts a full 40-character commit SHA and builds
that exact commit for `win-amd64`, `mac-amd64`, `mac-arm64`, `linux-amd64`, and
`linux-arm64`. The Windows build runs the test suite before uploading an SDK
archive. Artifact names identify the SDK version and platform deterministically.

The fork does not publish from upstream-style `v*` tags. If project prereleases
are enabled after the artifact workflow has been validated, their tags use the
`rerevved-sdk-v*` namespace. Creating a tag or release requires explicit owner
approval, and the release commit and version must match the installed SDK pinned
by consumers.
