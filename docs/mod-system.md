# Mod system

ReXGlue discovers manually installed native mod packages under `mods/` beside
the executable. Set `mods_data_root` to use another root. Discovery reads
package metadata and payload paths without loading native code or changing the
filesystem. F1 displays every direct-child package, including invalid records,
and reports its compatibility and active state.

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
`lib<code><config-postfix>.dylib`. Native code is loaded only from the
platform-qualified directory; a flat `code/` binary is not a package payload.

## Version-1 manifest

```toml
manifest_version = 1

[mod]
id = "state-inspector"
name = "State Inspector"
version = "1.0"
author = "Aeshur"
description = "Displays title state for development."
min_game_version = "1.0.0"
code = "state_inspector"
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

## Profile loadout

The active profile owns `mod_order.txt`. Each nonempty line is one exact package
ID in native load order. Whitespace is trimmed, blank lines and lines beginning
with `#` are ignored, and comments are not retained when F1 writes the file.
An absent file means an empty loadout and does not create a file at startup.

Malformed IDs, repeated IDs, missing packages, and selected packages with
blocking catalog diagnostics fail closed for the complete desired set. The game
still starts and F1 shows every line diagnostic so the file can be repaired.
F1 stages enable, disable, and reorder changes; Apply writes one ID per line
atomically, writes a zero-byte file for an explicit empty selection, and leaves
the running native set unchanged until restart. Replacing an invalid current
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
safe, contain no links or reparse points, and contain at least one regular
file. Asset packs contain no native code and do not use the gameplay plugin
ABI.

The active profile owns `asset_order.txt`, independently of the gameplay
`mod_order.txt`. The runtime package may also ship
`asset-overrides/default_asset_order.txt` beside the asset-pack directories. When the
profile has no `asset_order.txt`, this bundled file supplies the initial order;
a profile file always wins, including an explicitly empty file. The bundled
file is parsed and validated with the same rules as the profile file, and a
malformed default produces diagnostics instead of being silently accepted. The
runtime still reports that no profile file exists, so saving materializes a
profile-local override.

Each nonempty line is one package ID in priority order. The top line is highest
priority; the resolver checks it first and returns the first present regular
file for a logical key under `assets/`. It copies the bytes and reports the
winning package plus lower-priority shadowed regular files. Missing files fall
through. A present unsafe, unreadable, or oversized winning file is an error
and never falls through. Saving an asset loadout is atomic and takes effect
after restart; the Asset Overrides entry in the F1 Mods window stages enable
and priority changes independently from gameplay mod order.
