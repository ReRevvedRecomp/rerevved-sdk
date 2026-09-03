# Mod system

ReXGlue can load native code plugins from an ordered set of mod folders. The
host application owns title-specific APIs and guest semantics; plugins use the
generic lifecycle defined in `rex/system/mod_plugin.h`.

## Folder layout

The default mod root is `mods/` next to the executable. Set `mods_data_root`
to use another location. `enabled_mods` is a comma-separated list of folder
names in priority order:

```toml
enabled_mods = "game_api,state_inspector"
```

A distributable code mod uses this layout:

```text
mods/<name>/
  mod.toml
  icon.png
  code/
    windows-x64/<name>.dll
    windows-arm64/<name>.dll
    linux-x64/lib<name>.so
    linux-arm64/lib<name>.so
    macos-x64/lib<name>.dylib
    macos-arm64/lib<name>.dylib
```

The loader checks the running platform's directory first and retains support
for a flat `code/` directory for local development.

## Manifest

`mod.toml` supports these fields:

| Field | Meaning |
|---|---|
| `name` | Display name; defaults to the folder name. |
| `version` | Dotted numeric mod version. |
| `author` | Displayed author. |
| `description` | Displayed description. |
| `code` | Plugin binary stem without extension or build postfix. |
| `requires` | Comma-separated dependencies, optionally with `>= version`. |
| `load_after` | Comma-separated soft ordering hints. |
| `conflicts` | Comma-separated incompatible mods. |
| `game_version` | Minimum host version, with an optional `>=` prefix. |
| `platform` | Comma-separated packaged platforms. |

Missing dependencies, ordering mistakes, and conflicts are diagnosed without
preventing the title from starting. A verified mod or host version mismatch is
fatal because the plugin explicitly rejected that API version.

## Code plugin ABI

Every plugin exports:

```cpp
extern "C" REX_MOD_PLUGIN_EXPORT uint32_t rex_mod_abi_version();
extern "C" REX_MOD_PLUGIN_EXPORT rex::system::IModPlugin* rex_mod_create(
    uint32_t abi_version, const rex::system::ModHostContext* context);
```

The current ABI version is `rex::system::kModPluginAbiVersion`. The loader
rejects missing exports, ABI mismatches, missing binaries, and incompatible
runtime build configurations with a message naming the mod and path.

Plugins may implement three lifecycle methods:

- `OnCreateDialogs` registers overlays and keybinds after ImGui exists.
- `OnModuleLaunched` runs after the guest module and kernel state are ready.
- `OnShutdown` releases plugin-owned resources before host UI teardown.

F1 opens a read-only manager showing enabled mods in priority order. Enabling,
disabling, reordering, asset overlays, and online catalogs are separate
capabilities and are not part of this ABI layer.
