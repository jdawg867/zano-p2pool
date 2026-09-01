# zano-p2pool roadmap

## Phase 1 — daemon foundation

- [x] CMake/C++20 project bootstrap
- [x] Zano JSON-RPC client
- [x] `getblocktemplate` request
- [x] template response parser
- [x] `submitblock` request
- [x] parser unit test
- [x] live integration test against a synced Zano **testnet** `zanod`
- [x] sanitized live testnet metadata fixture and regression test
- [ ] robust RPC error/status handling
- [ ] daemon reconnect/backoff

## Phase 2 — ProgPoWZ verification

- [x] identify canonical Zano ProgPoWZ hashing entry points
- [x] integrate an exact-Zano ProgPoWZ verifier backend
- [x] calculate network target from Zano difficulty
- [x] derive the canonical mining header from an RPC block template
- [x] verify nonce/result against a block template locally
- [x] distinguish share target vs. full network target
- [x] pin deterministic exact-Zano PoW compatibility vectors

## Phase 3 — local share chain

- [x] define versioned share serialization
- [x] share IDs and parent linkage
- [x] share timestamp rules
- [x] cumulative work
- [x] stale/orphan handling
- [x] local persistence
- [x] deterministic best-tip/reorg rules
- [x] share-chain structural tests
- [x] gate share-chain admission on local ProgPoWZ verification

Milestone 0.3 is complete and merged to `main`. Production-facing
`ShareChain::submit_share()` binds locally trusted Zano work context and performs
exact ProgPoWZ verification before claimed share difficulty contributes chain work.

## Phase 4 — Stratum

- [x] Zano-compatible JSON-RPC request/response codec
- [x] deterministic worker/session state
- [x] versioned local work registry
- [x] per-session difficulty and target generation
- [x] current/stale/unknown work-header classification
- [x] route `eth_submitWork` into verified share-chain admission
- [x] duplicate `(job_version, nonce)` rejection
- [x] distinguish accepted share vs. full-network block candidate
- [x] Stratum TCP listener
- [x] bind to loopback by default with configurable port
- [x] local socket integration test
- [x] wire live daemon-derived templates into executable Stratum mode
- [x] test with ProgPoWZ-capable miners

Milestone 0.4 is complete and merged to `main`. It was validated with a local
exact-Zano Release build passing all 13 tests, live Zano testnet template refresh,
and a real SRBMiner-MULTI `progpow_zano` session submitting repeatedly accepted
shares through the verified Stratum path.

## Phase 5 — P2P network

- [x] versioned binary message envelope
- [x] handshake/version/network identity codec
- [x] capability bits and public node identifier semantics
- [x] wrong-network / unsupported-version rejection
- [x] TCP listener/client transport
- [x] bounded stream framing
- [x] share gossip
- [x] locally trusted-context gate for peer shares
- [x] duplicate peer-share suppression
- [x] missing-parent request/response synchronization
- [x] exact-Zano orphan promotion after parent synchronization
- [x] best-tip handshake and `TipAnnounce` sync hints
- [x] shared/reconstructable mining-context synchronization
- [x] outbound reconnect/backoff
- [x] peer scoring/bans
- [x] consensus/reorg integration tests
- [x] protocol fuzzing
- [x] two-node live testnet validation

Checkpoint 1 pins the P2P v1 envelope and handshake byte-for-byte. Frames use a
12-byte big-endian header (`ZP2P`, protocol version, message type, reserved flags,
payload length), enforce a 64 KiB payload cap, and fail closed on malformed,
truncated, trailing, unsupported-version/type/flag data. The handshake carries
network, non-zero public 32-byte node ID, capability bits, advertised listen port,
and a best-share sync hint. Best-share hints are not trusted consensus data.

Checkpoint 2 adds configurable TCP listener/client transport, bounded stream
framing, bidirectional socket handshake validation, and loopback integration tests.

Checkpoint 3 adds canonical `ShareAnnounce` gossip using the existing 165-byte
`Share` serialization. Peer shares are admitted only when their mining context is
already trusted locally, and exact-Zano CI rehashes them before they can affect the
share chain.

Checkpoint 4 adds bounded `ShareRequest` / `ShareResponse` synchronization by
exact `ShareId`. Found responses bind the requested ID to the returned canonical
share; not-found responses are explicit. In exact-Zano CI, receiving a valid child
first creates a verified orphan, requesting/receiving its parent locally rehashes
the parent, and the child is deterministically promoted. `p2p_sync_test` brings the
suite to 17 tests, with local exact-Zano confirmation complete.

