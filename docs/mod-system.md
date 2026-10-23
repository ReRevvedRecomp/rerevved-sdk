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
points are rejected. Startup rechecks the entry and reads it within the source
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

Native plugins load during setup. Before the guest main thread resumes, a single
pass at module launch invokes native `OnModuleLaunched` callbacks and executes
Lua entries in the desired package order. A title's `IModScriptHost` validates,
stages, and atomically publishes each complete bundle. It returns success
without allocating after publication. The SDK prepares owned IDs and activation
storage before calling the host, then acknowledges success through
`ModScriptActivation::Commit`, which neither allocates nor throws. The host must
keep its descriptors immutable and must not rescan or mutate Runtime mod state
inside `Publish`.

A package becomes active only after successful publication. A read, compile,
execute, budget, memory, decode, schema, support, or publish failure affects
only that package; later packages still run. Desired IDs and `mod_order.txt`
remain unchanged. F1 shows compatibility, activation, and the failure stage and
message. Native packages also show a Native label because their changes require
a restart. Syntax and execution errors retain source lines; title errors
identify returned fields or rules without inventing source lines.

Rescan refreshes metadata without running Lua and reattaches session activation
or failure by package ID. A startup failure does not block saving a repaired,
otherwise compatible package for restart. Editing a running package changes no
published rules. Active means published, not that a rule is effective in every
gameplay situation.

## Profile loadout

The active profile owns `mod_order.txt`. Each nonempty line is one exact package
ID in package load order. Whitespace is trimmed, blank lines and lines beginning
with `#` are ignored, and comments are not retained when F1 writes the file. An
absent file means an empty loadout and does not create a file at startup.

If an ID is malformed or repeated, a package is missing, or a selected package
has a blocking catalog diagnostic, none of the desired packages load. The game
still starts and F1 shows every line diagnostic so the file can be repaired. F1
stages enable, disable, and reorder changes; Save changes writes one ID per line
atomically, writes a zero-byte file for an explicit empty selection, and leaves
the running package set unchanged until restart. Replacing an invalid current
file requires explicit confirmation.

Rescanning refreshes installed package facts while preserving the staged IDs and
their order. Closing F1 with unapplied changes requires explicit discard
confirmation. The manager does not install, import, update, replace, delete, or
reload packages.

Copying the default profile to create a named profile includes `mod_order.txt`
and `asset_order.txt` when present, preserving explicitly empty files.

Native plugins retain the `rex_mod_create`, `rex_mod_abi_version`,
`OnCreateDialogs`, `OnModuleLaunched`, and `OnShutdown` ABI and lifecycle.

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
priority changes independently from gameplay mod order.
