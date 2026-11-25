# System Link online server

`rexglue online-server` is a title-neutral UDP service for private tests. It
forwards opaque guest payloads between authenticated members of a room. A shared
password authenticates joining; the client chooses a 32-byte build fingerprint and the
first member fixes the room's fingerprint until the room empties. Each room also
has a session directory, and a session host can obtain short-lived join tokens
that admit other players to its room without the password.

This service is not for public deployment. Payloads and session tokens are
unencrypted. DATA has no payload integrity or replay protection. Anyone who can
observe a session token and spoof its source endpoint can impersonate that player.
The password does not authenticate the server to the client. The
fingerprint is a compatibility assertion by the client, not build attestation.
Pre-authentication state is bounded, but the service has no global traffic
limiter. Broadcast can multiply aggregate traffic even though each outgoing
datagram is no larger than its triggering datagram. Directory replies can be
larger than their requests; they go only to an authenticated member's observed
endpoint and are bounded by the directory rate limits. Directory records and
join tokens are unencrypted on the wire; anyone who can observe a TOKEN_GRANT
can join the token's room until the token dies. Optional [direct
paths](relay-direct-paths.md) authenticate the datagrams they carry and reveal
members' LAN addresses to the rest of the room.

The handshake has no stateless challenge, so requests with spoofed source
addresses can exhaust its bounded pending state.

