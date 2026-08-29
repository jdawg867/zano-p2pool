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
- [ ] TCP listener/client transport
- [ ] bounded stream framing
- [ ] share gossip
- [ ] missing-parent synchronization
- [ ] best-tip sync hints
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
Normal and exact-Zano CI are green on the checkpoint code head; `p2p_protocol_test`
brings the suite to 14 tests.

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
