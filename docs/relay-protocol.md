# Relay protocol

This is the wire reference for the private [System Link
relay](SYSTEM_LINK_RELAY.md). See [Relay client](relay-client.md) for guest
behavior and [Direct relay paths](relay-direct-paths.md) for authenticated peer
traffic.

## Protocol v2

Each UDP datagram starts with this 8-byte header. Multi-byte integers are
little-endian except IPv4 fields, which are four network-order octets. Guest and
endpoint ports in the internal API are host-order numeric values. IPv4 values in
that API use the same in-memory representation as `in_addr::s_addr`.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | ASCII `RRLY` |
| 4 | 1 | Version, 2 |
| 5 | 1 | Type from the table below |
| 6 | 2 | Total datagram byte length, including header |

The body begins at offset 8. `N` is the length of the room name in bytes, and
`P` is the length of the payload in bytes. There is no terminator or implicit
padding. A room name is 1-32 opaque bytes, compared byte-for-byte.

| Type | Direction | Body in wire order | Total bytes |
|---|---|---|---|
| 1 HELLO | Client to relay | client nonce[16], N:u8, room[N], fingerprint[32] | 57+N |
| 1 HELLO (token) | Client to relay | client nonce[16], 0:u8, token id[16], fingerprint[32] | 73 |
| 2 CHALLENGE | Relay to client | server nonce[16] | 24 |
| 3 JOIN | Client to relay | HMAC-SHA256[32] | 40 |
| 4 WELCOME | Relay to client | peer id:u32, token:u64, virtual IPv4[4] | 24 |
| 5 REJECT | Relay to client | reason:u8 | 9 |
| 6 DATA | Client to relay | token:u64, source guest port:u16, destination IPv4[4], destination guest port:u16, payload[P] | 24+P |
| 7 DELIVER | Relay to client | source virtual IPv4[4], source guest port:u16, destination guest port:u16, broadcast:u8, payload[P] | 17+P |
| 8 HEARTBEAT | Client to relay | token:u64 | 16 |
| 9 ACK | Relay to client | token:u64 | 16 |
| 10 BYE | Client to relay | token:u64 | 16 |
| 11 PUBLISH | Client to relay | token:u64, request:u32, sequence:u32, record | 24+R |
| 12 REMOVE | Client to relay | token:u64, request:u32, sequence:u32, session id:u64 | 32 |
| 13 SEARCH | Client to relay | token:u64, request:u32, max results:u16, required flags:u32 | 26 |
| 14 LOOKUP | Client to relay | token:u64, request:u32, session id:u64 | 28 |
| 15 RESULTS | Relay to client | request:u32, snapshot:u32, page:u16, page count:u16, count:u8, records | 21+sum(R) |
| 16 TOKEN_REQUEST | Client to relay | token:u64, request:u32, session id:u64 | 28 |
| 17 TOKEN_GRANT | Relay to client | request:u32, token id[16], token secret[32], expiry seconds:u32 | 64 |
| 18 STATUS | Relay to client | request:u32, status:u8 | 13 |
| 19 CANDIDATES | Client to relay | token:u64, count:u8, count x (IPv4[4], port:u16), tag[16] | 33+6C |
| 20 CONNECT_REQUEST | Client to relay | token:u64, target virtual IPv4[4], tag[16] | 36 |
| 21 CONNECT_INFO | Relay to client | attempt id:u32, other virtual IPv4[4], observed IPv4[4], observed port:u16, count:u8, count x (IPv4[4], port:u16), wrapped key[32], tag[16] | 71+6C |
| 22 CONNECT_CLOSE | Relay to client | attempt id:u32, other virtual IPv4[4], tag[16] | 32 |

`C` is the candidate count, 0-4. Candidate and observed endpoints have a nonzero
address and port, and attempt ids are nonzero. A relay without types 19-22 drops
them as malformed, so a client against it never receives CONNECT_INFO and stays
on the relay. See [Direct paths](relay-direct-paths.md).

REJECT reasons are 1 version, 2 malformed, 3 authentication, 4 room full, 5 too
many rooms, 6 fingerprint mismatch, and 7 rate limited. The broadcast flag is
exactly 0 or 1. DATA to `255.255.255.255` broadcasts to every other member in
the sender's room. Unicast goes only to the room member owning the destination
address, including the sender if it addresses itself. Unknown destinations and
messages in the wrong direction are dropped and counted.

