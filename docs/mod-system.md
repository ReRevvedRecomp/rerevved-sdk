# Mod system

ReXGlue discovers manually installed native and Lua mod packages under `mods/`
beside the executable. Set `mods_data_root` to use another root. Discovery reads
package metadata and payload paths without executing package code or changing
the filesystem. F1 displays every package directly under the root, including
invalid records, and reports its compatibility and active state. These are the
SDK's default keys; ReRevved uses F2 for Mods and F1 for Settings.

## Package layout

```text
mods/<id>/
  mod.toml
  icon.png                         optional
  code/<runtime-platform>/<binary>
```

The folder name is the manifest package ID. IDs contain one to 63 lowercase
ASCII letters or digits separated by single hyphens. A folder and manifest ID
must match exactly. Discovery scans one level only; linked package folders are
not traversed, and linked manifest, platform, or binary paths are not accepted
as package payloads.

Recognized runtime platforms are `windows-x64`, `windows-arm64`, `linux-x64`,
`linux-arm64`, `macos-x64`, and `macos-arm64`. The binary name uses the native
loader convention: Windows uses `<code><config-postfix>.dll`, Linux uses
`lib<code><config-postfix>.so`, and macOS uses
`lib<code><config-postfix>.dylib`. Native code is loaded only from the directory
for the runtime platform; a flat `code/` binary is not a package payload.

## Version-1 manifest

```toml
manifest_version = 1

[mod]
id = "example-mod"
name = "Example Mod"
version = "1.0"
author = "Example Author"
description = "Demonstrates a native package."
min_game_version = "1.0.0"
code = "example_mod"
plugin_abi = 1
```

`manifest_version`, `[mod].id`, `name`, `version`, `code`, and `plugin_abi` are
required. Package versions use exactly two non-negative numeric components
(`major.minor`). `min_game_version` is optional, uses exactly three non-negative
numeric components (`major.minor.patch`), and requires the host game version to
meet the stated minimum. `code` is one filename stem without a path or
extension. Every valid package must provide a matching native binary for the
current runtime platform and declare the current plugin ABI.

`author` and `description` are optional display metadata. Unknown top-level or
`[mod]` fields produce visible nonblocking warnings. A malformed manifest,
identity mismatch, version mismatch, missing current-platform binary, or plugin
ABI mismatch remains visible as an invalid or incompatible package and blocks
that package from the enabled set.

## Version-2 Lua manifest

A Lua package contains `mod.toml` and one source file directly in its folder,
usually `main.lua`, plus optional `icon.png` and assets. It contains no native
payload.

```toml
manifest_version = 2

[mod]
id = "example-rules"
name = "Example Rules"
version = "1.0"
author = "Example Author"
description = "Publishes declarative rules to the title."
min_game_version = "1.0.0"

[payload]
kind = "lua"
entry = "main.lua"
api = "example.rules"
api_version = 1
requires = ["terrain_yield"]
```

The metadata rules match version 1. The single `[payload]` table requires
exactly `kind`, `entry`, `api`, `api_version`, and `requires`; extra payload
fields are errors. `kind` must be `lua`, `api` is a nonempty string, and
`api_version` is a positive 32-bit integer. `requires` is a nonempty ordered
array of distinct domain names, each 1 to 63 lowercase ASCII letters, digits,
underscores or hyphens. The title must advertise the same API and version and
every declared domain through `RuntimeConfig::mod_script_host`. A missing host,
contract, or domain blocks the package before any script runs. The title
validates the returned schema, including that every returned domain was
declared.

`entry` must name a regular `.lua` file directly in the package folder, not a
link. Separators, colons, absolute and traversal paths, and links or reparse
points are rejected. Activation rechecks the entry and reads it within the source
limit. Native `code` and `plugin_abi` fields anywhere in a version-2 manifest,
and a `code/` entry in the package, are blocking errors. Replace an installed
native package with a complete Lua package in a fresh directory; extracting over
it leaves `code/` and fails validation. Preserve local edits and backups outside
discovery roots.

### Lua environment and bounds

The SDK embeds [Lua 5.5.1](../thirdparty/lua/README.md). Every invocation uses a
fresh state, accepts text chunks only, and closes the VM before passing an owned
value tree to the title. A script returns exactly one value; the title owns its
required table shape and rule semantics. Scripts can use local functions,
arithmetic, conditionals, and loops to construct that value.

The environment exposes only `assert`, `error`, `type`, `ipairs`, `pairs`, and
`next`. There is no module loading, protected-call helper, I/O, OS, debug,
string library, metatable manipulation, or garbage-collection control. Globals
written by one package are absent from every other package's state. Rules run
natively after publication; no Lua callback remains in gameplay.

