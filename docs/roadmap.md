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
- [x] deterministic best-tip/reorg rules
- [x] share-chain structural tests
- [x] gate production share-chain admission on local ProgPoWZ verification
- [x] bind verified admission to locally trusted Zano work context
- [x] record full-network block-candidate status on verified shares
- [ ] local persistence/recovery follow-up

Milestone 0.3 local consensus scope is complete: canonical shares, cumulative-work fork
choice, orphan/stale handling, timestamp policy, trusted-template binding, and exact
ProgPoWZ admission are covered by deterministic Release tests. Persistence remains a
separate follow-up after in-memory consensus behavior is stable.

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