Checkpoint 5 adds deterministic best-tip synchronization hints to both the initial
handshake and later 40-byte `TipAnnounce` messages (`ShareId + height`). No peer
cumulative-work value is transmitted or trusted. Unknown tip IDs are fetched by
exact ID; known tips with inconsistent advertised heights are flagged. If a known
tip is already an orphan, the planner requests its missing parent. Local verified
cumulative work remains the only best-tip selection input. Normal and exact-Zano
CI are green and local exact-Zano confirmation is complete.

Checkpoint 6B.5 closes the runtime mining-context trust path. A connected peer can
announce an independently anchored HF6 mining context; the receiving node verifies
its payout policy plus balance/range proofs, promotes only the locally derived
mining header into `P2pTrustedWorkRegistry`, and can then locally rehash and admit a
share mined against that foreign header. `p2p_mining_context_runtime_test` exercises
this over real loopback P2P sockets. On 2026-08-30 the exact-Zano Release suite passed
31/31 locally, and branch CI passed both the normal `build-and-test` job and the
exact-Zano `progpowz-compat` job.

Checkpoint 7 adds managed outbound peer reconnect with bounded exponential backoff.
Configured outbound endpoints remain registered after an initial dial failure or a
later socket disconnect, while live duplicate node IDs remain rejected by the
existing admission path. `p2p_runtime_test` now covers both delayed peer startup and
reconnection after an established peer disappears and returns, including successful
message exchange after recovery. On 2026-08-30 the exact-Zano Release suite passed
31/31 locally in 3.04 seconds, and branch CI passed both `build-and-test` and
`progpowz-compat`.

Checkpoint 8 adds deterministic peer scoring and temporary bans keyed by validated
public node ID. Explicit protocol-level violations accumulate toward a configurable
threshold; reaching it disconnects that identity immediately and suppresses managed
outbound reconnects until the ban expires, while ordinary socket disconnects remain
neutral. Automatic scoring is applied only to explicit, unambiguous protocol abuse:
unexpected in-session handshakes, capability misuse, inconsistent tip heights,
rejected proof-bearing mining contexts, and similarly typed invalid peer data.
Generic parser/validation exceptions remain reputation-neutral until attributable
failures are typed explicitly. Duplicate shares, unknown work contexts, and
asynchronous mining-context anchor mismatches remain neutral. `p2p_runtime_test`
covers threshold, disconnect, ban-window reconnect gating, expiry and recovery;
`p2p_node_test` covers automatic protocol classification and scoring. On 2026-08-30
the exact-Zano Release suite passed 31/31 locally in 3.22 seconds, and branch CI
passed both `build-and-test` and `progpowz-compat`.

Checkpoint 9 adds a networked consensus/reorg convergence test over real P2P
sockets. Two collectors receive the same competing share branches in opposite
orders from separate providers. One collector first adopts branch A, then receives
a weaker prefix of branch B, and finally performs a real reorg when branch B gains
strictly greater locally verified cumulative work. The other collector sees the
winning branch first and later receives the losing branch. Both converge on the
same best tip independent of arrival order, while all received shares cross the
normal P2P protocol and exact ProgPoWZ admission path in exact-Zano builds.
`p2p_consensus_reorg_test` brings the suite to 32 tests. On 2026-08-30 the exact-Zano
Release suite passed 32/32 locally in 3.69 seconds, and branch CI passed both
`build-and-test` and `progpowz-compat`.

Checkpoint 10 adds deterministic protocol fuzzing across every peer-controlled P2P
message parser. `p2p_protocol_fuzz_test` starts from canonical handshake, share
announce, share request/response, tip announce and mining-context frames; it
systematically truncates, appends and bit-flips every seed, attacks payload-length
boundaries, directly feeds thousands of arbitrary payloads into each semantic
parser, and runs 20,000 additional structured mutations derived from valid frames.
Successful parses must reserialize byte-for-byte canonically; malformed inputs may
fail closed through the parser's expected exceptions but must not crash, hang or
bypass the 64 KiB payload bound. The test brings the suite to 33 tests. On
2026-08-30 the exact-Zano Release suite passed 33/33 locally in 3.76 seconds, and
branch CI passed both `build-and-test` and `progpowz-compat`. With this checkpoint,
all Phase 5 P2P-network roadmap items are complete.

### Live two-node P2P validation

