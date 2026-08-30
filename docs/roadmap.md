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
- [ ] outbound reconnect/backoff
- [ ] peer scoring/bans
- [ ] consensus/reorg integration tests
- [ ] protocol fuzzing
- [ ] two-node live testnet validation

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

`SRBMMiner -> Stratum -> ProgPoWZ verification -> share chain -> block candidate -> canonical block reconstruction -> zanod submitblock`.

Independent `zanod getblocktemplate` calls can produce different mining headers at
the same Zano height. The protocol/runtime now has an independently verified
mining-context exchange path for those foreign headers; the remaining proof is a
live two-node testnet run with independently fetched daemon templates and real share
gossip across the P2P connection.

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
