# zano-p2pool P2P protocol specification

This document specifies the public `ZP2P` wire protocol implemented by
zano-p2pool. It describes protocol version 2 as of September 2026. The C++
serializers and parsers remain the normative implementation; changes to the
wire format require a protocol-version change and corresponding test vectors.

## Conventions

- Transport is a persistent TCP byte stream.
- All unsigned integers are big-endian. There are no wire varints outside the
  embedded Zano block-template blob.
- Byte offsets are zero-based and relative to the start of the structure being
  described.
- `Hash256`, `ShareId`, `MinerId`, `NodeId`, and `SidechainId` are opaque
  32-byte strings on the wire. Hash bytes are transmitted in the order returned
  by the implementation; peers must not reverse them.
- `Difficulty128` is an unsigned 128-bit integer encoded as 16 big-endian bytes.
- Sizes and offsets in this document are decimal unless prefixed with `0x`.

## Connection and handshake

The dialing peer sends one Handshake frame immediately after TCP connection.
The accepting peer validates it, sends its own Handshake frame, and then both
peers may exchange other messages. A Handshake received after this exchange is
a protocol violation.

A peer accepts a handshake only when the parent network and sidechain ID match
its local configuration and the remote node ID differs from its own. Node IDs
are public, non-zero identifiers. Duplicate live connections carrying the same
node ID are rejected by the runtime.

## Envelope

Every message is one envelope with a 12-byte header followed by exactly the
declared payload.

| Offset | Size | Field | Required value |
|---:|---:|---|---|
| 0 | 4 | magic | ASCII `ZP2P` (`5a 50 32 50`) |
| 4 | 1 | protocol version | `2` |
| 5 | 1 | message type | one of the values below |
| 6 | 2 | flags | `0`; no flags are currently defined |
| 8 | 4 | payload length | `0..65536` |
| 12 | variable | payload | exactly `payload length` bytes |

| Type | Name | Capability used |
|---:|---|---|
| 1 | Handshake | none |
| 2 | ShareAnnounce | share gossip, bit 0 |
| 3 | ShareRequest | share sync, bit 1 |
| 4 | ShareResponse | share sync, bit 1 |
| 5 | TipAnnounce | share sync, bit 1 |
| 6 | MiningContextAnnounce | mining context, bit 2 |

Unknown versions, types, or flags are rejected. A frame shorter or longer than
its declared size is rejected. The stream receiver checks the header before
allocating the payload and never accepts a payload above 64 KiB.

## Handshake payload (type 1)

The payload is exactly 115 bytes.

| Offset | Size | Field | Rules |
|---:|---:|---|---|
| 0 | 1 | network | `1` testnet, `2` mainnet |
| 1 | 32 | sidechain ID | non-zero and equal to the receiver's configured ID |
| 33 | 32 | node ID | non-zero and different from the receiver's node ID |
| 65 | 8 | capabilities | bit mask; current bits are listed below |
| 73 | 2 | listen port | advertised TCP port; `0` means none |
| 75 | 32 | best share ID | synchronization hint; all zero means no tip |
| 107 | 8 | best share height | must be `0` when best share ID is zero |

Capability bits are `0x1` for share gossip, `0x2` for share synchronization,
and `0x4` for mining-context exchange. Current nodes advertise `0x7`. Unknown
capability bits do not make the handshake malformed, but grant no behavior.

The canonical sidechain ID is `cn_fast_hash` of the canonical sidechain
parameter encoding. Current IDs are:

| Parent network | Sidechain ID (hex) |
|---|---|
| testnet | `4ac0cdc2a86e9617f18035428e103017d545c4ad7ebfb74d283e60256396e8e8` |
| mainnet | `8cfa674d782bfc0a3824d654617c814da98ea2ec64b5701a41ddd40a522225bf` |

The parameter encoding uses the eight-byte domain `ZP2SIDV3`, followed by the
parent-network byte, minimum and maximum share-version bytes, then seven
big-endian `uint64` consensus parameters. See `sidechain_params.hpp` for the
canonical values and encoding.

## Canonical share encoding

ShareAnnounce carries one canonical share directly. A found ShareResponse
contains the same encoding after its response header. `ShareId` is
`cn_fast_hash(canonical_share_bytes)`.

| Offset | Size | Field | Rules |
|---:|---:|---|---|
| 0 | 4 | magic | ASCII `ZP2S` |
| 4 | 1 | share version | `1` or `2` |
| 5 | 32 | parent share ID | all zero only for a root share |
| 37 | 8 | share height | unsigned |
| 45 | 8 | timestamp | Unix seconds |
| 53 | 8 | Zano height | parent-chain template height |
| 61 | 32 | mining-header hash | trusted-work lookup key with Zano height |
| 93 | 8 | nonce | ProgPoWZ nonce |
| 101 | 16 | share difficulty | non-zero unsigned 128-bit integer |
| 117 | 16 | network difficulty | non-zero unsigned 128-bit integer |
| 133 | 32 | miner ID | public payout identity |
| 165 | 32 | spend public key | share v2 only |
| 197 | 32 | view public key | share v2 only |

Share v1 is exactly 165 bytes and carries no payout keys. Share v2 is exactly
229 bytes and requires both public keys. For v2, `miner_id` must equal
`cn_fast_hash(5a 50 32 50 00 || spend_public_key || view_public_key)`. The canonical
public sidechain profile currently admits only v2 shares; v1 remains a parsing
compatibility format.

Receipt of a well-formed share does not establish trust. The receiver requires
a locally verified work context matching `(zano_height, mining_header_hash)`,
then independently applies share timing, parent, difficulty, ProgPoWZ, chain,
and payout-capable-version rules. Unknown-work shares are not admitted.