On 2026-08-30 two independent `zano-p2pool` processes were run against the same
synchronized Zano testnet daemon while each fetched and refreshed its own block
template. Node B connected to Node A over the P2P runtime, both sides repeatedly
accepted independently verified peer mining contexts, and temporary
`anchor-mismatch` results during asynchronous daemon-height refreshes recovered on
later matching anchors without weakening the trust gate.

SRBMiner-MULTI 3.6.0 then mined through Node A's `progpow_zano` Stratum endpoint.
Real GPU shares were accepted by Node A and relayed across the P2P connection. On
clean Node B shutdown, the runtime reported:

- `Stratum stopped. Verified shares in memory: 1737`
- `P2P stopped. Connected shares in memory: 1737`

This validates the live two-node path:

`SRBMiner -> Node A Stratum -> exact ProgPoWZ validation -> Node A share chain -> P2P ShareAnnounce -> Node B trusted foreign mining context -> Node B exact ProgPoWZ revalidation -> Node B connected share chain`.

### Live block-submission validation

On 2026-08-30 the current `feature/p2p-foundation` implementation was validated
end-to-end against a synchronized Zano testnet daemon:

- local exact-Zano Release build passed 30/30 tests;
- SRBMiner-MULTI connected over the local `progpow_zano` Stratum endpoint;
- real GPU shares were repeatedly accepted by the verified P2Pool share path;
- a full-network-difficulty candidate was found at Zano height 167413;
- the pool reconstructed the canonical HF6 miner transaction and full block;
- `zanod` accepted the block through JSON-RPC `submitblock`;
- the pool immediately refreshed and published the height-167414 Stratum template;
- the daemon remained synchronized and subsequently advanced beyond height 167422.

This validates the complete single-node path:

`SRBMiner -> Stratum -> ProgPoWZ verification -> share chain -> block candidate -> canonical block reconstruction -> zanod submitblock`.

Independent `zanod getblocktemplate` calls can produce different mining headers at
the same Zano height. The protocol/runtime now has an independently verified
mining-context exchange path for those foreign headers, and the live two-node run
has confirmed real share propagation across that trust boundary.

## Phase 6 — PPLNS and payouts

- [x] deterministic PPLNS window
- [x] work-weighted payout accounting
- [x] audit Zano miner-transaction consensus rules
- [x] determine feasibility of direct multi-recipient coinbase payouts
- [x] temporary custodial payout mode not required; direct non-custodial payouts are feasible
- [x] non-custodial payout design if consensus permits

Checkpoint 1 records the already-implemented deterministic PPLNS accounting path.
`build_pplns_window()` walks only the locally selected best chain newest-to-oldest,
credits exactly the requested work, and clips the oldest included share when the
window boundary falls inside that share. Repeated shares aggregate by `MinerId`,
and v2 public payout keys remain bound to the credited identity. `allocate_pplns_reward()`
uses exact integer arithmetic with deterministic largest-remainder apportionment,
lexicographic `MinerId` tie-breaking, and an exact reward-sum invariant. The Phase 6
regression added on `feature/pplns-window-accounting` additionally proves that after
a share-chain reorg, stale-branch work is excluded from the payout window and the
new best branch alone determines credited work.

Checkpoint 2 records the pinned-Zano HF6 miner-transaction audit and direct payout
feasibility result. The audited Zano source fixes current miner transactions at
transaction version 4 / hardfork 6, enforces a hard 32-output transaction limit
that matches the Bulletproof+ aggregation maximum, and defines a 125 kB full-reward
zone. The exact-Zano adapter passes the verified PPLNS destinations directly into
Zano's canonical `construct_miner_tx()` implementation, refuses plans above the
32-output limit, binds the generated reward back to the daemon template, and uses
Zano's own balance/range-proof machinery. `zano_miner_tx_test` constructs and parses
a real two-recipient HF6 miner transaction in exact-Zano builds, while the P2P
mining-context proof tests independently verify the resulting proof-bearing miner
transaction before trust promotion. This confirms direct multi-recipient,
non-custodial coinbase payouts are feasible for the current pinned HF6 rules, so a
temporary custodial fallback is intentionally not required. On 2026-08-30 the local
exact-Zano Release suite passed 33/33 in 3.76 seconds, and branch CI #409 passed both
`build-and-test` and `progpowz-compat`.

## Phase 7 — public network hardening

- [x] mainnet-compatible sidechain parameters
- [ ] seed nodes
- [x] observability/metrics
- [ ] rate limits
- [x] persistence recovery
- [ ] adversarial tests
- [ ] release builds
- [ ] protocol specification