| Per-invocation resource | Default limit |
|---|---|
| Source | 256 KiB |
| Lua allocations, including compilation and VM state | 8 MiB |
| Execution | 1,000,000 VM instructions, checked every 1,000 |
| Returned table depth | 8, with the root table at depth 1 |
| Total returned key/value entries | 4,096 |
| Copied string bytes, including keys | 2 MiB |

The instruction limit permits at most one hook interval of overshoot. Source and
allocation bounds also cover compilation. Callers of `RunModScript` and
`RunModScriptFile` can supply `ModScriptLimits`; normal package launch uses the
defaults. Lua uses signed 64-bit integers and double-precision numbers. Integral
floats remain numbers in the owned representation.

The decoder uses raw table access and rejects metatables, cycles, unsupported
values (including functions, userdata, and threads), nonfinite numbers, and keys
other than integers or strings. Boolean, integer, finite number, string, and
table values are copied without coercion. Integer keys sort numerically before
string keys sorted by byte order. Shared acyclic tables are copied at each
occurrence and count against the same limits.

### Launch, publication, and status

Native plugins load during setup. Before the guest main thread resumes,
`DispatchModLaunch` invokes their `OnModuleLaunched` callbacks and activates Lua
through the title's [`IModScriptHost`](../include/rex/system/mod_script.h).
The host keeps its advertised contract descriptors immutable after registration.

A host that returns true from `supports_replacement()` supports live Lua
loadouts. Startup runs native callbacks in their relative package order, then
executes every selected Lua entry in its selected order. The SDK closes each VM
and owns every decoded value before calling `PrepareReplacement` once for the
whole Lua loadout. Native callbacks do not observe partially published Lua
bundles on this path.

`PrepareReplacement` validates all bundles and prepares one transaction across
every affected domain, including removal of rules owned by the previous Lua
loadout. It must preserve registrations owned by native plugins or other
providers. An empty request list removes all Lua-owned rules. Changed values,
package order, and repeated application use the same replacement contract. The
title owns rule composition and precedence. A title may reject preparation when
its current game or session cannot safely accept a rule change.

The returned `ModScriptReplacement` owns all storage and synchronization needed
for publication. `Commit()` cannot fail, throw, or allocate. Destroying the
transaction without committing cancels it. The host must not rescan or mutate
Runtime mod state while preparation is outstanding. Domain publication locks
may be released in Commit, but a title that synchronizes rule and metadata
readers must keep their shared boundary through transaction destruction.

The SDK prepares catalog state, active IDs, package metadata, and the Lua digest
before publication. For manager Apply, it atomically saves `mod_order.txt` after
host preparation and before Commit. A script, schema, support, preparation, or
file write failure cancels the replacement and preserves the previous running
Lua loadout and active metadata. Script and host failures also leave the saved
loadout unchanged. After Commit, the SDK publishes the prepared active state.
A package becomes active only after its rules commit. Startup never writes the
loadout file. One Lua failure rejects the whole Lua transaction while native
callback failures remain independent.

A host that leaves `supports_replacement()` false uses `Publish` for each
startup bundle in mixed package order. The SDK prepares activation storage
before calling Publish and acknowledges success with the allocation-free,
nonthrowing `ModScriptActivation::Commit`. Publish validates and atomically
publishes its bundle, returns success without allocating after publication,
and must not rescan or mutate Runtime mod state. A failed bundle leaves later
packages eligible to run. Manager changes for this host require restart.

The manager shows compatibility, activation, and the failure stage and message.
Read, compile, execute, budget, memory, decode, schema, support, and publication
failures retain their stages. Syntax and execution errors retain source lines.
Title errors identify returned fields or rules. Rescan refreshes metadata
without executing Lua and reattaches activation or failure by package ID.
Editing a file changes published rules only after a successful Apply. Active
means published. The title decides when a rule affects gameplay and whether
existing units, rewards, or other completed effects can change.

### Active digest and title notification

[`Runtime::active_mod_script_digest()`](../include/rex/runtime.h) returns the
cached SHA-256 hexadecimal digest of the last committed ordered Lua loadout.
It hashes a versioned, length-framed encoding of each Lua package ID, API name
and version, ordered declared domains, and the typed decoded value tree. Table
keys use the decoder's canonical order. Source formatting or comments that
produce the same decoded values do not change the digest. Integer and number
values retain their distinct types. An accepted empty Lua loadout has its own
digest. Before the first successful replacement the string is empty.

The digest excludes native DLLs, asset contents, and title-specific effective
rule interpretation. Package versions remain separate metadata. Matching Lua
digests and ID/version lists do not establish identical native code, assets, or
complete game state.