## ShareAnnounce payload (type 2)

The entire payload is one 165-byte v1 or 229-byte v2 canonical share. The peer
must advertise capability bit 0. Connected shares are relayed to other peers;
an orphan causes a ShareRequest for its missing parent.

## ShareRequest payload (type 3)

The payload is exactly one non-zero 32-byte ShareId. The peer must advertise
capability bit 1. A node answers only from shares connected to its local chain.

## ShareResponse payload (type 4)

| Offset | Size | Field | Rules |
|---:|---:|---|---|
| 0 | 1 | result | `1` found, `2` not found |
| 1 | 32 | requested share ID | non-zero |
| 33 | variable | share | present only for result `1` |

A not-found response is exactly 33 bytes. A found response is 198 bytes for a
v1 share or 262 bytes for v2. The hash of the embedded canonical share must
equal `requested share ID`; mismatches are rejected. Returned shares pass the
same independent admission path as announced shares.

## TipAnnounce payload (type 5)

The payload is exactly 40 bytes: a 32-byte ShareId followed by an eight-byte
share height. A zero ID requires height zero and means the sender has no tip.
The peer must advertise capability bit 1.

Tip data is only a synchronization hint. An unknown ID is requested and
validated locally. A claimed height never contributes cumulative work or
selects the local best tip; a height that conflicts with an already known share
is a protocol violation.

## MiningContextAnnounce payload (type 6)

The payload contains a peer proposal for an exact Zano mining context. Its
fixed portion is 121 bytes.

| Offset | Size | Field | Rules |
|---:|---:|---|---|
| 0 | 1 | mining-context version | `1` |
| 1 | 8 | Zano height | unsigned |
| 9 | 32 | previous block hash | locally anchored |
| 41 | 16 | network difficulty | non-zero |
| 57 | 32 | ProgPoWZ seed | locally anchored |
| 89 | 8 | block reward without fee | locally anchored |
| 97 | 8 | block reward | payout-policy input |
| 105 | 8 | transaction fees | payout-policy input |
| 113 | 4 | block-template blob length | unsigned |
| 117 | 4 | `miner_tx_tgc` JSON length | unsigned |
| 121 | variable | block-template blob | exact declared length, non-empty |
| after blob | variable | `miner_tx_tgc` JSON | exact declared length, valid object |

The two declared variable lengths must consume the payload exactly, and the
whole payload must not exceed 64 KiB. The JSON must contain a non-empty,
even-length hexadecimal string field named `tx_key`. The proposal ID is
`cn_fast_hash(canonical_mining_context_payload)`.

The peer must advertise capability bit 2. This message is an untrusted proposal,
not a trust transfer. Promotion into the trusted-work registry occurs only after
all of these checks succeed:

1. The height, previous hash, network difficulty, seed, and reward-without-fee
   match a locally synchronized `zanod` template.
2. The embedded current-HF6 block and miner-transaction structure is canonical.
3. The miner transaction is cryptographically bound to `miner_tx_tgc`.
4. Destinations and committed amounts match the locally derived PPLNS payout
   plan and native-asset reward policy.
5. Zano's HF6 balance proof and Bulletproof+ range/aggregation proofs verify.

Nodes defer mining-context messages until the corresponding local anchor and
payout expectation are available.

## Error handling, limits, and reputation

Parsing is fail closed. Non-canonical sizes, trailing bytes, unsupported enum
values, zero IDs where forbidden, component-length mismatches, and invalid
embedded encodings cause the connection's receive loop to end. Canonical
messages round-trip byte-for-byte through every parser and serializer; the test
suite systematically truncates, extends, mutates, and fuzzes all peer-controlled
message payloads.

The runtime defaults to 64 simultaneous peers and an inbound token bucket of
512 messages with a 256-message-per-second refill. The first excess message
disconnects the peer without a reputation penalty.

Explicit semantic violations use a 25-point penalty. At the default threshold
of 100 points, the public node ID is disconnected and banned for five minutes;
the score resets when the ban expires. Capability abuse, conflicting tip
heights, rejected mining-context proofs, and a second Handshake are penalized.
Ordinary socket failures and shares referencing unknown work contexts are not.

## Compatibility requirements

- Protocol v2 has no negotiated downgrade or extension flags.
- Implementations must send canonical encodings and exact lengths.
- Receivers must enforce the 64 KiB envelope limit before payload allocation.
- A future incompatible framing or message change must use another protocol
  version. Consensus parameter changes must use a new sidechain parameter
  version/domain and therefore a new SidechainId.
- Interoperability requires more than decoding: every share and mining context
  must pass local consensus and cryptographic verification before affecting
  chain selection or trusted work.

## Normative implementation map

| Area | Implementation | Regression coverage |
|---|---|---|
| envelope and handshake | `p2p_protocol.cpp` | `p2p_protocol_test.cpp`, `p2p_protocol_fuzz_test.cpp` |
| TCP exchange | `p2p_transport.cpp` | `p2p_transport_test.cpp` |
| shares and gossip | `share.cpp`, `p2p_share.cpp` | `p2p_share_test.cpp` |
| synchronization | `p2p_sync.cpp`, `p2p_tip.cpp` | `p2p_sync_test.cpp`, `p2p_tip_test.cpp` |
| mining context | `p2p_mining_context.cpp` | `p2p_mining_context_test.cpp` |
| trust promotion | `p2p_mining_context_trust.cpp` | `p2p_mining_context_trust_test.cpp` |
| runtime limits | `p2p_runtime.cpp`, `p2p_peer_score.hpp` | `p2p_runtime_test.cpp`, `p2p_node_test.cpp` |
