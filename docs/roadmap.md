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
- [ ] local persistence
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
- [ ] protocol fuzzing
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
neutral. Automatic scoring is applied only to unambiguous protocol abuse: malformed
payload exceptions, unexpected in-session handshakes, capability misuse,
inconsistent tip heights, rejected proof-bearing mining contexts, and similarly
invalid peer data. Duplicate shares, unknown work contexts, and asynchronous
mining-context anchor mismatches remain neutral. `p2p_runtime_test` covers threshold,
disconnect, ban-window reconnect gating, expiry and recovery; `p2p_node_test` covers
automatic protocol classification and scoring. On 2026-08-30 the exact-Zano Release
suite passed 31/31 locally in 3.22 seconds, and branch CI passed both `build-and-test`
and `progpowz-compat`.

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

- [ ] deterministic PPLNS window
- [ ] work-weighted payout accounting
- [ ] audit Zano miner-transaction consensus rules
- [ ] determine feasibility of direct multi-recipient coinbase payouts
- [ ] temporary custodial payout mode only if clearly marked
- [ ] non-custodial payout design if consensus permits

## Phase 7 — public network hardening

- [ ] mainnet-compatible sidechain parameters
- [ ] seed nodes
- [ ] observability/metrics
- [ ] rate limits
- [ ] persistence recovery
- [ ] adversarial tests
- [ ] release builds
- [ ] protocol specification
