# Relay client

This page is for SDK integrators. For relay operation and security limits, see
[System Link relay](SYSTEM_LINK_RELAY.md). Wire formats are in [Relay
protocol](relay-protocol.md).

- [Configuration](#configuration)
- [Guest sockets and delivery](#guest-sockets-and-delivery)
- [Session directory client](#session-directory-client)
- [Round-trip probes](#round-trip-probes)
- [Live sessions](#live-sessions)
- [Live invites](#live-invites)
- [Disconnected relay](#disconnected-relay)

## Configuration

The client is disabled by default. F4 exposes these settings in `Online`:

| Setting | Audience | Default | Purpose |
|---|---|---|---|
| `system_link_invite` | Player | Empty | Code supplied by the relay host |
| `system_link_profile_name` | Player | Empty | Optional player name |
| `system_link_lan_enabled` | Player | `false` | Direct LAN mode |
| `system_link_xuid` | Advanced | `0` | Automatic identity or an explicit per-player XUID |
| `system_link_local_address` | Advanced | Empty | Selected direct-LAN IPv4 address |
| `system_link_relay_address` | Advanced | Empty | Explicit relay host:port |
| `system_link_relay_room` | Advanced | Empty | Explicit relay room |
| `system_link_relay_secret_file` | Advanced | Empty | Explicit path to the relay secret file |
| `system_link_advertised_address` | Advanced | Empty | Relay host:port that host invites tell other players to use |
| `live_sessions` | Player | `false` | List and join Live-mode sessions through the relay |
| `live_invite` | Advanced | Empty | Join a Live session from an invite code, for this session only |
| `system_link_direct` | Player | `false` | Try [direct paths](relay-direct-paths.md) to other relay players |
| `system_link_direct_drop_after` | Developer | `0` | Test only: drop received direct datagrams this many seconds into a direct path |
| `system_link_status` | Player, read-only | `Off` | Current session connection status |

All editable settings except `live_invite` and `system_link_direct_drop_after`
persist in configuration, and all require a restart.
`system_link_direct_drop_after` accepts 0-3600 and is session-only:
configuration files cannot set it and it is never saved. Pasting or editing a
string applies it when Enter is pressed or the edited field loses focus. Save
settings before restarting. The status is init-only, refuses writes, and is
never persisted.

A non-empty invite requests relay mode and supplies the endpoint, room and
secret. Combining an invite with any explicit relay address, room or secret-file
setting fails configuration. LAN and relay are mutually exclusive. Setting any
explicit relay option requests relay mode; all three must be supplied together.
For explicit configuration:

```toml
system_link_relay_address = "relay.example.net:30100"
system_link_relay_room = "private-test"
system_link_relay_secret_file = "relay-secret.txt"
system_link_lan_enabled = false
```

The address accepts an IPv4 literal or a hostname resolving to IPv4, followed by
an explicit port from 1 to 65535. The room is 1-32 bytes. Use the same room,
secret and executable build on both instances, with distinct XUIDs. The client
accepts secret files of at most 65,536 bytes (64 KiB), before ASCII whitespace
trimming; the server's file limit is 4 KiB. Both use the same trimming rule.
Only the path belongs in configuration; never put the secret on the command
line.

ReXApp takes one startup snapshot in `SetupEnvironment`, after `LoadConfig` and
logging initialization and before constructing the Runtime. Command-line values
already take precedence over configuration at this point, and the active config
path is known. The snapshot freezes mode, endpoint, room, secret source,
advertised address, local address, XUID, player name, `live_sessions`,
`system_link_direct` and `system_link_direct_drop_after`. Every runtime cvar
reader uses that snapshot; F4 edits affect the next process start. The invite
text remains in process memory, is shown in F4, and remains in the saved config.
Decoded invite secrets use wiped storage; each handshake receives its own wiped
copy. An explicit secret-file path is frozen, and the file is read for each new
handshake.

When System Link is requested and XUID is zero, startup generates
`0xB13E000000000000 | r`, where `r` is a nonzero 48-bit value from OS
randomness. The guest's `0x00C0000000000000` mask remains clear. Only
`system_link_xuid` is persisted through `SaveConfigSubset`; other config lines
are preserved. Large uint64 values are saved as quoted decimal strings because
TOML integers are signed. A failed write warns once and retains the generated
identity for the session. A configured or command-line nonzero XUID is never
replaced or written.

Player names accept 0-15 ASCII bytes: letters, digits, spaces, `-`, `_` and `.`,
without leading or trailing spaces. Invalid setter values are refused. In a
System Link snapshot, an empty name becomes `Player-XXXX`, using the XUID's last
four uppercase hex digits.

The snapshot entry point is internal to the runtime. The ReXApp source, compiled
by the consumer, forward declares it, so it needs no header outside the
installed SDK. Tests and other hosts that never initialize the snapshot continue
to read current cvar values, use explicit XUID configuration and retain the
`User` fallback; invite decoding, automatic identity and restart freezing
require snapshot initialization. Name validation still applies to the cvar
setter.

Status is `Off`, `LAN`, `Relay: waiting for System Link`, `Relay: connecting`,
`Relay: disconnected`, `Relay: connected as 198.18.x.y`, `Relay: lost
connection`, or `Relay failed: <reason>`. Once any direct attempt has run, the
connected status ends with the direct peer count, for example `Relay: connected
as 198.18.0.2 (direct: 1 of 1 peers)`. With `live_sessions` set, `Off` and `LAN`
read `Off (Live sessions inactive)` and `LAN (Live sessions inactive)`, because
the option needs relay mode. Failure reasons identify bad invites or settings,
an unresolved relay address, an unreachable relay (handshake timeout), a wrong
secret, a different game build, a full room, too many rooms, rate limiting, or
an unreadable executable. Status contains neither codes nor secrets. Waiting is
shown before the guest's first relay startup; connecting is shown only during
its handshake. Failed starts retain their reason through cleanup until a new
handshake begins. Normal final cleanup shows disconnected.

The first relay `XNetStartup` resolves the relay name and hashes the executable
on the calling thread before starting the HELLO/CHALLENGE/JOIN handshake.
Hashing work is bounded by the executable's file size; the OS resolver has its
own timing, and neither operation is covered by the five-second handshake
deadline. Startup waits on that thread for the handshake result. HELLO retries
every 500 ms until the first CHALLENGE. The client computes one JOIN and sends
it at most three times, 1.5 seconds apart, within that deadline. Duplicate
CHALLENGEs do not reset the outstanding JOIN. The client reads the secret
through OS file APIs directly into one bounded allocation, avoiding library
stream buffers. That allocation is explicitly wiped once WELCOME has derived the
association key (see [Direct paths](relay-direct-paths.md)), and on every
earlier exit, including rejection and timeout. HMAC wipes its key block, inner
and outer pad buffers, and temporary digests before returning; this does not
promise erasure of OS file caches or every internal SHA-256 implementation copy.
A relay that cannot be joined does not fail `XNetStartup`; see [Disconnected
relay](#disconnected-relay). There is no direct-LAN fallback. Repeated startup
preserves a live association and its socket mappings. Startup and cleanup
serialize the association check and identity configuration. A new startup joins
the dead worker and resets a failed or lost association before attempting a
fresh handshake, preserving guest ports and each open socket's native loopback
binding. Only cleanup clears the port table. If the relay assigns a different
virtual IPv4, startup reconfigures the shared identity while retaining sockets;
open sessions do not survive that address change.

The fingerprint is `SHA256(SDK_VERSION_BYTES || SHA256(HOST_EXE_BYTES))`: the
SDK version string has no terminator, and the inner digest is 32 raw bytes. The
running host executable is read once on the first relay startup in the process
in 256 KiB chunks; only a successful fingerprint is cached. An unreadable
executable leaves the relay disconnected and is retried on the next startup.
This detects executable and SDK version differences, not changes to external
assets, plugins or runtime DLLs that retain the same SDK version.

The assigned virtual IPv4 configures the existing LAN state, which supplies
XNADDR, address conversion, host session info, session details and QoS state.
The profile XUID/name and retail dashboard-version gates use the frozen System
Link mode. Relay mode applies the snapshot identity without enabling direct LAN.
These gates do not establish end-to-end title play.

## Guest sockets and delivery

Guest UDP and VDP sockets bind natively to `127.0.0.1` with OS-assigned ports. A
separate table owns up to 1,024 guest ports per process. Explicit collisions on
guest ports return `WSAEADDRINUSE`; sockets bound with port zero and senders
that were never bound receive virtual ephemeral ports in 49152-65535. Closing a
socket releases its guest port. Separate processes can both own guest ports
26900 and 1000. `getsockname` reports the assigned virtual IPv4 and guest port.
Stream sockets retain native behavior, including wildcard binds, and never enter
the relay port table. Relay socket classification is recorded separately from
the LAN registry at creation and survives startup and cleanup until the guest
closes the socket. Direct-LAN cleanup clears its socket kinds and port claims;
configuration while already enabled retains them.

`sendto` and synchronous `WSASendTo` enqueue DATA for limited broadcast
(`255.255.255.255`) or virtual unicast (`198.18.0.0/15`). Other destinations
return `WSAENETUNREACH`. Payload bytes, including the guest's VDP length prefix,
are unchanged. Payloads above 1,200 bytes return `WSAEMSGSIZE`. The send queue
holds at most 256 datagrams and returns `WSAEWOULDBLOCK` when full. A successful
send reports the full guest length accepted into that queue; UDP delivery is not
acknowledged.

One background thread owns the relay socket and one loopback injector socket.
The relay socket binds to `127.0.0.1` when the relay endpoint is loopback and to
all interfaces otherwise. DELIVER packets for registered ports are wrapped with
a random per-association 64-bit local token and the virtual source address and
port. The injector sends the envelope to the destination socket's native
loopback port. Native socket buffers bound incoming queues. Unknown guest ports
are dropped and counted. `recvfrom` accepts only the injector endpoint with the
matching token, strips the envelope and restores the virtual source. Invalid
local packets are silently consumed. Native blocking, nonblocking and `select`
readiness remain available. After discarding a non-envelope or otherwise invalid
datagram, a blocking `recvfrom` keeps waiting; a nonblocking receive reports
would-block when no valid packet remains. Each iteration reloads the current
injector and token. An error, including would-block, leaves the caller's source
address and address length untouched. Peek retains valid packets, and short
buffers preserve the host's UDP truncation behavior, including `WSAEMSGSIZE` on
Windows.

The thread waits with `select` until relay traffic, a local send/stop wakeup, or
the next handshake or heartbeat deadline. Windows UDP connection resets are
disabled; residual resets are counted as no-ops. The thread sends a heartbeat
every two seconds. Ten seconds without a datagram from the configured relay
endpoint marks the association lost and logs once; guest sends then fail with
`WSAENETDOWN`. Each successful `XNetStartup`, including the Ex form and a
startup that left the relay disconnected, takes a reference, and each
`XNetCleanup` releases one. Titles may send on open sockets after a cleanup and
may call startup more often than cleanup, in which case the association lasts
until the process exits and the relay expires the peer after its idle timeout.
Only the last cleanup sends BYE when joined, joins the worker and clears relay
state and the shared LAN state. After that, sends on sockets the guest still
holds report the full length and are dropped and counted, as a lossy network
would, rather than failing. Guest socket handles remain guest-owned.

Cleanup logs relay association state, deliveries, heartbeats and drops for
unknown ports, invalid wire packets, invalid loopback envelopes, rejected
destinations, full queues, oversized sends, socket errors, residual connection
resets, pending sends discarded at loss or cleanup, sends dropped after the last
cleanup, and the direct-path counters: attempts, established paths, fallbacks,
direct datagrams in and out, connect timeouts, probe failures, and drops for bad
tags, malformed frames, unknown peers, stale attempts, bad MACs, replays and
fault injection, and evicted peer entries. Association state values are 0
stopped, 1 joining, 2 live, 3 lost and 4 failed. The shared LAN packet/byte
counters retain their throttled debug reporting. Rejected-destination logs occur
on the first rejection and at most once every five seconds thereafter.
Deliveries count successful injector sends; shared receive counters count guest
reads. No payload or secret bytes are logged.

## Session directory client

The client exposes the directory to SDK code without blocking the calling
thread. `PublishSession` and `RemoveSession` queue an update; the client thread
sends queued updates one at a time, in order, and retries each every second
until the relay answers with STATUS or five attempts have been sent. Each update
carries the next update sequence of the association; a retry reuses it. A
rejected or unanswered update increments `directory_failed`. The update queue
holds at most 64 entries. Both calls return false when the association is not
live, the queue is full, or the record is invalid.

`BeginSearch`, `BeginLookup` and `BeginTokenRequest` return a nonzero request
handle, or 0 when 64 requests are already outstanding. `PollRecords` and
`PollToken` report pending, done or failed. Done carries the records reassembled
from every RESULTS page, or the granted token. A request keeps one request ID
for its lifetime and is retransmitted every second until it completes; it fails
after three seconds (timeout), on a STATUS reply (rejected), or when the
association ends. Pages from any attempt combine when they carry the same
directory snapshot. A page with a newer snapshot discards the pages collected so
far; a page with an older snapshot is ignored. Pages arriving after completion
are ignored. A request started while the association is not live fails at once.
The first done or failed poll consumes the handle; later polls report failed
with an invalid-handle reason. Failed requests increment `requests_failed`.

Association loss, failure and cleanup fail pending requests and forget queued
updates. The relay removes the peer's sessions when it removes the peer, so the
client keeps no published-session mirror.

`Start` also accepts a `JoinToken` instead of a room and secret. The token's
32-byte secret is held in the same wiped `Secret` storage as a room secret and
is wiped once WELCOME has derived the association key. The relay chooses the
room from the token. After a TOKEN_GRANT is processed, the client wipes the
receive buffer and the decoded secret, so the secret remains only in the
request's `Secret` storage.

The client supports the ordinary guest datagram path and synchronous
`WSASendTo`; overlapped receive remains unsupported. Datagram `connect`, `send`
and `recv` fail with `WSAEOPNOTSUPP`, so they cannot bypass relay routing or
expose loopback envelopes. The service's security, room, traffic and payload
limits in [Relay protocol](relay-protocol.md#state-and-limits) apply to every
client. Loopback tests establish SDK transport behavior. They do not establish
that title discovery or play over the Internet works.

## Round-trip probes

`BeginProbe` sends 1-8 probes to a virtual address and `PollProbe` reports
pending, done or failed with the count sent and one round trip per answered
probe. Probes are 16-byte DATA payloads (`RXQP`, kind 1 request or 2 reply,
sequence, two zero bytes, a 32-bit set id, four zero bytes) from guest port 0 to
guest port 0. Guest sockets never own port 0, so the receiving client answers or
consumes every port-0 datagram on its relay thread and none reaches a guest
socket; malformed ones and requests from other ports count as invalid. The relay
forwards probes as ordinary DATA under its packet and byte limits, so probes
need no protocol change. Port-0 payloads that start with `RXSD` are the SDK's
host session data side channel instead; see [Host session
data](runtime-integration.md#host-session-data-and-notifications).

A client answers requests only after `SetProbeReplies(true)`, which relay
`XNetStartup` sets when Live sessions are active. It sends probes from a
40-per-second token bucket and replies from an 80-per-second bucket, each with a
one-second burst; requests beyond the reply budget or a full send queue are
dropped and counted. A set is done when every probe was answered or one second
after its last probe; probes it could not send within two seconds are not sent.
At most 64 sets exist; a non-virtual address or a probe started without a live
association fails at once, and association loss fails pending sets.

## Live sessions

With `live_sessions` in a valid relay configuration, Live-mode XGI sessions use
the session directory. In LAN mode, with System Link off, or with the option
off, every XGI and XNet path behaves as without it.

Publication. A host create with matchmaking (`0x08`) and without arbitration
(`0x10`) is published. The record carries the session ID, key-exchange key,
flags, slot counts, the creator's contexts and properties, the
`X_PROPERTY_GAMER_HOSTNAME` property (`0x40008109`) set to the player name, the
XNADDR Ethernet bytes and the `XNetQosListen` data. Joins, leaves, modifies and
context, property or listen-data changes republish it at most four times a
second, inside the relay's ten updates per second. Delete, `XSessionEnd` and the
final `XNetCleanup` remove it; an ended session is never republished. A removal
the client cannot queue is retried until it is accepted. After association loss
the relay has already removed it. Ranked (arbitrated) and non-matchmaking
sessions are never published.

Search. `XSessionSearch` and `XSessionSearchEx` with a null results pointer or a
buffer below 8 bytes store `8 + 1326 * results` in the request's size field and
return 122 (`ERROR_INSUFFICIENT_BUFFER`). A request with an overlapped completes
asynchronously: dispatch returns pending and the overlapped completes with
status 0 when the directory answers, or with no results after 2.5 seconds. The
relay client retransmits a search every second and fails it after three, so 2.5
seconds allows three attempts and completes the guest first. A search without an
overlapped, for zero results, or for game type 0 (ranked) returns no results at
once. Game type (`0x800A`) and game mode (`0x800B`) contexts in the request must
match the record's; other contexts and properties are advisory. The searcher's
own session is excluded. `XMsgCancelIORequest` completes a pending search as
cancelled.

Results use the public XSESSION_SEARCHRESULT_HEADER (count, results pointer) and
92-byte XSESSION_SEARCHRESULT layout: XSESSION_INFO (XNKID, XNADDR, XNKEY), open
public and private slots, filled public and private slots, property and context
counts, then property and context pointers. XUSER_PROPERTY entries are 24 bytes
and XUSER_CONTEXT entries 8 bytes; string and binary values follow the arrays. A
string value ends at its first UTF-16 terminator, which its byte count includes;
a string without one is left out. The title's reads at these offsets match the
public Xbox 360 shapes. XNADDR `ina` is the host's virtual address and maps
unchanged through `XNetXnAddrToInAddr`. Whole trailing records that do not fit
are dropped, and every pointer stays inside the caller's buffer. Guest ranges
the service reads must be committed and readable; ranges it writes must also be
writable.

QoS. `XNetQosLookup` allocates one XNQOS block with the system heap, so
`XNetQosRelease` frees it, and sets each entry's pending count. Each virtual
target receives the requested probes; the entry reports probes sent and
received, minimum and median round trip, the target's listen data from the last
search, and the contacted and data-received flags. Unreachable, non-virtual and
server targets complete without data. The event is signaled when pending reaches
zero. Without Live sessions the export leaves every guest register unchanged.

Join. A joiner's session takes the listed record's flags without the host bit,
its slot counts and its filled slots, so `XSessionGetDetails` reports
matchmaking (`0x08`), the host's open slots, and invites disabled (`0x100`) only
when the host disabled them. The record's members occupy slots in addition to
joined members. A remote join consumes one of them in the slot kind the join
states; the leave of a member this client never saw join consumes one, public
first. Counts therefore follow later membership changes. This is an
approximation without the host's roster: a new member who joins before every
counted member is named also consumes a count, leaving occupancy one low until
those members are named. Do not treat these counts as an authoritative host
roster.

Each kernel arms the service at construction. Kernel teardown closes it before
kernel objects and guest memory are destroyed: the worker stops, pending
requests are dropped, and calls admitted earlier finish on their own reference.
The next kernel arms a new service.

Logging is debug-level and carries counts, sizes and timings only.

## Live invites

A Live invite lets a player join a friend's Live session from outside the guest:
from a chat client's join button, a pasted code, or the command line, whether
the game is closed or already running. The public API is
`include/rex/live_invite.h`; the invite layer knows nothing about any chat
client.

Code format. An invite is ASCII `rr1.` followed by unpadded base64url of:
version byte 1, the session ID (u64, big-endian), the join token ID (16 bytes),
the token secret (32 bytes), and the relay address as one length byte and
`host:port` text. The room is not carried; the relay takes it from the token. A
code is at most 128 characters, which is Discord's join secret limit, so the
relay address can be at most 35 characters; encoding fails for a longer one.
Decoding rejects any other prefix or version, characters outside the base64url
alphabet, nonzero padding bits, short or trailing bytes, a zero session ID, and
an empty or unparseable address. The token secret is held in wiped storage from
decode through the join.

Startup. `live_invite` (Advanced, String, restart required, never written to
configuration) applies to one launch, for example `--live_invite=rr1.<code>` on
the command line. When it is set as startup settings freeze, it selects relay
mode with the code's address, joins with the code's token instead of a room and
secret, forces `live_sessions` on, and keeps the session ID as the startup
invite. It takes precedence for that session over `system_link_invite`, the
explicit relay address, room and secret file, and `system_link_lan_enabled`:
they are ignored and LAN is off, but their saved values do not change. Startup
logs one info line naming the ignored settings, never their values.
`system_link_local_address`, `system_link_xuid` and `system_link_profile_name`
still apply. A code that does not decode is a configuration error. A title can
set it from `ReXApp::OnPreSystemLinkInit`, which runs after the configuration is
loaded and logging is up, immediately before the settings freeze.

Token lifetime. A join token can be used repeatedly until it expires, 900
seconds after the host requested it. The relay revokes it when the session is
removed. A token join cannot rejoin after its token expires: a later
`XNetStartup` that the relay rejects records
`Relay failed: invite expired; ask for a new invite`. The player then needs a
fresh invite and a restart.

Delivery. One accepted invite is pending per process: the session's record, for
user 0. The guest learns of it only through `XN_LIVE_INVITE_ACCEPTED`
(`0x02000002`, parameter 0), which is broadcast once the relay association is
live, a `LOOKUP` of the session has returned it, and an XNotify listener whose
mask covers the Live area (bit `0x2`) is open. The startup invite waits for the
association and retries a failed lookup up to five times a second apart. While
no such listener is open, including after the guest closes its last one, the
invite waits and the notification follows the next matching
`XamNotifyCreateListener`. Each invite is announced once.

`XInviteGetAcceptedInfo` (XLiveBase `0x00058023`, an argument list of a pointer
to the user index and the output pointer) then writes the 0x54-byte
XINVITE_INFO: the invitee XUID (`system_link_xuid`), inviter XUID 0, the running
title ID, an XSESSION_INFO with the session ID, the host XNADDR and key exactly
as a search result carries them, and fFromGameInvite 0. Without a pending
invite, or for another user index, it fails.

`XSessionSearchByID` (XGI `0x000B001B`) takes a 20-byte request: user index, the
64-bit session ID at +0x04, the buffer size at +0x0C and the result buffer at
+0x10. A null buffer or one below 8 bytes stores 1334 at +0x0C and returns
122. With Live sessions active, an overlapped request issues a `LOOKUP` and
completes like `XSessionSearch`, with the same result writer, one result
when the session is in this room and none otherwise; a request without an
overlapped answers at once from the pending invite. Returning the invited
session clears the pending invite; an invite never searched for is dropped
60 seconds after its notification. Without Live sessions the request
succeeds without writing results.

Host API. `CurrentHostInvite()` returns the invite code for the published Live
session this instance hosts, with the party size: occupied slots and public plus
private slots. The first call for a session requests a join token and returns an
empty code until it arrives. After that, the service requests a replacement in
the background when less than 300 seconds remain, so at most two tokens per
session are live; the title polls for the current code. No token is requested
before the title asks or after the session ends. The code is empty when this
instance hosts no published session, the association is not live, or the relay
address cannot be encoded.

The code carries the relay address other players should connect to. By default
that is the configured relay address, which is wrong for a host whose game runs
on the relay PC (the address is loopback) or behind a tunnel. That host sets
`system_link_advertised_address` to the public `host:port` (an IPv4 literal or
DNS name, port required, at most 35 characters in all; checked against the same
rules as an invite's address), and every invite code uses it. The host's own
connection still uses the configured relay address. An invalid or too long value
logs one warning at startup and is ignored.

Join API. `BeginJoin(code)` starts joining from a running game and `PollJoin()`
reports the result; neither blocks. The result is accepted when this instance is
in relay mode with Live sessions on, its association is live, its resolved relay
endpoint equals the code's, and a `LOOKUP` of the session returns it, which
proves the same room because `LOOKUP` sees only the requester's room. The
pending invite is then replaced and delivered as above. A relay host that
differs from the configured text is resolved on a background task. The other
results are:

| Result | When |
|---|---|
| Restart required | System Link off, LAN, a configuration error, Live sessions off, no live association, another relay, or the session not in this room |
| Invalid | The code does not decode |
| Unavailable | The code's relay cannot be resolved, or the lookup fails or cannot start |

For restart required, the title may relaunch with `live_invite` set to the same
code.

Session status. `CurrentSession()` reports whether the guest has a System Link
or Live session in any System Link mode, whether this instance hosts it, and its
occupied and total slots, for presence text such as "In a lobby". It does not
say what kind of game the session is for.

Discord client. `include/rex/discord_rpc.h` supports a party and join secret kept
apart from the presence and merged into every `SET_ACTIVITY` as `party.id`,
`party.size [current, max]` and `secrets.join`; clearing them removes the keys,
and `instance` stays false. With a join handler set, the client subscribes to
`ACTIVITY_JOIN` and `ACTIVITY_JOIN_REQUEST` after READY, passes each
`ACTIVITY_JOIN` secret to the handler on its worker thread, and logs join
requests, which the player accepts in Discord. `WaitForJoin` waits for one join
secret for the cold-start path: before `Start` it uses its own connection and
closes it cleanly; after `Start` it shares the worker's. The client refuses IPC
frames over 64 KiB, handles at most 16 frames per pass, and closes the
connection when a write is not accepted within 500 ms, so a misbehaving peer
cannot hold a join wait past its timeout. Discord errors are logged by code and
message only, with any Live invite code in the message replaced by `<invite>`,
and join secrets are wiped from memory after use. `discord_activity=false`
publishes no activity, party or secret, so friends see no join button, but join
events still arrive while the title has a handler or a waiter. A title that
offers invites passes `CurrentHostInvite()` as the join secret with a party ID
that does not reveal it, and passes a received secret to `BeginJoin`, or sets it
as `live_invite` before startup. The client connects to the first of
`discord-ipc-0` through `discord-ipc-9` that answers. `discord_instance_id=N`
(session only), or the `DISCORD_INSTANCE_ID` environment variable when the
option is -1, selects `discord-ipc-N` alone, so two clients signed in to
different accounts on one machine can each serve one game instance.

## Disconnected relay

On a console, `XNetStartup` fails only for invalid parameters or resource
exhaustion; a missing network appears afterwards through link status, title
XNADDR state and socket errors. Relay startup follows that model. Only a
configuration error, LAN combined with relay, or a zero XUID returns
`X_ERROR_INVALID_PARAMETER`. When the relay address does not resolve, the secret
or executable cannot be read, the handshake times out, or the relay rejects the
join, startup logs the reason, records it in the status as `Relay failed:
<reason>`, takes a reference and returns success. The title keeps running
without a network. Each later startup while the association is not live retries
the full handshake, with the same five-second deadline; a successful retry
clears the startup reason.

While the association is not live, the guest-visible APIs behave as on a console
without a network:

- `XNetGetTitleXnAddr` returns zero IPv4 fields with `XNET_GET_XNADDR_PENDING`
  only while a handshake is running, and `XNET_GET_XNADDR_NONE` otherwise.
  Titles poll a pending address until it changes, while NONE is terminal. The
  Ethernet bytes keep the last relay identity, or `CC` bytes when there is none,
  because some titles seed from them.
- `XNetGetEthernetLinkStatus` reports no link, as it does in every mode.
- Datagram `bind` and `sendto` fail with `WSAENETDOWN`, including after a
  failure before the handshake began; `recvfrom` on a socket without a relay
  port fails with `WSAEINVAL`; `select` keeps native readiness. After the last
  cleanup, sends are dropped as described above.
- Before the first join, `XNetQosListen` accepts valid requests and stores
  nothing; address conversions fail with an error; host session creation writes
  no session info; session details succeed without writing details. After a lost
  association these keep the last identity until a rejoin assigns a new one.
  With Live sessions active, session searches complete with zero results;
  otherwise they succeed without writing results.
