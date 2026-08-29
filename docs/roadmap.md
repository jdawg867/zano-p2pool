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
- [ ] Stratum TCP listener
- [ ] bind to loopback by default with configurable port
- [ ] local socket integration test
- [ ] test with ProgPoWZ-capable miners

Checkpoint 2 was confirmed locally with an exact-Zano Release build passing 10/10 tests.
Checkpoint 3 is CI-green. Current work is resolved against the job actually issued
to the session, duplicate nonces are suppressed before expensive hashing, and
accepted shares/block candidates both pass through the same exact local ProgPoWZ
and `ShareChain::submit_share()` verification path. Ordinary shares are never sent
to `zanod`, and block candidates are surfaced without automatic submission.

## Phase 5 — P2P network

- [ ] peer handshake/versioning
- [ ] peer discovery/bootstrap peers
- [ ] share gossip
- [ ] missing-parent synchronization
- [ ] peer scoring/bans
- [ ] consensus/reorg tests
- [ ] protocol fuzzing

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
