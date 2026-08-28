# Direct LAN System Link

The SDK contains an experimental, direct-LAN System Link foundation. It is
disabled by default and does not provide discovery, matchmaking, relay,
Internet play, or compatibility guarantees.

Enable it for one process with these init-only configuration values:

```toml
system_link_lan_enabled = true
system_link_local_address = "192.168.1.10"
system_link_xuid = 12771645593269305345
```

`system_link_local_address` must be an explicit local IPv4 address selected for
that process. `system_link_xuid` must be a nonzero, per-instance decimal value.
Startup fails when either value is unusable. Separate instances must use
distinct local addresses and XUIDs.

When enabled, wildcard socket binds use the selected address. The SDK exposes a
process-local XNADDR identity, preserves exact XNADDR conversion round trips
observed by that process, and retains XNet QoS listener data as opaque bytes.
Cleanup discards those mappings, QoS state, and diagnostic counters.

Current diagnostics report configuration, bind failures, bounded packet and
byte counters, and QoS payload length. They do not log or interpret packet or
QoS payload contents.
