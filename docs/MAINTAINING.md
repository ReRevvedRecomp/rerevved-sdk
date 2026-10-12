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

An observer may opt in to `OnDrawPrepared` through `WantsPreparedDraws` before
registration. D3D12 delivers that view after CPU shader analysis and before
original submission setup. It retains `kUnfinished` and
`original_host_issued == false`. Analyzed ordinary nonstacked 2D samples can be
complete using immutable raw shader metadata and the current CPU fetch state.
Unsigned and signed companion bindings retain DXBC descriptor order; each
stage/fetch must have one effective sampler interpretation. Requiredness for
an unused signed companion is classified from validated shader analysis and
fetch swizzled signs before payload preparation. Its payload may be unavailable
with its actual reason without making the draw incomplete; validation, sampler
agreement and all required CPU-read, dirty and GPU-write guards still apply.
An optional companion failure reads no payload bytes and creates no memory
version, resource, native owner or completion state. Unsupported query or
dimension demand, inaccessible or GPU-owned required textures, memexport, and
invalid geometry remain incomplete. The legacy texture ranges must cover the
canonical cache layout, except for the selected unsigned linear BC1 and BC2
forms. The exact packed tuples below are unsigned, linear, nonstacked 2D,
with `k8in16`, raw swizzle `0x688`, and minimum LOD 0. Pitch is in
32-texel fetch units; each row permits only its listed maximum LOD.

| Format | Width x height | Pitch | Maximum LOD |
|---|---|---|---|
| BC1 (`k_DXT1`) | 64x64 | 4 | 6 |
| BC1 (`k_DXT1`) | 128x128 | 4 | 7 |
| BC1 (`k_DXT1`) | 256x256 | 8 | 8 |
| BC1 (`k_DXT1`) | 256x128 | 8 | 8 |
| BC1 (`k_DXT1`) | 512x512 | 16 | 9 |
| BC1 (`k_DXT1`) | 32x32 | 4 | 5 |
| BC1 (`k_DXT1`) | 512x256 | 16 | 9 |
| BC1 (`k_DXT1`) | 1024x1024 | 32 | 10 |
| BC2 (`k_DXT2_3`) | 64x64 | 4 | 6 |
| BC2 (`k_DXT2_3`) | 128x256 | 4 | 8 |
| BC2 (`k_DXT2_3`) | 128x128 | 4 | 7 |
| BC2 (`k_DXT2_3`) | 128x64 | 4 | 7 |
| BC2 (`k_DXT2_3`) | 256x128 | 8 | 8 |
| BC2 (`k_DXT2_3`) | 256x256 | 8 | 8 |
| BC2 (`k_DXT2_3`) | 512x256 | 16 | 9 |
| BC2 (`k_DXT2_3`) | 512x512 | 16 | 9 |

Prepared extends only these packed mip ranges to
`texture_util::GetGuestTextureLayout`'s canonical extent. Single-level
BC1 8x8/pitch4, 512x512/pitch16, 1024x1024/pitch32,
2048x2048/pitch64, 4096x4096/pitch128 and BC2 256x256/pitch8
already pass the ordinary range-coverage guard with LOD 0 and no packed mips;
they receive no tuple exception. This metadata admission is not a format-wide
pixel-conformance claim. The consumer owns the guarded copy and the ordinary
cache metadata remains unchanged. Other
uncovered packed-mip and mip-only layouts remain unavailable. Preparation
creates no host texture, upload, descriptor or texture watch. Missing texture
versions use the guarded reader. A subsequent `OnDraw` keeps the event ID and
actual original result. Both views are synchronous.

A required native-source texture is reported as `kTextureUnresolved`, with
no CPU payload or memory version. The retained unsigned, single-level Rgba8
form and one signed tiled RG16 float form have exact range owners. The float
form is nonstacked 2D, 1280x720, pitch 40 fetch units, raw signs `0x55`,
swizzle `0xB48`, and one unpacked level. Its Prepared view uses the canonical
`0x398000` tiled extent, including padding and the final guard. Ordinary cache
metadata and full-range watches are unchanged. Its unused unsigned companion
retains descriptor order but creates no second required native dependency.
Unsigned or mixed-sign float demand remains required and unsupported.

Only that exact signed owner may bypass its resolve's dirty interval. Unknown
dirty state, CPU overwrites, partial aliases, failed originals and retired or
replaced owners remain rejected. A successful original resolve retains
prospective native-source ownership; it does not establish native completion
or completed guest CPU readback. Consumers must bind the range to their own
ordered producer and actual completion owner.

