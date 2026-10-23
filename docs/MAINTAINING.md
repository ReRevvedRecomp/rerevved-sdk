# Maintaining the ReRevved fork

The fork is maintained for ReRevved. Read [Contributing](../README.md#contributing)
for branch and review rules, and [Development](development.md) for build and
validation commands. `upstream/main` is the upstream comparison reference.

## Accepting upstream changes

1. Fetch `upstream` and identify the exact accepted upstream release commit.
2. Create a disposable integration branch from that commit.
3. Reconstruct the maintained delta as small, coherent landmark commits. Do not
   merge `upstream/main` or the previous maintained branch into the candidate.
4. Review the linear graph, complete diff, and every file deletion.
5. Build, test, and install the candidate SDK on Windows.
6. Validate consuming projects and update their exact commit and version pins
   only after the candidate is accepted.

Moving the published `main` reference requires explicit owner approval after its
consumers and collaborators have been accounted for. Do not place project
commits on `upstream/main`.

## Artifacts and releases

The `SDK artifacts` workflow accepts a full 40-character commit SHA and builds
that exact commit for `win-amd64`, `mac-amd64`, `mac-arm64`, `linux-amd64`, and
`linux-arm64`. The Windows job runs the full test suite; Unix jobs run the
`^mod_plugin\.` selection with PowerPC tests disabled. Each workflow artifact is
named `rexglue-sdk-<platform>` and contains a
`rexglue-sdk-<version>-<platform>.zip` archive.

The retained release and nightly workflows publish only in
`rexglue/rexglue-sdk`; this fork uses the artifact workflow above. The fork does
not publish from upstream-style `v*` tags. Creating a project tag or release
requires explicit owner approval. Consumer locks identify the exact accepted SDK
commit and installed version.


## ReXApp lifecycle and overlays

See [ReXApp lifecycle and
overlays](runtime-integration.md#rexapp-lifecycle-and-overlays).

## Overlay settings, skin and FPS counter

See [Overlay settings, skin and FPS
counter](runtime-integration.md#overlay-settings-skin-and-fps-counter).

## Host session data and notifications

See [Host session data and
notifications](runtime-integration.md#host-session-data-and-notifications).

## Detached video delegate

See [Detached video delegate](runtime-integration.md#detached-video-delegate).

## Physical write notices

See [Physical write notices](runtime-integration.md#physical-write-notices).

## Lua package runtime

See [Lua package runtime](runtime-integration.md#lua-package-runtime).

## Frame-input diagnostics

See [Frame-input diagnostics](frame-input.md).

## Guest output frame timing

See [Guest output frame
timing](runtime-integration.md#guest-output-frame-timing).
