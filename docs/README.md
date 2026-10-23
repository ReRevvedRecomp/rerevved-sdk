# ReRevved SDK documentation

Use this index for the maintained ReXGlue fork. The SDK is a developer package,
not the player release.

## Play the game

- [ReRevved
  documentation](https://github.com/ReRevvedRecomp/rerevved/blob/main/docs/README.md)
  explains installation, settings, troubleshooting, online play, and mods.

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

## Contribution guidance

- [Contributing](../README.md#contributing) states branch, review, and commit rules.
- [Style guide](style-guide.md) covers maintained C++, CMake, scripts, and
  prose.
- [Agent guidance](ai_agents/README.md) defines scope and evidence requirements
  for automated contributions.
- [Test authoring](ai_agents/test-authoring.md) covers fixtures and regression tests.