A version 1 HELLO whose length matches its room length receives a 9-byte version
1 REJECT with reason 1, so an older client reports a protocol version mismatch.
Every other datagram with an unsupported version is dropped silently. Both cases
count as `version` drops.

The JOIN HMAC key is the trimmed secret's bytes. Its input is the exact
concatenation of ASCII `RRLY-JOIN-v1` (no terminator), client nonce[16], server
nonce[16], room length:u8, room bytes, and fingerprint[32]. The relay looks up
the pending HELLO by observed UDP source IPv4 and port. An identical HELLO
resends the same challenge; changed HELLO fields replace the challenge without
extending its original expiry or resetting its failed-JOIN count. A failed JOIN
retains the pending challenge until the third failed attempt, which drops it. A
successful JOIN consumes the challenge and retains its WELCOME on the peer,
keyed by source endpoint and JOIN auth digest. An identical JOIN resends the
same WELCOME without allocating another peer, token or address or counting a
failure. Retention ends at the first authenticated HEARTBEAT or DATA, or five
seconds after the successful JOIN, whichever comes first; retries do not extend
it. BYE and peer removal also discard the retained reply. Subsequent JOIN
replays are rejected without creating a peer. HELLO from a joined endpoint
starts a rejoin; a successful replacement releases the old peer and address
before assigning the new identity. A failed rejoin leaves the old peer in place.
Clients should use a fresh random nonce for each handshake.

A token HELLO has a zero room length followed by the 16-byte token id. Its JOIN
uses the same HMAC construction keyed by the 32-byte token secret, with the 16
raw token-id bytes in place of the room bytes (room length 16). At JOIN the
relay looks up a live token, takes the room from it and applies the same rules
for fingerprints, room capacity and failed JOINs. An unknown, expired or revoked
token, or a wrong token secret, receives REJECT reason 3. A token may be used
for any number of joins until it dies.

### Directory messages

A session record (`R` bytes, 50 plus its three blobs) is, in wire order: session
id:u64, key[16], flags:u32, max public:u16, max private:u16, open public:u16,
open private:u16, host peer id:u32, host virtual IPv4[4], properties length:u16,
properties, contexts length:u16, contexts, QoS length:u16, QoS. Properties,
contexts and QoS are opaque to the relay and capped at 384, 128 and 256 bytes,
so the largest record is 818 bytes, the largest PUBLISH 842 and a RESULTS page
with one largest record 839. Codecs reject a zero session id, open counts above
their maximums, oversized blobs, and short or trailing bytes. SEARCH requires
1-200 results; REMOVE, LOOKUP and TOKEN_REQUEST require a nonzero session id;
PUBLISH and REMOVE require a nonzero sequence.

Directory requests are accepted only from a live peer's observed endpoint with
its session token, exactly as DATA. They consume the peer's packet and byte
buckets and refresh its idle deadline. The relay replies to the requester only.

PUBLISH and REMOVE apply in sequence order per peer. The relay records the last
applied sequence and its STATUS. A repeat of that sequence receives the recorded
STATUS again, and an older sequence receives STATUS ok; neither is applied, so a
delayed retransmission cannot undo a later update. Sequences compare as 32-bit
serial numbers, so order survives wrap; the client skips zero after 4294967295.
A new association starts a new peer and sequence.

- PUBLISH inserts or replaces the record keyed by room and session id. The relay
  overwrites the host peer id and host virtual IPv4 with the sender's, so a
  client cannot choose a host identity. A session id held by another peer in the
  room returns STATUS conflict. Replacing a session moves it to the front of
  search order.
- REMOVE deletes the sender's session and returns STATUS ok, including when the
  session is absent. A session held by another peer returns conflict.
- SEARCH returns sessions in the requester's room whose flags contain every
  required flag and whose open public count is nonzero, excluding sessions the
  requester hosts, most recently published first, at most `max results`.
- LOOKUP returns zero or one session from the requester's room, including its
  own and full sessions.
- TOKEN_REQUEST from the session's host returns TOKEN_GRANT. Any other requester
  receives STATUS not found. A repeated request ID from the same host returns
  the same live token with its remaining lifetime, rounded up to whole seconds,
  without allocating another token or counting against the token limit.

