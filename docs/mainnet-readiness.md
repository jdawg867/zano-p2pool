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

The standard Zano payout-address decoder accepts the classic non-auditable
address format using base58 prefix `0xc5`. The pinned Zano source at
`1508cf6ae3ef44a52d66137d30f800b06ce917ee` defines
`CURRENCY_PUBLIC_ADDRESS_BASE58_PREFIX` as `0xc5` outside the
mainnet/testnet conditional, so the classic standard-address prefix is shared by
both builds at this audited revision. Integrated, auditable, and gateway address
formats use different prefixes and remain outside the P2Pool decoder's supported
surface.

## Audit gates

### 1. Network selection and defaults

- [x] runtime exposes explicit `--network testnet|mainnet`
- [x] testnet and mainnet daemon RPC defaults are distinct
- [x] default share-store paths are network-separated
- [x] runtime maps the selected network into P2P and sidechain parent-network
      identity
- [x] add regression coverage proving mainnet runtime configuration selects
      the expected RPC, P2P network, sidechain ID, and persistence namespace
      without contacting a live daemon

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
- [x] verify from the pinned Zano source that classic standard addresses use
      base58 prefix `0xc5` for both mainnet and testnet at the audited revision
- [ ] add a pinned non-secret real-world mainnet standard-address vector
- [x] network-specific prefix handling is not required for the currently
      supported classic standard-address format at the audited revision
- [x] integrated, auditable, gateway, and other unsupported address formats
      remain intentionally fail-closed unless explicitly implemented

### 4. Zano daemon and block-template compatibility

- [x] source-audit the pinned Zano commit
      `1508cf6ae3ef44a52d66137d30f800b06ce917ee`
- [x] pin the source constants relevant to the current payout path:
      transaction version 4, HF6 miner-transaction output cap 32, and
      125000-byte full-reward zone
- [x] pin HF6 activation configuration from the audited source: mainnet active
      after height 3833000 with minimum build 501; testnet active after height
      1050 with minimum build 474
- [ ] verify the exact Zano mainnet runtime version/build selected for launch
- [ ] confirm the synchronized mainnet daemon is beyond the required hardfork
      activation and reports the expected runtime rules
- [ ] confirm mainnet `getblocktemplate` fields exercise the same canonical
      mining-header path used by the current exact-Zano tests
- [ ] run a non-mining mainnet template/header compatibility check against a
      synchronized local mainnet daemon
- [ ] do not submit a mainnet block during compatibility validation

### 5. Persistence and restart recovery

- [x] durable share stores are bound to the canonical sidechain ID
- [x] production testnet backup/restore and restart recovery have been exercised
- [x] add a regression proving a testnet store cannot be opened as a mainnet
      store and vice versa; exact-Zano validation passes in both directions
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

### 9. Explicit mainnet activation guard

The current executable accepts `--network mainnet` and emits a warning, but it
does not otherwise prevent a user from starting Stratum/P2P on mainnet. While
the readiness audit is open, public beta builds should fail closed unless the
operator supplies a deliberate experimental-mainnet opt-in.

- [x] add an explicit `--experimental-mainnet` opt-in required for long-lived
      mainnet runtime
- [x] cover the deterministic guard and network mapping with regression tests
- [x] verify the CLI fails closed before RPC startup without the opt-in and
      rejects the opt-in when used with testnet
- [ ] document removal or replacement of the experimental guard as a launch
      gate

### 10. Release and launch sequence

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

## Pass 2 source-audit findings

The exact pinned Zano source confirms that classic standard-address prefix
`0xc5`, transaction version 4, the 32-output post-HF6 transaction limit, and
the 125000-byte full-reward zone are not testnet-only assumptions. Mainnet and
testnet do diverge in daemon ports, network identity, formation version, and
hardfork activation heights, which is expected and already reflected in P2Pool's
network/sidechain separation.

No address-decoder code change is required solely to distinguish classic
mainnet and testnet standard addresses at this revision. Deterministic
runtime-network regression coverage and the explicit long-lived-mainnet
activation guard are implemented and validated. The durable ShareStore boundary
is now also pinned to the canonical testnet/mainnet SidechainIds in both
directions: testnet stores are rejected by mainnet readers and mainnet stores are
rejected by testnet readers without mutating the original store. The exact-Zano
suite passes 48/48 with this regression enabled. The next external gate is a
non-mining compatibility check against a synchronized mainnet daemon.
