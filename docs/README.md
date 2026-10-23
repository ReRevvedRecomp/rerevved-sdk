# ReRevved SDK documentation

The ReRevved SDK is the maintained ReXGlue fork for ReRevved: Xbox 360
PowerPC-to-C++ recompilation and runtime, graphics, audio, input, and mod
support. It is a developer package, not the player release.

## Install

**To play:** use a [ReRevved
release](https://github.com/ReRevvedRecomp/rerevved/releases), not an SDK
package. Follow the [ReRevved
documentation](https://github.com/ReRevvedRecomp/rerevved/blob/main/docs/README.md)
to set up game files, adjust settings, play online, and use mods.

**To develop:** use the exact SDK commit and installed version in the title's
`rexglue-sdk.lock.json`. Upstream downloads do not contain this fork's changes.
The [SDK artifacts workflow](../.github/workflows/sdk-artifacts.yaml) builds a
selected full commit SHA. See [Development](development.md) to build and
install from source and [Maintaining the fork](MAINTAINING.md) for artifact
selection and consumer validation.

## System requirements

For the Windows x64 source build, use Clang 18 or newer, Visual Studio C++ Build
Tools with the Windows SDK, CMake 3.25 or newer, and Ninja. Start in an x64
developer environment with the repository's submodules initialized. Python is
needed for the formatting tools. The [development
guide](development.md#prerequisites) covers other presets and PowerPC test
tools; SDK presets do not establish title runtime support.

Players need their own legally obtained Xbox 360 Civilization Revolution files
and version 1.3 title update. See the title documentation for player system
requirements; the SDK is not a playable release.

## Build and package

- [Development](development.md) lists prerequisites, build and install commands,
  checks, tests, and the limits of each validation step.
- [Maintaining the fork](MAINTAINING.md) explains upstream integration, exact
  commit artifacts, consumer locks, and publication approval.
- [Upstream release process](RELEASING.md) records ReXGlue's version derivation
  and release process; its branch and publication commands are not fork policy.

## Integrate or extend

- [Runtime integration](runtime-integration.md) covers ReXApp lifetime,
  overlays, settings, session data, memory watches, and detached presentation.
- [Frame-input diagnostics](frame-input.md) defines prepared draw, copy, and
  swap contracts and their resource, lifetime, and completion limits.
- [Direct LAN System Link](LAN_SYSTEM_LINK.md) explains local identity, socket
  behavior, session state, and LAN-only constraints.
- [System Link relay](SYSTEM_LINK_RELAY.md) explains private relay setup,
  credentials, server options, logs, and security limits.
- [Relay client](relay-client.md) defines configuration, guest delivery, session
  discovery, Live invites, and disconnected behavior.
- [Relay protocol](relay-protocol.md) specifies wire messages and server limits.
- [Direct relay paths](relay-direct-paths.md) explains peer routing,
  authentication, fallback, and the trust model.
- [Mod system](mod-system.md) defines native and Lua packages, loadouts,
  publication, and asset overrides.
- [ReXGlue wiki](https://github.com/rexglue/rexglue-sdk/wiki) covers the
  upstream CLI and general setup; local pages own this fork's behavior.

## Contributing

Keep SDK changes reusable; title behavior belongs in the title repository.
Preserve upstream attribution, vendored code and dependency boundaries.

Open pull requests against `main`, the only maintained branch. Never commit
fork work to `upstream/main`; [Maintaining the fork](MAINTAINING.md)
covers upstream integration and consumer locks.

Installed `include/rex` headers, exported CMake targets, and the mod and GPU
plugin interfaces are public SDK surface. Follow the [release version
rules](RELEASING.md) when they change, and validate the title against the
new install before repinning it. Do not commit generated output, `out/` or
machine configuration.

Follow the [style guide](style-guide.md) and [Development](development.md)
for formatting, builds and tests. A pull request explains the problem, the new
behavior, any public interface change, and the tests run. Commit subjects use
`type: imperative summary`, at most 50 characters, with no body or parentheses,
a lowercase type and a lowercase opening verb. Upstream's
[CONTRIBUTING.md](../CONTRIBUTING.md) covers general setup.

- [Style guide](style-guide.md) covers maintained C++, CMake, scripts, and
  prose.
- [Contribution guidance](ai_agents/README.md) defines scope and evidence
  requirements for SDK changes.
- [Test authoring](ai_agents/test-authoring.md) covers fixtures and regression tests.

## Licensing

The ReXGlue SDK fork is BSD-3-Clause software by the ReXGlue and Xenia authors;
see [LICENSE](../LICENSE). This is an unofficial project and contains no game
assets. ReRevved is not affiliated with or endorsed by Firaxis Games, 2K or
Take-Two Interactive.
