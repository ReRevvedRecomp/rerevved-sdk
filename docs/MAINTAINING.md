# Maintaining the ReRevved fork

`main` is the maintained project branch and the repository's default branch.
`upstream/main` is the upstream comparison reference.

## Accepting upstream changes

1. Fetch `upstream` and identify the exact accepted upstream release commit.
2. Create a disposable integration branch from that commit.
3. Reconstruct the maintained delta as small, coherent landmark commits.
   Do not merge `upstream/main` or the previous maintained branch into the candidate.
4. Review the linear graph, complete diff, and every file deletion.
5. Build, test, and install the candidate SDK on Windows.
6. Validate consuming projects and update their exact commit and version pins
   only after the candidate is accepted.

Moving the published `main` reference requires explicit owner approval after
its consumers and collaborators have been accounted for.
Do not place project commits on `upstream/main`.

## Artifacts and releases

The `SDK artifacts` workflow accepts a full 40-character commit SHA and builds
that exact commit for `win-amd64`, `mac-amd64`, `mac-arm64`, `linux-amd64`, and
`linux-arm64`. The Windows build runs the test suite before uploading an SDK
archive. Artifact names identify the SDK version and platform deterministically.

The retained release and nightly workflows publish only in
`rexglue/rexglue-sdk`; this fork uses the artifact workflow above.
The fork does not publish from upstream-style `v*` tags. If project prereleases
are enabled after the artifact workflow has been validated, their tags use the
`rerevved-sdk-v*` namespace. Creating a tag or release requires explicit owner
approval, and the release commit and version must match the installed SDK pinned
by consumers.

## Frame-input diagnostics

The D3D12 runtime accepts `--d3d12_frame_input_costs` at startup. This optional
session-only flag defaults to false and is never saved to configuration. It logs
scalar frame-input costs to the runtime log while an input observer is attached;
it does not record guest input bytes. The first completed frame is reported
alone, followed by windows of 120 completed frames and partial windows at
failure, observer retirement/replacement or orderly context shutdown. Output is
capped at 256 records per command-processor thread, including replacements.

Each record names its observer, stream, frame/submission range and completed
frame count. Draw/copy preparation is timed after observer admission and
excludes timed mailbox/observer dispatch. Callback time includes that dispatch.
Shader and texture-view preparation are subsets of SDK draw time; guest-memory
read time is nested in callback time, including reads during swap processing.
Texture counts sample SDK availability before dispatch. A current guest-memory
token does not establish collector reuse, exact consumer ranges or GPU
completion. Timings include enabled instrumentation overhead and exclude the
original Xenos draw path and presentation outside the observer. They do not
measure complete frame time or FPS. The disabled path reads no clocks or extra
texture tokens.

The D3D12 input observer also supplies optional CPU buffer watch versions for
exact vertex fetches, copy geometry and submitted index prefixes. These tokens
are borrowed for the synchronous callback and identify guest bytes, not GPU
resources or completion. Consumers must match the complete range and check
validity before and after copying and on reuse. Missing, stale, inaccessible or
GPU-owned versions require the ordinary guarded reader. Writes, releases and
physical recommits invalidate the watches. Buffer metadata is limited to 4,096
ranges per frame; saturation omits tokens without evicting borrowed contexts.
The metadata is retired at swap, observer replacement/detachment and shutdown.

## Guest output frame timing

The D3D12 presenter exposes `GetGuestOutputFrameStats()` for CPU or GPU output
overrides. It counts distinct output tokens whose swapchain `Present` returns
`S_OK`. Repeated presentation, older mailbox entries, failed and occluded calls,
and producer or consumer-copy completion do not add frames. Successful original
output resets the measured interval.

FPS and mean frame time update over completed windows of at least one second.
After one second without a new presented token the rate becomes zero. Startup
has no timing sample until the first complete window. The counter uses bounded
scalar state and a monotonic clock, and its snapshots are thread-safe. It does
not establish physical scanout or GPU retirement. The existing F3 overlay can
display it through `SetGuestFrameStats`.
