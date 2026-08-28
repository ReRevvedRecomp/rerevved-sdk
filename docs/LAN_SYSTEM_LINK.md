# Direct LAN System Link

The SDK contains an experimental, direct-LAN System Link foundation. It is
disabled by default and does not provide discovery, matchmaking, relay,
Internet play, or compatibility guarantees.

Enable it for one process with these init-only configuration values:

```toml
system_link_lan_enabled = true
system_link_local_address = "192.168.1.10"
system_link_xuid = 12771645593269305345
system_link_profile_name = "LAN Host"
```

`system_link_local_address` must be an explicit local IPv4 address selected for
that process. `system_link_xuid` must be a nonzero, per-instance decimal value.
`system_link_profile_name` is an optional per-instance display name of at most
15 bytes. Startup fails when either required value is unusable. Separate
instances must use distinct local addresses and XUIDs, and should use distinct
display names.

When enabled, wildcard socket binds use the selected address. The SDK exposes a
process-local XNADDR identity, preserves exact XNADDR conversion round trips
observed by that process, and retains XNet QoS listener data as opaque bytes.
Cleanup discards those mappings, QoS state, and diagnostic counters.

The ordinary socket path implements `getsockname` so a title can read back the
selected endpoint after binding.

Current diagnostics report configuration, bind failures, bounded packet and
byte counters, and QoS payload length. They do not log or interpret packet or
QoS payload contents.