`WantsPreparedCopies` similarly opts in before registration to `OnCopyPrepared`
before original submission setup. A complete view requires a nonzero normal
resolve plan with guarded CPU geometry. The bounded color-0 Rgba8 main resolve
also admits the selected color/depth clear form: surface `0x0A010280`, depth
`0x530`, mode `6`, copy control `0x00100340`, destination pitch
`0x02D00500`, destination info `0x01000300`, HIZ clear `0xFFF`, depth clear
`0xFFFFFF00`, zero color-clear words, and the selected left resolve geometry.
That geometry is `[-0.5,-0.5,639.5,-0.5,639.5,719.5]` in guest float order.
The paired no-clear right resolve keeps the captured guest rectangle
`[639.5,-0.5,1279.5,-0.5,1279.5,719.5]` with window offset `0x7D80`, window
scissor `640..1280`, and vertex-window-offset mode enabled; `GetResolveInfo`
therefore derives the same source half for original and prepared planning.
Other clear words and forms, readback, array destinations, no-effect and
unsupported inputs remain incomplete. Planning and the prepared geometry reader
share one immutable CPU vertex snapshot.
`planned_address` and `planned_length` are prospective; they never mark guest
memory dirty, promote a version or establish native GPU completion. Prepared
copies retain `original_succeeded == false` and zero `written_*`. The later
`OnCopy` retains the ID and reports actual original results, including false
with a nonzero partial write. CPU preparation failure does not erase those
actual results.

Prepared callback results remain advisory. Rejection or an exception retires
the observer through the existing mailbox rules and does not suppress the
original command. Observers that do not opt in receive no prepared callback.

`WantsPreparedSwaps` freezes terminal opt-in at registration. The common CPU
PM4 decoder calls the default-noop `OnSwapDecoded` hook after consuming the
complete swap packet and setting its existing trace token, before `IssueSwap`.
D3D12 delivers `OnSwapPrepared` before original submission or swap texture
acquisition. Its first decoded boundary calls `OnFrameBegin` once with the same
view, then delivers the prepared terminal. Subsequent terminals keep that stream
open. Backend `frame` and `submission` are observations that may repeat after
original failure; the observer token and ordered event ID identify the boundary.
The later `OnSwap` uses that exact token, ID and backend observations. Early
original failure consumes the ID even when no later swap view exists. A detached
or replaced observer receives no old terminal's post callback or End.

Swap registers borrow the final live register file. Both gamma spans borrow
callback-local copies of the actual CPU gamma owner, containing 256 table words
and 384 PWL words. Retain needed state by copying it before returning. Fetch-0
format selects table or PWL gamma, and `use_fxaa`/`is_8bpc` describe requested CPU
policy; original allocation failure may change realized post-processing. The
decoded frontbuffer values do not prove a native producer or provide pixels,
host resources, device, fence or output lifetime. The consumer must prove a
complete supported graph and retain its existing native terminal owners.
If optional terminal setup fails, an open prepared stream may receive the same
prospective ID and decoded scalars with empty register and gamma spans for
failure attribution. That state is incomplete and must be rejected. The SDK
retires only the selected observer and closes its local stream and interval
metadata; original issue continues.

Prepared swap provenance stays `original_succeeded == false`. The later
`OnFrameEnd` reports the actual original result, including false before source
acquisition. For prepared terminal observers, End(false) is an observation and
does not close accepted successor CPU preparation. Rejection, detachment and
native consumer failure retain their own stop rules. Legacy observers retain
failure-close behavior. Frame-local buffer metadata and the CPU dirty-interval
guard transition once at the decoded prepared terminal; the later original
callback does not transition them again. Shared-memory GPU ownership and CPU
overwrite precedence remain intact.

Prepared admission does not prove native recording, successful fence signaling,
completion, version promotion, guest writes or presentation. Actual native
completion and consumer-copy retirement remain the consumer's responsibility.
Original GPU work, ring progress, trace lifetime and original fences continue.
This terminal adapter covers D3D12; Vulkan has the common hook but no equivalent
frame-input terminal adapter. The bounded clear admission is internal planner
behavior and does not add public API or ABI surface. Rebuild the SDK and
consumers for API floor 0.25.0.

The common CPU command processor also exposes an optional packet-admission
callback for consumers that must bound a command subset before decoding. The
callback sees the packet header while the ring read index is still at the
packet start; returning false leaves that index unchanged, stops the worker,
and causes the next writeback to report the rejected start. Consumers publish
write pointers through `PublishWritePointer`, which rejects out-of-range
values and never restarts a failed worker; an in-flight sampled range completes
its effects and writeback before that rejection becomes terminal. `read_ptr_index()`,
`worker_running()`, `worker_failed()`, and `progress_sequence()` are
synchronized observations; progress is the final sequence point after an
endian-correct guest writeback and terminal status publication. Omitting packet
admission preserves the legacy decoder behavior. `Initialize()` reports worker
construction or creation failure, and `Shutdown()` tolerates partial startup.
The common CPU command processor, register file and register definitions are
runtime-owned and accept borrowed CPU owners plus an explicit legacy interrupt
bridge; GPU plugin providers, MMIO, vblank and exports remain plugin-owned. GPU
plugin ABI 4 is unchanged: `GpuCreateInfo`, `IGraphicsSystem` and its callback
view layouts retain their documented structures.

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
