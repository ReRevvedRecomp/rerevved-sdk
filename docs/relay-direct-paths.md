# Direct relay paths

Direct paths are optional and require the [relay client](relay-client.md). Read
the [relay security limits](SYSTEM_LINK_RELAY.md) and this page's [threat
model](#threat-model) before enabling them.

With `system_link_direct` set in relay mode, two members of a room that exchange
unicast guest traffic try a direct UDP path and move that pair's unicast
datagrams onto it while it works. Guest virtual addresses and ports never
change, and a direct datagram is injected exactly as a DELIVER, with the same
envelope and the virtual source restored, so the guest cannot tell which path
carried it. Broadcast and round-trip probes on guest port 0 always use the
relay. Without the option, against a relay without direct-path message types, or
with a peer that has the option off, every datagram uses the relay.

## Flow

1. After WELCOME the client sends CANDIDATES three times, two seconds apart: up
   to four local IPv4 endpoints with the relay socket's port: only `127.0.0.1`
   when the relay endpoint is loopback, otherwise the host's interface addresses
   that are not loopback, link-local (`169.254.0.0/16`), multicast or in the
   virtual range. The relay stores the newest list per peer beside the peer's
   observed endpoint. CANDIDATES or CONNECT_REQUEST marks a peer as taking part
   in direct paths.
2. The first unicast guest datagram sent to or received from another member (not
   broadcast, not guest port 0) sends CONNECT_REQUEST for that member's virtual
   address. The client repeats it every second and gives up after three seconds
   without CONNECT_INFO. Each peer has at most one outstanding attempt.
3. When both peers are live members of the same room that take part in direct
   paths, the relay sends CONNECT_INFO to both; otherwise it drops the request
   and counts `connect_unavailable`. Requests from either peer while the pair's
   attempt is under 10 seconds old repeat that attempt's id and key; later
   requests create a new attempt with a new id and key. Attempt ids increase. A
   client keeps only the newest attempt per peer, ignores a repeat of its
   current attempt, and drops direct frames for any other attempt. For the life
   of the association it remembers the highest attempt id accepted from each
   peer, even after CONNECT_CLOSE or eviction, and drops a CONNECT_INFO at or
   below it as a stale attempt, so a captured CONNECT_INFO cannot revive a
   closed attempt and its old frames. An authenticated CONNECT_CLOSE raises that
   id to at least the closed attempt even when the client has no entry for the
   peer, so a CONNECT_INFO delayed past its CONNECT_CLOSE is dropped too. Past
   1,024 remembered peers the lowest remembered id becomes a floor for every
   peer.
4. Each side sends PROBE to every candidate of the other, the observed endpoint
   and the local ones, every 200 ms for five seconds, from the socket it uses
   for the relay so NAT mappings match the endpoint observed by the relay. A
   PROBE_ACK for one of its probes, received inside that attempt's probing
   window, establishes its send path to the acknowledgement's source endpoint;
   the path counts as local when that source is one of the peer's local
   candidates. Probes still outstanding when the window ends no longer count. A
   local candidate is preferred: a path established through the observed
   endpoint keeps probing the local candidates until the window ends and moves
   to the first that answers.
5. Each side decides its own send path. A receiver accepts authenticated DATA
   for the current attempt even while its own sends use the relay, so one-way
   paths work.

## Keys

Labels are ASCII without a terminator; integers are little-endian and IPv4
fields are network-order octets, as on the wire.

- `K_assoc = HMAC-SHA256(join key, "RRLY-ASSOC-v1" || client nonce[16] || server
  nonce[16] || peer id:u32)`. The join key is the JOIN HMAC key: the trimmed
  room secret, or the token secret for a token join. The peer id comes from
  WELCOME. Both sides derive it at JOIN and it never goes on the wire. The
  client keeps the secret in its wiped storage until WELCOME, derives `K_assoc`,
  then wipes the secret; the relay keeps `K_assoc` per peer in wiped storage.
- The relay draws a 32-byte pair key per attempt from its seeded random source
  and sends it to each member wrapped under that member's `K_assoc`: `ct = pair
  key XOR HMAC(K_assoc, "RRLY-PAIR-v1" || attempt id:u32 || other virtual
  IPv4[4])` and `tag = HMAC(K_assoc, "RRLY-PAIR-TAG-v1" || attempt id || other
  virtual IPv4 || observed IPv4[4] || observed port:u16 || count:u8 || count x
  (IPv4[4], port:u16) || ct)[0:16]`. The endpoint and candidate bytes are
  exactly as encoded in the CONNECT_INFO, so a relay-path attacker cannot
  redirect probes without failing the tag. A client drops a CONNECT_INFO whose
  tag does not verify.
- `K_dir = HMAC(pair key, "RRLY-DIRECT-v1" || sender virtual IPv4[4])` keys the
  frames each side sends.
- The CONNECT_CLOSE tag is `HMAC(K_assoc, "RRLY-CLOSE-v1" || attempt id || other
  virtual IPv4)[0:16]`. Only the relay can produce it, and relay attempt ids
  only increase, so the floor a CLOSE sets never blocks a later attempt.
- CANDIDATES and CONNECT_REQUEST end with `tag = HMAC(K_assoc, label || every
  encoded byte before the tag)[0:16]`, with label `"RRLY-CAND-v1"` or
  `"RRLY-CREQ-v1"`. The session token alone travels in plaintext, so the tag
  stops a relay-path attacker who spoofs a member's source from storing
  candidates it controls or starting attempts. The relay checks the tag in
  constant time after the packet and byte buckets and before the connect bucket,
  and counts failures as `connect_auth`.

## Direct frames

Direct frames travel between the two clients' relay sockets:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | ASCII `RRDT` |
| 4 | 1 | Version, 1 |
| 5 | 1 | Type: 1 PROBE, 2 PROBE_ACK, 3 DATA, 4 KEEPALIVE, 5 KEEPALIVE_ACK |
| 6 | 4 | Attempt id:u32 |
| 10 | 4 | Sender virtual IPv4 |
| 14 | 8 | Sequence:u64, nonzero |
| 22 | 8 | Control types: echo:u64, zero in PROBE and KEEPALIVE, the answered sequence in an acknowledgement |
| 22 | 4+P | DATA: source guest port:u16, destination guest port:u16, payload[P] |
| End - 16 | 16 | `HMAC(K_dir, all preceding bytes)[0:16]` |

Control frames are 46 bytes. DATA is 42+P bytes with P at most 1,200, the relay
DATA limit, so the largest frame is 1,242 bytes. Each side numbers its frames
from 1 within an attempt. A receiver checks the structure, a current attempt for
the sender virtual address, the attempt id, the MAC, and a 64-entry sliding
replay window per direction that drops duplicates and anything 64 or more
sequences behind the highest accepted one. Failures are dropped without a reply
and counted. An acknowledgement goes to the source of the frame it answers and
has the same size, so no reply is larger than its request. An authenticated
DATA, KEEPALIVE or KEEPALIVE_ACK from an endpoint that is not the current path
moves the path there (NAT rebinding). PROBE frames are answered but never move a
path.

## Timers and fallback

Each side judges only its own send path, by acknowledgement. While a path is up,
the side sends an authenticated KEEPALIVE every 500 ms whether or not data is
flowing, and the peer answers every authenticated KEEPALIVE with a same-size
KEEPALIVE_ACK, even while the peer's own sends use the relay. The send path goes
down when no authenticated KEEPALIVE_ACK, or PROBE_ACK for the current attempt,
has arrived for 2 seconds. Received direct DATA, KEEPALIVE and probes do not
keep it up, so a side whose sends are lost while it still hears its peer falls
back on its own. It also goes down on CONNECT_CLOSE, which the relay sends to
the remaining member when a paired peer leaves the room by BYE, idle expiry or
rejoin; when a newer attempt for the peer arrives; and with the relay
association, at loss, restart or the final `XNetCleanup`. Unicast then switches
back to the relay at once. There is no timer for a lack of received traffic:
receive state belongs to the attempt and lasts until the attempt is replaced or
closed, and a receiver keeps accepting authenticated DATA for the current
attempt while its own sends use the relay. The relay keeps delivering relayed
DATA throughout, so either path may carry a datagram at any time and the guest
sees ordinary UDP. Datagrams sent on a path that has stopped working are lost
for at most about 2 seconds before that side falls back; with loss in both
directions both sides fall back within that time.

A CONNECT_REQUEST timeout, an attempt without an answered probe and a path going
down each start a backoff: the next attempt waits 30 seconds after the first
failure, 60 after the second and 120 after each later one, and an established
path resets the count. Guest traffic triggers the retry once the backoff has
passed.

`system_link_direct_drop_after=N` drops every direct datagram received from N
seconds into a direct path until the association ends, which exercises mid-match
fallback during testing. The faulted side stops acknowledging, so both sides
fall back within about 2 seconds, and later attempts fail during probing because
their acknowledgements are dropped too.

## Status, logs and limits

Each path change logs one info line with the peer's virtual address and the
chosen endpoint kind, local or observed; endpoints and keys are never logged. A
client tracks at most 64 peers. When the table is full, a new peer evicts the
least recently used entry without a live path or probing attempt, idle entries
before unanswered requests, and counts `direct_evicted`. CANDIDATES and
CONNECT_REQUEST share a per-peer token bucket of four per second with that
burst, after the packet and byte buckets; excess requests count
`connect_rate_limited`. A peer stores at most four candidates, and the newest
list replaces the previous one. At most 4,096 attempts exist; a request for
another pair beyond that counts `connect_limit`. The relay removes a pair's
attempt when either peer leaves. The relay's periodic counter line includes the
live attempt count as `attempts` and the four connect drop reasons as
`connect_rate_limited`, `connect_unavailable`, `connect_limit` and
`connect_auth`.

## Threat model

Direct frames have integrity and replay protection against anyone who is neither
of the two peers nor the relay, including observers of relay traffic: the pair
key travels only wrapped under each member's `K_assoc`, which derives from the
join secret and is never sent. Payloads remain unencrypted. Room members share
the room secret, so a member who observes another member's handshake can derive
that member's `K_assoc`; room members are trusted. A token member's `K_assoc`
derives from the token secret, so anyone who observed its TOKEN_GRANT and
handshake can derive it, as they could already join with the token. The relay
chooses the pair key and can impersonate either peer, as it can for relayed
DATA. Local candidates reveal members' LAN addresses to the other members of the
room.
