# Mainnet readiness audit

## Purpose

This document tracks the gates that must be completed before zano-p2pool is
operated as a public mainnet P2Pool. The current public beta deployment remains
testnet-only while this audit is open.

The audit is deliberately fail-closed: no permanent mainnet seed endpoints are
added and no mainnet mining or payout service is exposed until every required
mainnet assumption has either a pinned regression test or a documented live
validation result.

## Source-confirmed current state

The current runtime has explicit `testnet` and `mainnet` network modes. The
default daemon RPC endpoints are separated:

- testnet: `http://127.0.0.1:12111/json_rpc`
- mainnet: `http://127.0.0.1:11211/json_rpc`

Default share-store paths are also separated by network under
`~/.zano-p2pool/<network>/shares.dat`.

P2P maps the runtime network into distinct `P2pNetwork` values and derives a
canonical sidechain ID from the corresponding parent network. Mainnet and
testnet deliberately use different sidechain IDs even though their current
economic parameters otherwise match.

Built-in seed policy is asymmetric by design:

- testnet has two public DNS seeds:
  `zano-pool.ddns.net:37888` and `zano-pool2.ddns.net:37888`;
- mainnet has no built-in seeds yet.

The standard Zano payout-address decoder currently accepts the classic
non-auditable address format using base58 prefix `0xc5`. Its API is not
network-parameterized, so the mainnet/testnet address-prefix assumption must be
verified against the pinned Zano source before mainnet launch.

## Audit gates

### 1. Network selection and defaults

- [x] runtime exposes explicit `--network testnet|mainnet`
- [x] testnet and mainnet daemon RPC defaults are distinct
- [x] default share-store paths are network-separated
- [x] runtime maps the selected network into P2P and sidechain parent-network
      identity
- [ ] add or pin regression coverage proving mainnet runtime configuration
      selects the expected RPC, P2P network, sidechain ID, and persistence
      namespace without contacting a live daemon

### 2. Mainnet sidechain identity and consensus profile

- [x] mainnet and testnet canonical sidechain IDs are distinct
- [x] current sidechain economic parameters are pinned by deterministic
      serialization and ID vectors
- [x] P2P handshake validation rejects wrong-network and wrong-sidechain peers
- [ ] re-audit every consensus-relevant parameter against the intended mainnet
      launch profile
- [ ] record the exact mainnet sidechain ID and parameter encoding in the
      operator launch checklist

### 3. Wallet and payout address semantics

- [x] standard payout-address decoding is checksum- and prefix-validated
- [ ] verify from the pinned Zano source whether base58 prefix `0xc5` is valid
      for the intended mainnet payout address format and whether testnet uses the
      same or a different prefix
- [ ] add pinned mainnet address vectors from a non-secret public address
- [ ] if network address prefixes differ, make payout-address validation
      network-aware and reject cross-network addresses
- [ ] confirm unsupported integrated/auditable/subaddress formats remain
      intentionally fail-closed unless explicitly implemented

### 4. Zano daemon and block-template compatibility

- [ ] verify the audited Zano commit and runtime version intended for mainnet
- [ ] confirm mainnet `getblocktemplate` fields exercise the same canonical
      mining-header path used by the current exact-Zano tests
- [ ] confirm the active mainnet hardfork/miner-transaction rules are compatible
      with the direct PPLNS miner-transaction construction path
- [ ] run a non-mining mainnet template/header compatibility check against a
      synchronized local mainnet daemon
- [ ] do not submit a mainnet block during compatibility validation

### 5. Persistence and restart recovery

- [x] durable share stores are bound to the canonical sidechain ID
- [x] production testnet backup/restore and restart recovery have been exercised
- [ ] add a regression proving a testnet store cannot be opened as a mainnet
      store and vice versa, if not already covered explicitly
- [ ] define mainnet backup cadence, retention, restore drill, and rollback
      ownership before public operation

### 6. P2P bootstrap and seed infrastructure

- [x] seed framework supports network-specific defaults
- [x] mainnet built-in seed list is currently empty
- [x] testnet DNS bootstrap has been validated live without explicit peers
- [ ] provision at least two dedicated mainnet seed nodes in independent failure
      domains
- [ ] assign stable mainnet DNS names; do not reuse the testnet hostnames
- [ ] verify public TCP reachability and DNS resolution from an external host
- [ ] perform a fresh-node mainnet default-seed bootstrap proof before adding
      the endpoints to a release
- [ ] retain explicit peer wiring between seed operators where useful; seed
      nodes do not need to bootstrap through a list containing themselves

### 7. Mainnet operator security

- [ ] run Zano RPC on loopback only
- [ ] keep metrics on loopback or behind authenticated monitoring
- [ ] expose only the intended P2P port publicly on seed nodes
- [ ] expose Stratum only on nodes intended to serve miners
- [ ] use a dedicated unprivileged service account
- [ ] verify environment-file permissions and avoid logging wallet identifiers
- [ ] validate systemd hardening, file ownership, limits, and restart policy on
      the mainnet host image
- [ ] remove stale firewall rules before launch

### 8. Mainnet observability and rollback

- [ ] define launch health gates for `up`, persistence, daemon height, P2P peer
      count, sidechain tip, template-refresh failures, and block-submission
      outcomes
- [ ] preserve previous binaries and unit backups through the initial mainnet
      soak
- [ ] document the exact rollback trigger and procedure
- [ ] perform a controlled restart and recovery drill before enabling miners

### 9. Release and launch sequence

- [ ] complete all code/regression gaps on this audit branch
- [ ] pass the full normal and exact-Zano test suites
- [ ] build and smoke-test the release archive
- [ ] independently verify the published archive, checksum, version, build
      metadata, dependencies, and mainnet seed policy
- [ ] deploy mainnet seed nodes first with Stratum disabled
- [ ] complete a bounded P2P-only mainnet soak
- [ ] enable mining on one canary node only after the P2P-only soak passes
- [ ] enable additional mainnet mining endpoints only after canary validation

## Current blocking items

Permanent mainnet seed infrastructure is intentionally blocked on the remaining
audit gates above. The first implementation work should focus on deterministic
mainnet configuration/address regression coverage and on verifying the active
mainnet daemon/miner-transaction assumptions against the pinned Zano source.

The existing beta.2 testnet nodes should remain running unchanged while this
audit proceeds, and their previous binaries and unit backups should remain
available until the longer beta soak is considered complete.