Checkpoint 1 establishes canonical sidechain identity. `SidechainParameters` has a
deterministic domain-separated serialization and 32-byte `SidechainId`; P2P protocol
v2 carries that ID in every handshake and rejects peers whose parent Zano network
matches but whose P2Pool consensus profile differs. Runtime handshake normalization
ensures the stored local identity exactly matches the wire identity rather than
silently retaining an all-zero placeholder.

Checkpoint 2 completes the current mainnet-compatible economic sidechain profile.
Both mainnet and testnet profiles commit to a 10-second target share interval, a
100,000,000 minimum share difficulty, a 2160-share difficulty-estimation history,
a 32-share direct-payout PPLNS history cap, and a PPLNS work cap of twice the current
Zano network difficulty. The next-share difficulty is derived from the selected
parent branch's own cumulative-work/timestamp history, trims the oldest/newest 10%
by timestamp before estimating work rate, floors at the configured minimum, and is
capped at current parent-network difficulty. Configured `ShareChain` admission
rejects a share whose claimed difficulty differs from that branch-relative result,
including orphan promotion, while Stratum publishes the same consensus target so
local miners cannot drift from the sidechain rule.

The policy-level PPLNS builder deterministically uses the lesser of the work present
in the newest 32 best-chain shares and twice current Zano network difficulty. This
keeps bootstrap payout windows complete once verified history exists, preserves
partial-oldest-share accounting, follows reorg-selected best-chain history, and
guarantees no more than 32 credited share identities can enter the direct HF6
coinbase plan. On 2026-08-30 the local exact-Zano Release suite passed 33/33 in 3.76
seconds, and CI #449 passed both `build-and-test` and `progpowz-compat`.

Checkpoint 3 completes live multi-recipient PPLNS template replacement and direct
HF6 payout submission. Canonical payout-capable share v2 is enforced by sidechain
identity v3, P2P mining-context trust verifies complete deterministic payout plans,
and the runtime atomically publishes rebuilt PPLNS work to Stratum, P2P trust and
block submission. Live Zano testnet validation produced two-recipient PPLNS work and
successful `submitblock` results at heights 169473 and 169474. The final exact-Zano
suite passed 33/33 locally in 4.14 seconds, PR #21 merged to `main`, and post-merge CI
#487 passed both `build-and-test` and `progpowz-compat`.

Checkpoint 4 adds restart-safe sidechain persistence and recovery. `ShareStore` is
an append-only file bound to the canonical `SidechainId`; each record stores the
canonical serialized share plus its `ShareId`, rejects wrong-sidechain stores and
full-record corruption, restores orphan/parent ordering deterministically, and can
truncate an interrupted final append back to the last complete record. The runtime
persists locally accepted and P2P-admitted shares, defaults to
`~/.zano-p2pool/<network>/shares.dat`, supports `--share-store PATH` and
`--no-share-store`, and shuts down on append failure rather than silently losing
durable consensus history.

Live Zano testnet restart validation persisted 15 connected shares, stopped the
node cleanly, then restarted without mining. Recovery reported `records=15`,
`connected=15`, `orphans=0`; mining-context trust was immediately `ready`, payout
mode was immediately `canonical PPLNS`, and canonical PPLNS block submission was
enabled without bootstrap re-establishment. The final exact-Zano regression suite
passed 33/33 locally in 4.14 seconds on 2026-08-31.

Checkpoint 5 adds opt-in runtime observability. A bounded HTTP server defaults to
loopback and exposes Prometheus-style `GET /metrics` plus `GET /healthz`. The metric
surface is intentionally label-free and excludes wallet addresses, node IDs, share
IDs, peer identities, and other payout-identifying values. It reports parent-chain
height, connected/orphan share counts and tip height, connected P2P and Stratum
peers, trusted work contexts, a process-local Stratum job sequence, accepted/admitted
share counters, block-candidate and submission outcomes, template-refresh failures,
and persistence health. Runtime logs use `Stratum job #N`, and the exported gauge is
`zano_p2pool_stratum_job_sequence`, avoiding ambiguity with node, P2P, or canonical
share protocol versions.

Live Zano testnet validation started from the previously persisted 15-share chain,
reported a healthy `/healthz`, and served correct idle metrics before mining. With
SRBMiner connected, the same endpoint tracked growth to 77 connected shares and tip
height 76, one Stratum connection, 62 accepted shares, 62 full-difficulty block
candidates, 17 successful `submitblock` events, two non-success block-submission
events, and healthy persistence throughout. The final exact-Zano regression suite
passed 34/34 locally in 4.17 seconds on 2026-09-01.