Keys in this page use SDK defaults: F4 Settings and F1 Mods. ReRevved uses F1
Settings and F2 Mods. Use the [ReRevved
documentation](https://github.com/ReRevvedRecomp/rerevved/blob/main/docs/README.md)
for the title's hosting scripts and Discord Join flow.

## Run

1. Run `rexglue online-invite --address server.example.net:30100 --password-file
   server-password.txt --create-password` with an address the other players can reach.
   It prints one invite code line to stdout. Share that code privately with
   the players joining your game.
2. Run `rexglue online-server --password-file server-password.txt`. Use `--bind 127.0.0.1`
   for a loopback-only test, or bind the selected private interface.
3. Each player opens F4 settings, selects `Online`, pastes the code into
   `system_link_invite`, saves settings, and restarts the game. A player name is
   optional. No launch flags are needed.
4. Stop the online server with Ctrl+C.

`online-invite` defaults to room `testers`; `--room <name>` selects another room.
It validates the IPv4 or DNS `host:port` text without resolving the host.
`--create-password` exclusively creates a missing file with 32 random Crockford
base32 characters and a newline using the system's random source. An existing
file is read unchanged. Without the flag, the file must exist. Invite creation
requires a regular file of at most 4 KiB and a password of 16-64 bytes after
trimming leading and trailing ASCII whitespace (`space`, tab, CR, LF, form feed,
vertical tab). These exact trimmed bytes authenticate against
`rexglue online-server` using the same file. The server itself accepts any
nonempty trimmed password within its limit on file size. Codes and passwords are
never logged.

## Server options

| Option | Default | Accepted range |
|---|---|---|
| `--bind` | `0.0.0.0` | Numeric IPv4 address |
| `--port` | `30100` | 1-65535 |
| `--password-file` | Required | Regular file, at most 4 KiB, non-empty after trimming |
| `--max-rooms` | 16 | 1-256 |
| `--max-members` | 4 | 1-64 per room |

## In-process server

A title can run the same server on a host thread through
`rex/online_server.h`, so one player hosts without starting `rexglue`.
`MakeInvite` returns the invite code for an address and a password file, and
creates the file with a random password when it is missing, as `rexglue
online-invite --create-password` does with the room `testers`. `Start` takes
the bind address, UDP port, the same password file and the member limit; it
opens the port before it returns, so a port in use is reported to the caller.
`Stop`
closes the port and joins the thread. `Current` reports the port and the
joined player and game counts. One server runs per process, with the default
room limit of 16. No thread or socket exists before `Start`, and the thread
sleeps on its socket for up to 100 ms between packets. It logs one line when
it starts listening and one when it stops; the status and problem lines below
belong to the command.

## Router port

`rex/port_mapping.h` asks the home router to forward an outside UDP port to
the server, so a host needs no router setup. `Open` starts a host thread and
returns at once; `Current` reports the state, and while the port is open the
public IPv4 address and outside port to put in an invite. `Close` removes
the mapping and can wait a few seconds for the router. Nothing is sent before
`Open`.

The thread tries UPnP first, on Windows only, through the system's
`IUPnPNAT`. It takes the first of ten outside ports from the server's port
that no other mapping holds; a mapping with the same label that points at
this computer counts as its own. It then tries NAT-PMP (RFC 6886) on the
default gateway, UDP port 5351, with a lifetime of one hour renewed at half
that; a failed renewal is asked again until the lifetime runs out. On Linux
the gateway comes from `/proc/net/route`; on macOS no gateway is looked up,
so the port is never opened there.

| Failure | Meaning |
|---|---|
| `kNoNetwork` | No network connection with a default gateway. |
| `kNoDiscovery` | The Windows service SSDP Discovery is disabled. |
| `kNoRouter` | Nothing answered UPnP or NAT-PMP. `public_network` is set when a connected Windows network is Public, which hides the router from UPnP. |
| `kPortsTaken` | Other mappings hold all ten outside ports. |
| `kSharedAddress` | The router's outside address is private or shared by the provider, so a mapping cannot be reached; the mapping is removed. |
| `kRefused` | The router answered and refused, or stopped renewing. |

The log names the method, the ports and the failure, never the outside
address.

## Hosting from the game

`online_host` (Advanced, Boolean, restart required) makes one launch host,
for example `--online_host=true` on the command line; a title relaunches
itself with it. It is session-only: never written to configuration and not
settable from it. As startup settings freeze, the game:

1. Reads the password in `online-server/server-password.txt` beside the
   configuration file, creating it on the first launch that hosts.
2. Starts the in-process server on `online_host_port` (default 30100, saved in
   configuration). A port in use becomes the `system_link_status` failure.
3. Selects relay mode joined to `127.0.0.1` on that port with room `testers`
   and that password. This takes precedence for the session over
   `live_invite`, `system_link_invite`, explicit relay settings and LAN, whose
   saved values stay.
4. Asks the router for the port in the background. When it opens, the public
   address and outside port become the advertised address that host invites
   carry; until then, and after a failure, the game hands out no host invite.
   With `system_link_advertised_address` set, that address is used and the
   router is left alone.

Any configuration error at the freeze stops the server again and skips the
router.

`rex/online_host.h` reports this to the title: whether the launch hosts, why
the server did not start, the router port's state and failure, the address
friends reach and the invite code for it. The application stops the server
and removes the router port when it closes. Without `online_host` no server
thread, socket or router request exists.

## Server logs

At startup the server logs its bind address, UDP port, room limit and member
limit once, for example `Online server listening on 0.0.0.0:30100 (up to 16
rooms, 4 members per room)`. At the default info level, it then reports player
or room count changes, such as `Online server: 2 players in 1 game`. Players are
authenticated members, not pending handshakes. An unchanged or empty server
stays quiet.

Rejected requests and socket problems produce warnings naming only counter
increases, for example `Online server problems: malformed=+3 auth=+1`. Warnings
are limited to one every five seconds; increases during that interval accumulate for
the next warning, even if traffic stops. All drop reasons except normal idle
expiry contribute warnings, including directory and direct-path rejections.
Receive errors, send errors and connection resets also contribute. Idle expiry
is reflected in the player count instead.

Use `--log-level debug` or `--verbose` for the full diagnostic counters: rooms,
peers, pending handshakes, directory sessions, live join tokens, direct-path
attempts, input/output packets and bytes, drops by reason, and socket errors.
Only nonzero counters are printed, at most once every five seconds, for example
`Online server counters: rooms=1 peers=2 packets_in=50 bytes_in=5000`. No
diagnostic line is printed when all counters are zero. Ctrl+C logs a one-line
`Online server stopped:` summary of packet and byte totals, rejected requests (excluding idle
expiry), receive/send errors and connection resets, including increases still
waiting for a warning. These logging levels apply to both the console and a file
selected with `--log-file online-server.log`.

Directory drop reasons are `directory_rate_limited`, `session_conflict`,
`session_limit`, `token_denied` and `token_limit`. No packet payload, room name,
fingerprint, nonce, token, password, session ID, session key or record content is
logged. Core output counters count datagrams submitted to the socket; failed
sends also increment the transport's counter for send errors. Windows UDP
connection resets are disabled; residual resets are counted as no-ops separately
from receive errors. The receive waits for traffic until the next maintenance
deadline, at most 100 ms away. It forwards arrivals immediately and returns to
the Ctrl+C stop check after each iteration without a sleep.

## Invite format

An invite contains the password; treat the entire code as a credential. It is
`RXR1-` followed by unpadded Crockford base32 with alphabet
`0123456789ABCDEFGHJKMNPQRSTVWXYZ`. Encoding emits uppercase without grouping.
Decoding accepts either case, ignores ASCII spaces and dashes after the prefix,
and maps `O` to `0` and `I` or `L` to `1`.

The decoded bytes, in order, are:

| Field | Encoding |
|---|---|
| Version | One byte, 1 |
| Port | Two bytes, little-endian, 1-65535 |
| Host | One-byte length, then 1-253 ASCII letters, digits, `-` or `.` |
| Room | One-byte length, then 1-32 opaque bytes, as in the relay protocol |
| Password | One-byte length, then 16-64 bytes |
| Checksum | First four bytes of SHA-256 over all preceding bytes |

Invalid fields, nonzero trailing base32 bits, truncation, trailing payload and
checksum errors report `invite code is damaged or incomplete`. Only a wrong
prefix (`not an invite code`) and a checksummed higher payload version (`invite
code was made for a newer version`) have distinct errors.

## SDK client

See [Relay client](relay-client.md) for startup configuration, guest sockets,
session discovery, Live invites, and disconnected behavior.

## Protocol v2

See [Relay protocol](relay-protocol.md) for wire encoding and directory
messages.

## State and limits

See [State and limits](relay-protocol.md#state-and-limits) for server-side
handshakes, traffic budgets, session lifetime, and resource bounds.

## Direct paths

See [Direct relay paths](relay-direct-paths.md) for peer routing, keys,
authentication, fallback, and the trust model.

## Build and focused tests

Follow [Development](development.md#check-the-changed-surface) for
configuration, builds, and the focused network test command.

The codec, authentication, core rules, and UDP loopback use the existing Catch2
unit target. The loopback case skips only when loopback sockets cannot be
opened. Client tests cover the handshake, guest-port table, envelope validation,
loopback delivery with select, broadcast replies, unchanged VDP prefixes,
wrong-password rejection, heartbeat/loss, and timeout behavior. Directory tests
(`[directory]`) cover the record and message codecs, paging at the size limit,
room isolation, filters, ownership, every limit, session and token lifetime,
token joins, update ordering, repeated token grants, the version 1 reply, and a
two-client publish, search, lookup, token join and remove through the loopback
server. Client tests drop, reorder and duplicate server replies and cover request
and update saturation and failure on cleanup and loss. Live session tests
(`[live_sessions]`) cover the option and inactive status, publication, republish
and removal, the search writer and size probe, deferred completion, filters, QoS
lookups, probe budgets, and a two-client publish, search, QoS lookup, join,
transport and delete through the loopback server. Direct-path tests (`[direct]`)
cover the message and frame codecs with malformed, truncated and oversized
input, the replay window, `K_assoc` for room and token joins, pair-key wrapping
and bad tags, relay availability, rate limits, candidate bounds, forged
CANDIDATES and CONNECT_REQUEST tags and CONNECT_CLOSE, the options and status,
and, through the loopback server, an established path that stops relay DATA for
the pair, forced fallback, spoofed, stale and replayed frames, a replayed
CONNECT_INFO after CONNECT_CLOSE, a CONNECT_INFO delayed past its CONNECT_CLOSE,
a late probe acknowledgement, peer-table eviction, one-way loss of one side's
sends, mid-match path loss with backoff, and relay-only operation without the
option, without peer support and against a server without direct-path types. Live
invite tests (`[live_invite]`) cover the code format and limits, `live_invite`
configuration and an expired token, notification delivery, XINVITE_INFO, search
by ID, the running-game join results, and host tokens and their refresh. Discord
tests (`[discord]`) cover activity payloads with and without a party and join
secret, and join event parsing.