RESULTS packs records in order into pages of at most 1,224 bytes; the client
reassembles pages 0 through page count minus 1. Zero results are one page with
no records. The snapshot is the relay's directory version, which changes
whenever any session is published, replaced or removed; equal snapshots for one
request identify identical result sets. STATUS values are 0 ok, 1 conflict, 2
limit and 3 not found.

Every codec rejects short or oversized datagrams, invalid magic, version, type,
field lengths, flags, and trailing bytes inconsistent with the declared length.
The maximum datagram is 1224 bytes and the maximum payload is 1200. HELLO is at
least 58 bytes, so it already exceeds CHALLENGE's 24 bytes without extra
padding. Invalid magic, unsupported versions and all malformed datagrams are
silently dropped and counted. Only well-formed protocol messages can cause
REJECT. Every handshake and DATA response, including REJECT, is suppressed if it
would exceed the incoming datagram's length. Directory and CONNECT replies are
exempt because they go only to authenticated peers.

## State and limits

At most 64 pending handshakes exist, with at most four per source IPv4, each
expiring five seconds after its first HELLO. New HELLOs exceeding either cap are
silently dropped and counted as rate limited. A room is created only on
successful JOIN. The relay rejects a mismatched fingerprint before creating a
peer. Peer identity is the observed UDP source endpoint and its 64-bit session
token; changing either requires a new session.

Virtual addresses are allocated from `198.18.0.0/15`, beginning at `198.18.0.1`,
with the lowest free address reused after leave. They are unique within a room
and may repeat in separate rooms. They never use `10.0.0.0/8` or
`192.168.0.0/16`. A peer id is nonzero and unique among active peers. The server
generates nonces and nonzero session tokens using HMAC-SHA256 with a fresh
32-byte OS random seed; the core receives that seed explicitly.

Each peer starts with a one-second token-bucket burst of 500 packets and 256
KiB. DATA and HEARTBEAT consume one packet and their full wire byte size. Both
buckets refill continuously at those per-second rates. Excess traffic is
silently dropped and counted. Authenticated BYE bypasses the buckets and frees
the peer immediately. A peer expires after 15 seconds without an accepted DATA
or HEARTBEAT. Malformed, spoofed, rate-limited and unroutable traffic does not
refresh that deadline. A broadcast to an empty room counts as valid. The last
peer's departure removes the room and its fingerprint.

Directory limits are fixed. A peer hosts at most 2 sessions and a room holds at
most 64; a PUBLISH that would exceed either returns STATUS limit. Each peer may
send 4 SEARCH or LOOKUP and 10 PUBLISH, REMOVE or TOKEN_REQUEST per second, as
token buckets with that burst; excess requests are dropped silently and counted.
A session has at most 4 live tokens; a further TOKEN_REQUEST returns STATUS
limit. Since a room holds at most 64 sessions, a search returns at most 64
records and 64 pages.

A session ends on REMOVE, when its host peer is removed (BYE, idle expiry or a
replacing JOIN from the same endpoint), and when its room empties. A token holds
16 random bytes of id and a 32-byte secret from the relay's seeded random source
and is bound to one room and session. It dies at the earlier of its expiry, 15
minutes after the grant by default (`Config` `token_lifetime_seconds`, reported
in TOKEN_GRANT), and its session's end. The relay never sends the room secret.
Token secrets in messages and the token table use a type that wipes every copy
when destroyed and wipes the source of a move, so a secret is wiped when its
token dies. The relay also wipes each encoded TOKEN_GRANT after sending it.

The core uses injected monotonic milliseconds, clamps backward clock inputs, has
no sockets, and returns at most 64 datagrams per input. Its internal headers
live under `src/core` and are not installed SDK headers. The UDP transport uses
Winsock on Windows and POSIX sockets on Unix. The same `Pump` iteration serves
the CLI and the two-client loopback test. The server core and transport belong
to the internal `rexrelay_server` static library, linked only by the CLI and
tests. The kernel client uses its own sockets and links the protocol codec, HMAC
and OS random-seed helper from `rexcore`, without the server implementation.
Socket users declare `ws2_32` on Windows.
