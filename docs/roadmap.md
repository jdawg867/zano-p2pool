# zano-p2pool roadmap

## Phase 1 — daemon foundation

- [x] CMake/C++20 project bootstrap
- [x] Zano JSON-RPC client
- [x] `getblocktemplate` request
- [x] template response parser
- [x] `submitblock` request
- [x] parser unit test
- [ ] live integration test against a synced `zanod`
- [ ] robust RPC error/status handling
- [ ] daemon reconnect/backoff

## Phase 2 — ProgPoWZ verification

- [ ] identify canonical Zano ProgPoWZ hashing entry points/test vectors
- [ ] integrate or clean-room-wrap ProgPoWZ verifier
- [ ] calculate network target from Zano difficulty
- [ ] verify nonce/result against a block template
- [ ] distinguish share target vs. full network target
- [ ] deterministic PoW test vectors

## Phase 3 — local share chain

- [ ] define versioned share serialization
- [ ] share IDs and parent linkage
- [ ] share timestamp rules
- [ ] cumulative work
- [ ] stale/orphan handling
- [ ] local persistence
- [ ] reorg rules
- [ ] share-chain tests

## Phase 4 — Stratum

- [ ] Stratum TCP listener
- [ ] miner login / wallet parsing
- [ ] job generation
- [ ] per-miner difficulty
- [ ] share submission
- [ ] duplicate/stale share detection
- [ ] test with ProgPoWZ-capable miners

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