Register `Runtime::SetModLoadoutChangedCallback` after Runtime setup and before
module launch to observe startup as well as manager Apply. The callback runs
synchronously on the UI thread after rules, active catalog state, active package
metadata, and digest commit. The transaction remains alive through the callback,
so a host can retain a shared rule/metadata boundary until it returns. The
callback cannot veto the committed change. Its exceptions are contained. Keep
captured objects alive until the callback is cleared or Runtime shuts down.
The title is responsible for refreshing its session snapshot or other consumers
in this callback.
The SDK does not automatically apply Lua changes to remote players.

## Profile loadout

The active profile owns `mod_order.txt`. Each nonempty line is one exact package
ID in package load order. Whitespace is trimmed, blank lines and lines beginning
with `#` are ignored, and comments are not retained when F1 writes the file. An
absent file means an empty loadout and does not create a file at startup.

If an ID is malformed or repeated, a package is missing, or a selected package
has a blocking catalog diagnostic, none of the desired packages load. The game
still starts and F1 shows every line diagnostic so the file can be repaired. F1
stages enable, disable, and reorder changes. Save changes writes one ID per
line atomically, or a zero-byte file for an explicit empty selection. If the
host supports replacement, it also runs the live Lua transaction above, even
when the selection is unchanged. Replacing an invalid current file requires
explicit confirmation.

An application can refuse loadout changes, for example while a multiplayer game
runs, with `Runtime::SetModLoadoutLockProvider`. The provider returns a reason
while changes are locked and no value otherwise. An empty reason uses the
default text, a throwing provider counts as locked, and an empty function clears
it. It runs on the UI thread every frame and on any thread that applies a
loadout, so it must be cheap and thread-safe. While locked, `ApplyModLoadout` and
a persisting `ApplyModScriptLoadout` return `ModLoadoutApplyStatus::kLocked`
with the reason as the diagnostic, and F1 disables the enable checkboxes, load
order editing and Save changes and shows the reason. The lock is evaluated again
before writing the loadout file and committing the prepared replacement. A lock
raised by then cancels the apply with nothing saved or published. This check
narrows the window but cannot close it. The replacement host must enforce the
lock at publication. Startup dispatch replays the saved loadout and is not
blocked. `mod_loadout_lock_reason()` returns the current reason, or no value when
unlocked. Asset overrides are not covered by the lock.

Rescanning refreshes installed package facts while preserving the staged IDs and
their order. Closing F1 with unapplied changes requires explicit discard
confirmation. Package installation, replacement, and removal are manual.
The manager applies compatible installed Lua sources through the title host.

Copying the default profile to create a named profile includes `mod_order.txt`
and `asset_order.txt` when present, preserving explicitly empty files.

Native plugins retain the `rex_mod_create`, `rex_mod_abi_version`,
`OnCreateDialogs`, `OnModuleLaunched`, and `OnShutdown` ABI and lifecycle.
Applying a loadout does not load, unload, or reorder running native DLLs. Native
selection or order changes and changes to their discovered version or binary
path set the status to require restart. Replacing an active native package ID
with a Lua payload is rejected as requiring restart. The SDK does not certify changed
DLL bytes under an unchanged ID, version, and path.

## Asset overrides

Asset overrides are a separate package and loadout system. The runtime discovers
direct-child packages under `asset-overrides/` beside the executable:

```text
asset-overrides/<id>/
  asset-pack.toml
  assets/<logical-key>
```

Each `asset-pack.toml` uses `manifest_version = 1` and an `[asset_pack]` table
with required `id`, `name`, and `version` fields plus optional `author` and
`description`. The package folder must match `id`. The `assets/` tree must be
safe, contain no links or reparse points, and contain at least one regular file.
Asset packs contain no native code and do not use the gameplay plugin ABI.

The active profile owns `asset_order.txt`, independently of the gameplay
`mod_order.txt`. The runtime package may also ship
`asset-overrides/default_asset_order.txt` beside the asset-pack directories.
When the profile has no `asset_order.txt`, this bundled file supplies the
initial order; a profile file always wins, including an explicitly empty file.
The bundled file is parsed and validated with the same rules as the profile
file, and a malformed default produces diagnostics instead of being silently
accepted. The runtime still reports that no profile file exists. Saving creates
an override for that profile.

Each nonempty line is one package ID in priority order. The top line is highest
priority; the resolver checks it first and returns the first present regular
file for a logical key under `assets/`. It copies the bytes and reports the
winning package plus lower-priority shadowed regular files. Missing files fall
through. A present unsafe, unreadable, or oversized winning file is an error and
never falls through. Saving an asset loadout is atomic and takes effect after
restart; the Asset Overrides entry in the F1 Mods window stages enable and
priority changes independently from gameplay mod order. Live Lua application
does not reload asset packs or invalidate title or graphics asset caches.
