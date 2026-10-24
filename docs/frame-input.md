# Frame-input diagnostics

This reference is for SDK and renderer integrators. Prepared observations do not
prove GPU completion, presentation, or pixel conformance. See [Runtime
integration](runtime-integration.md) for lifetime and memory contracts.

- [Cost logging](#cost-logging)
- [CPU buffer versions](#cpu-buffer-versions)
- [Prepared draws and CPU textures](#prepared-draws-and-cpu-textures)
- [Prepared CPU cube textures](#prepared-cpu-cube-textures)
- [Native-source textures](#native-source-textures)
- [Prepared copies](#prepared-copies)
- [Prepared swaps](#prepared-swaps)
- [CPU packet admission](#cpu-packet-admission)

## Cost logging

The D3D12 runtime accepts `--d3d12_frame_input_costs` at startup. This optional
session-only flag defaults to false and is never saved to configuration. It logs
scalar frame input costs to the runtime log while an input observer is attached;
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

## CPU buffer versions

The D3D12 input observer also supplies optional CPU buffer watch versions for
exact vertex fetches, copy geometry and submitted index prefixes. These tokens
are borrowed for the synchronous callback and identify guest bytes, not GPU
resources or completion. Consumers must match the complete range and check
validity before and after copying and on reuse. Missing, stale, inaccessible or
GPU-owned versions require the ordinary guarded reader. Writes, releases and
physical recommits invalidate the watches. Buffer metadata is limited to 4,096
ranges per frame; saturation omits tokens without evicting borrowed contexts.
The metadata is retired at swap, observer replacement/detachment and shutdown.

## Prepared draws and CPU textures

An observer may opt in to `OnDrawPrepared` through `WantsPreparedDraws` before
registration. D3D12 delivers that view after CPU shader analysis and before
original submission setup. It retains `kUnfinished` and `original_host_issued ==
false`. Analyzed ordinary nonstacked 2D samples and the exact CPU cube tuples
below can be complete using immutable raw shader metadata and the current CPU
fetch state. Unsigned and signed companion bindings retain DXBC descriptor
order; each stage/fetch must have one effective sampler interpretation. Before
preparing the payload, validated shader analysis and the fetch's swizzled signs
determine whether an unused signed companion is required. Its payload may be
unavailable with its actual reason without making the draw incomplete;
validation, sampler agreement and all required guards for CPU reads, dirty state
and GPU writes still apply. An optional companion failure reads no payload bytes
and creates no memory version, resource, native owner or completion state.
Unsupported query or dimension demand, inaccessible or GPU-owned required
textures, memexport, and invalid geometry remain incomplete. The legacy texture
ranges must cover the canonical cache layout, except for the selected unsigned
linear and tiled BC1, BC2, BC3, RGBA8, R8 and cube forms described below. The BC
packed tuples are unsigned, linear, nonstacked 2D, with `k8in16`, raw swizzle
`0x688`, and minimum LOD 0. Pitch is in 32-texel fetch units; each row permits
only its listed maximum LOD.

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
| BC3 (`k_DXT4_5`) | 64x64 | 4 | 6 |
| BC3 (`k_DXT4_5`) | 128x128 | 4 | 7 |
| BC3 (`k_DXT4_5`) | 256x128 | 8 | 8 |
| BC3 (`k_DXT4_5`) | 256x256 | 8 | 8 |
| BC3 (`k_DXT4_5`) | 512x256 | 16 | 9 |
| BC3 (`k_DXT4_5`) | 512x512 | 16 | 9 |

Prepared extends only these packed mip ranges to
`texture_util::GetGuestTextureLayout`'s canonical extent. Single-level BC1
8x8/pitch4, 512x512/pitch16, 1024x1024/pitch32, 2048x2048/pitch64,
4096x4096/pitch128, BC2 256x256/pitch8, and BC3 256x128/pitch8 and
512x512/pitch16 already pass the ordinary range-coverage guard with LOD 0 and no
packed mips; they receive no tuple exception. Accepting this metadata does not
prove pixel conformance for every texture using the format. The consumer owns
the guarded copy and the ordinary cache metadata remains unchanged. Other
uncovered packed-mip and mip-only layouts remain unavailable. Preparation
creates no host texture, upload, descriptor or texture watch. Missing texture
versions use the guarded reader. A subsequent `OnDraw` keeps the event ID and
actual original result. Both views are synchronous.

The BC3 packed correction also requires zero stack depth, numeric-format and
exponent-adjust fields, and an unset `force_bc_w_to_max`. It extends only the
mip range; the ordinary base coverage guard still applies. Consumer endpoint
proofs and device-dependent interpolation parity are separate pixel claims.

Five exact tiled, nonstacked 2D CPU-writer tuples extend the following legacy
ranges to the canonical tiled extent. All use `kNone`, swizzle `0x688`, unsigned
raw signs, minimum LOD 0, zero stack depth, numeric-format and exponent-adjust
fields, and an unset `force_bc_w_to_max`. Packed rows require a nonzero mip
page; the single-level BC2 row requires a zero mip address.

| Format | Width x height | Raw pitch | Maximum LOD | Packed | Base bytes, legacy -> Prepared | Mip bytes, legacy -> Prepared |
|---|---|---|---|---|---|---|
| BC1 | 128x64 | 4 | 7 | Yes | 4096 -> 8192 | 8192 -> 16384 |
| BC1 | 256x128 | 8 | 8 | Yes | 16384 | 16384 -> 24576 |
| BC2 | 128x64 | 4 | 7 | Yes | 8192 -> 16384 | 16384 -> 32768 |
| BC2 | 32x64 | 4 | 0 | No | 8192 -> 16384 | 0 |
| BC3 | 128x128 | 4 | 7 | Yes | 16384 | 32768 -> 49152 |

The 32x64 BC2 form retains its padded 128-texel pitch. Tiled BC3 128x128,
pitch4, single-level storage already passes ordinary admission with a 16384-byte
base range and no mips. It receives no correction tuple. Other undercovered
neighbors retain the ordinary rejection guard.

Prepared also extends the exact unsigned, linear, nonstacked 2D packed R8
(`k_8`) 512x512/pitch16 LOD0..9 form with endian `kNone`, swizzle `0x124`, and a
nonzero mip page. Its legacy mip extent is 122880 bytes; the canonical
126752-byte extent includes the 3872-byte tail at 122880. The RGBA8
(`k_8_8_8_8`) 256x4 and 256x1 packed forms use pitch8, `k8in32`, swizzle
`0x60A`, declared LOD0..8 and a zero mip page. Their Prepared base extents are
20480 and 17408 bytes, respectively, instead of the legacy 4096 and 1024 bytes.
LOD0 starts at byte offset 16384 in both base tails. These exceptions require
unsigned raw signs, normalized numbers, zero exponent adjustment, zero stack
depth and no forced BC alpha.

A zero mip page clamps the effective range to LOD0 through
[`GetSubresourcesFromFetchConstant`](../src/graphics/pipeline/texture/util.cpp)
(line 77); the cache key stores that normalized maximum in
[`BindingInfoFromFetchConstant`](../src/graphics/pipeline/texture/cache.cpp).
Prepared preserves this normalization. The RGBA8 8x8/pitch2 packed form with
declared LOD0..3 is already covered by its 2048-byte legacy base range; its
effective LOD0 starts at byte offset 64 and its canonical extent is 1888. The
unsigned RGBA8 tiled single levels 32x32/pitch1, 64x64/pitch2, 64x128/pitch2,
128x64/pitch4 and 128x128/pitch4 also pass the ordinary coverage guard. No
additional admission exception applies to these forms.

## Prepared CPU cube textures

Prepared CPU cube admission permits three linear, six-face packed forms with raw
stack depth 5, zero exponent adjustment and force-BC-W disabled. Base and mip
pages must be nonzero and distinct; their distance and ordering are
unrestricted. The 64x64 RGBA8 and RGBA16F forms use raw pitch 2 and LOD0..6.
RGBA8 uses k8in32, swizzle 0xA0A (B,G,R,constant one), num_format 0 and all
unsigned signs. RGBA16F uses k8in16, swizzle 0x688, num_format 1 and all signed
signs. The 128x128 BC3 form uses raw pitch 4, LOD0..7, k8in16, swizzle 0x688,
num_format 0 and all unsigned signs. Only the matching companion is required.
Extracting bindings from cube fetches preserves sampler overrides and slot
validation; the cache still decides whether to accept descriptors.

These Prepared views extend the legacy 49152-byte mip range to the canonical
94080-byte RGBA8 or 94208-byte RGBA16F range, including the final face's tail.
Their base ranges remain 98304 and 196608 bytes respectively. BC3 extends its
legacy 196608-byte mip range to 280192 bytes, with a 98304-byte base range. The
BC3 packed tail begins at 196608 bytes and spans 83584 bytes across all six
faces. The builder publishes each form's width, height and raw pitch, with depth
6. Ordinary cache metadata, CPU dirty and readability guards, and version
handling retain their existing behavior. This admission does not establish cube
pixel conformance.

## Native-source textures

A required native-source texture is reported as `kTextureUnresolved`, with no
CPU payload or memory version. The retained unsigned, single-level Rgba8 form
and one signed tiled RG16 float form have exact range owners. The float form is
nonstacked 2D, 1280x720, pitch 40 fetch units, raw signs `0x55`, swizzle
`0xB48`, and one unpacked level. Its Prepared view uses the canonical `0x398000`
tiled extent, including padding and the final guard. Ordinary cache metadata and
full-range watches are unchanged. Its unused unsigned companion retains
descriptor order but creates no second required native dependency. Unsigned or
mixed-sign float demand remains required and unsupported.

Only that exact signed owner may bypass its resolve's dirty interval. Unknown
dirty state, CPU overwrites, unsupported partial aliases, failed originals and
retired owners remain rejected. A successful original resolve retains
prospective native-source ownership; it does not establish native completion or
completed guest CPU readback. Consumers must bind the range to their own ordered
producer and actual completion owner.

`FrameInputNativeSources` uses the copy event ID as each armed plan's
generation. A plan with the same base address may replace a larger or smaller
range; an enclosing plan replaces every overlapping future view. Arming
immediately withdraws those views, and only successful original completion
publishes the new exact range as `kTextureUnresolved`. Completion and
abandonment use the armed generation so a stale result cannot affect a repeated
destination. An admitted texture view carries that ID in
`native_source_generation`, scoped to its observer stream; CPU and rejected
views carry zero. A paired view also carries the left copy ID in
`native_source_left_generation`, which is smaller; standalone, CPU and rejected
views carry zero there. Earlier draw recipes retain their copied producer
identity and native resource; the SDK sideband owns neither their bytes nor
their lifetime. Consumers must validate the new producer's format and layout,
including format changes over the same range; matching addresses alone never
establish the producer's identity. The `DrawCaptureFrameInputTextureView` layout
includes the 64-bit generation fields and `DrawCaptureFrameInputCopyView`
includes the destination origin. Rebuild the SDK and consumers for API floor
0.26.0.

Contained entries are unwatched and erased before checking the 4096-entry bound.
Superseded intervals with uncovered tails remain rejection coverage, including
the unused tail of a shrinking alias. Repeated same-base alias cycles therefore
retain at most two entries. Capacity exhaustion and invalid ranges fail the
owner closed. A crossing or interior overlap supersedes all overlapping current
views and records the new plan as rejected; unrelated ranges stay usable.
Physical invalidation and SharedMemory ownership loss reject affected current
views without resurrecting an earlier generation.

The tiled Rgba8 1280x720/pitch1280 composition admits two 640x720 halves,
including the selected clear-enabled left plan. Each half alone is rejected for
both member and full-destination views. The owner combines a right half with the
most recent current left member of the same composition key, pending or
completed, whose nonzero copy ID is smaller than the right ID. Draws and
unrelated resolves may intervene. The key matches destination base, width,
pitch, height, format and endian, and disjoint logical x-spans covering
`[0,1280)`. The left padded interval is `[0,0x384000)` and the right is
`[0x14000,0x398000)`, relative to the destination base. Physical callbacks and
the completed left member's SharedMemory watch reject intervening writes; an
overlapping third plan supersedes that member. The second Arm removes the first
watch before its own overlapping GPU write. Only successful completion of both
members publishes the `0x398000` union, identified by the right copy ID.
Failure, abandonment or invalidation of either member rejects the union.
Consumers must retain both ordered producer events, from
`native_source_left_generation` and `native_source_generation`, and establish
their own native completion; these padded ranges alone do not prove texel
coverage. Half admission checks the guarded guest rectangle and the planner's
normalized 640x720 rectangle and exact destination range. It requires the
selected copy control, Rgba8 destination info with endian zero, 1280x720
pitch/height and guest 2X sample encoding. It also requires the planner's
destination origin, `copy_dest_offset_x` and `copy_dest_offset_y`, to be zero:
`source_x`, `source_y` and the tile-aligned `planned_address` lower bound do not
show a window offset of 8, 16 or 24 rows, whose texels land below the tile
origin. The guest rectangle identifies left or right; the right destination base
includes the `0x14000` adjustment. Exact values of `PA_SC_WINDOW_OFFSET`,
`PA_SC_WINDOW_SCISSOR_TL`, `PA_SC_WINDOW_SCISSOR_BR` and `PA_SU_SC_MODE_CNTL`
are not composition gates: planning has already applied them to the normalized
rectangle, byte range and destination origin. A non-clear plan that fails the
half gate, including any other 640x720 Rgba8 copy, is armed as a standalone
plan. Standalone non-clear Rgba8 and RG16F plans retain their admission rules; a
clear-enabled plan that fails the gate is not admitted.

## Prepared copies

`WantsPreparedCopies` similarly opts in before registration to `OnCopyPrepared`
before original submission setup. A complete view requires a nonzero normal
resolve plan with guarded CPU geometry. The bounded color-0 Rgba8 main resolve
also admits the selected color/depth clear form: surface `0x0A010280`, depth
`0x530`, mode `6`, copy control `0x00100340`, destination pitch `0x02D00500`,
destination info `0x01000300`, HIZ clear `0xFFF`, depth clear `0xFFFFFF00`, zero
color-clear words, and the selected left resolve geometry. That geometry is
`[-0.5,-0.5,639.5,-0.5,639.5,719.5]` in guest float order. The paired no-clear
right resolve keeps the guest rectangle `[639.5,-0.5,1279.5,-0.5,1279.5,719.5]`;
`GetResolveInfo` derives the attachment-local source half for original and
prepared planning. The exact left clear tuple remains checked by the CPU
planner. Other clear words and forms, readback, array destinations, no-effect
and unsupported inputs remain incomplete. Planning and the prepared geometry
reader share one immutable CPU vertex snapshot. `planned_address` and
`planned_length` are prospective; they never mark guest memory dirty, promote a
version or establish native GPU completion. Prepared copies retain
`original_succeeded == false` and zero `written_*`. The later `OnCopy` retains
the ID and reports actual original results, including false with a nonzero
partial write. CPU preparation failure does not erase those actual results.

Prepared callback results remain advisory. Rejection or an exception retires the
observer through the existing mailbox rules and does not suppress the original
command. Observers that do not opt in receive no prepared callback.

## Prepared swaps

`WantsPreparedSwaps` fixes whether the observer receives prepared swaps when it
registers. The common CPU PM4 decoder calls `OnSwapDecoded` after consuming the
complete swap packet and setting its existing trace token, before `IssueSwap`.
The hook does nothing by default. D3D12 delivers `OnSwapPrepared` before
original submission or swap texture acquisition. Its first decoded boundary
calls `OnFrameBegin` once with the same view, then delivers the prepared
terminal. Subsequent terminals keep that stream open. Backend `frame` and
`submission` are observations that may repeat after original failure; the
observer token and ordered event ID identify the boundary. The later `OnSwap`
uses that exact token, ID and backend observations. Early original failure
consumes the ID even when no later swap view exists. A detached or replaced
observer receives no old terminal's post callback or End.

Swap registers borrow the final live register file. Both gamma spans borrow
callback-local copies of the actual CPU gamma owner, containing 256 table words
and 384 PWL words. Retain needed state by copying it before returning. Fetch-0
format selects table or PWL gamma, and `use_fxaa`/`is_8bpc` describe requested
CPU policy; original allocation failure may change realized post-processing. The
decoded frontbuffer values do not prove a native producer or provide pixels,
host resources, device, fence or output lifetime. The consumer must prove a
complete supported graph and retain its existing native terminal owners. If
optional terminal setup fails, an open prepared stream may receive the same
prospective ID and decoded scalars with empty register and gamma spans for
failure attribution. That state is incomplete and must be rejected. The SDK
retires only the selected observer and closes its local stream and interval
metadata; original issue continues.

Prepared swap provenance stays `original_succeeded == false`. The later
`OnFrameEnd` reports the actual original result, including false before source
acquisition. For prepared terminal observers, End(false) is an observation and
does not close accepted successor CPU preparation. Rejection, detachment and
native consumer failure retain their own stop rules. Legacy observers retain
failure-close behavior. The frame's buffer metadata and the guard for CPU dirty
intervals transition once at the decoded prepared terminal; the later original
callback does not transition them again. Shared-memory GPU ownership and CPU
overwrite precedence remain intact.

Prepared admission does not prove native recording, successful fence signaling,
completion, version promotion, guest writes or presentation. Actual native
completion and consumer-copy retirement remain the consumer's responsibility.
Original GPU work, ring progress, trace lifetime and original fences continue.
This terminal adapter covers D3D12; Vulkan has the common hook but no equivalent
frame-input terminal adapter. The bounded clear admission is internal planner
behavior and does not add public API or ABI surface.

## CPU packet admission

The common CPU command processor also exposes an optional packet-admission
callback for consumers that must bound a command subset before decoding. The
callback sees the packet header while the ring read index is still at the packet
start; returning false leaves that index unchanged, stops the worker, and causes
the next writeback to report the rejected start. Consumers publish write
pointers through `PublishWritePointer`, which rejects out-of-range values and
never restarts a failed worker; an in-flight sampled range completes its effects
and writeback before that rejection becomes terminal. `read_ptr_index()`,
`worker_running()`, `worker_failed()`, and `progress_sequence()` are
synchronized observations; progress is the final sequence point after writing
back to the guest in the correct byte order and publishing the terminal status.
Omitting packet admission preserves the legacy decoder behavior. `Initialize()`
reports worker construction or creation failure, and `Shutdown()` tolerates
partial startup. The common CPU command processor, register file and register
definitions are runtime-owned and accept borrowed CPU owners plus an explicit
legacy interrupt bridge; GPU plugin providers, MMIO, vblank and exports remain
plugin-owned. The GPU plugin ABI covers `GpuCreateInfo`, `IGraphicsSystem`
and its callback view layouts; see
[`gpu_plugin.h`](../include/rex/system/gpu_plugin.h).
