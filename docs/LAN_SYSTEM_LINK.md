# Direct LAN System Link

The SDK provides experimental support for System Link over a direct LAN
connection. It is disabled by default. It supplies local identity, session
state, and UDP socket behavior for title-owned discovery and gameplay protocols.
It does not supply a public matchmaking service or establish compatibility with
other titles.

For the separate client transport, which is off by default, see
[System Link relay](SYSTEM_LINK_RELAY.md). The two modes are mutually exclusive.
ReRevved remaps Settings to F1 and Mods to F2; its player guide owns
title-specific controls.

Enable it in the `Online` settings, opened with F4 by default in the SDK, or set
the following configuration values. Restart after changing them:

```toml
system_link_lan_enabled = true
system_link_local_address = "192.168.1.10"
system_link_profile_name = "LAN Host"
```

`system_link_local_address` must be an explicit local IPv4 address selected for
that process. ReXApp generates and persists a per-player `system_link_xuid` when
it is zero; an explicit nonzero value is retained. The optional
`system_link_profile_name` accepts at most 15 ASCII letters, digits, spaces,
`-`, `_` or `.`, without leading or trailing spaces. An empty name becomes
`Player-XXXX`, using the XUID's last four uppercase hex digits. Separate
instances must use distinct local addresses and XUIDs.

LAN enablement and player name are player settings; local address and XUID are
advanced settings. All are in `Online`, persist to configuration and require a
restart. ReXApp freezes them after config loading and before constructing the
Runtime. `system_link_status` is a read-only, unsaved player field showing `LAN`
when enabled. See [System Link relay](relay-client.md#configuration) for
snapshot, persistence, status and non-ReXApp host semantics. LAN plus an invite
or any explicit relay setting is a configuration error.

When enabled, wildcard socket binds use the selected address. The SDK exposes an
XNADDR identity local to the process, preserves exact XNADDR conversion round
trips observed by that process, and retains XNet QoS listener data as opaque
bytes. Cleanup discards those mappings, QoS state, and diagnostic counters.

When a title creates a session with the host flag (`XSESSION_CREATE_HOST`), the
SDK fills the caller's `XSESSION_INFO` with a fresh system-link session ID, the
local `XNADDR` as host address and a random key-exchange key, and writes a
random nonzero nonce. Non-host creates leave the host's `XSESSION_INFO`
untouched. LAN session details retain that info and nonce, creation flags, slot
capacities, joined member counts, and lobby/start/end state; delete or
`XNetCleanup` clears the record. `XSessionGetDetails` returns the 124-byte fixed
details without a member array and reports the required size for smaller
buffers.

With direct LAN enabled, `XamGetSystemVersion` reports retail dashboard
2.0.17559.0 so titles reach their session-details calls; `XNetStartupEx` and
`WSAStartupEx` forward to the ordinary startup.

The ordinary socket path implements `getsockname` so a title can read back the
selected endpoint after binding.

A datagram socket that the title never binds is bound to the selected address
and an ephemeral port before its first `sendto`. Its broadcasts and replies then
leave the selected interface rather than the host's lowest-metric route, and it
receives limited broadcasts addressed to its port on that interface. Windows
delivers limited broadcasts to sockets bound to a specific local address; this
behavior is not established on other platforms.

On Windows, a plain UDP bind of the wildcard address and a nonzero port also
takes a machine-wide wildcard claim on that port. A second instance binding the
same port then fails with `WSAEADDRINUSE`, so a title that searches for a free
port moves to the next one. Limited broadcasts reach every local instance, and
distinct ports keep broadcast-addressed traffic separate. VDP sockets do not
take claims, so each instance can bind the same VDP port on its own selected
address. VDP datagrams pass through unchanged, including any length prefix the
title writes. Closing the socket or `XNetCleanup` releases the claim; a repeated
`XNetStartup` keeps it.

Creating a session also binds the SDK's separate socket for host session data to
the selected address on UDP port 30110; see [Host session
data](runtime-integration.md#host-session-data-and-notifications).

Current diagnostics report configuration, bind failures, bounded packet and byte
counters, and QoS payload length. They do not log or interpret packet or QoS
payload contents.
