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

- [x] source-audit the historical pinned Zano commit
      `1508cf6ae3ef44a52d66137d30f800b06ce917ee`
- [x] move CI/release exact-Zano source pin to HF7-capable Zano release
      `b400b93f5d8bae42d5f5ac643c804d30faf9f8de` (build 603, minimum build
      600 for HF7); branch validation passes locally and CI is the next gate
- [x] pin the source constants relevant to the current payout path:
      transaction version 4, HF6 miner-transaction output cap 32, and
      125000-byte full-reward zone
- [x] pin HF6 activation configuration from the audited source: mainnet active
      after height 3833000 with minimum build 501; testnet active after height
      1050 with minimum build 474
- [x] select HF7-capable Zano build 603 at
      `b400b93f5d8bae42d5f5ac643c804d30faf9f8de` as the current audit target;
      final launch selection remains subject to a freshness recheck
- [ ] confirm the synchronized mainnet daemon is beyond the active hardfork
      activation and reports the expected HF7 runtime rules
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

The existing beta.2 testnet nodes must not be mutated until their current
runtime state is inspected, but they are now subject to an urgent compatibility
re-audit because upstream HF7 requires build 600 on testnet as well as mainnet.
Their previous binaries and unit backups must remain available during that
review.

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


## Pass 3 live-daemon finding: HF7 build cutoff

A clean mainnet daemon built from the historical exact-Zano pin
`1508cf6ae3ef44a52d66137d30f800b06ce917ee` successfully installed the
official height-3709300 LMDB snapshot, opened the expected mainnet genesis,
bound RPC and P2P to loopback, and exposed RPC at height 3709301. It could not
complete a P2P handshake with current public seed peers.

The current upstream Zano `release` branch is
`b400b93f5d8bae42d5f5ac643c804d30faf9f8de` (build 603). Its runtime
configuration adds HF7 at height 3833000 on mainnet and height 1050 on testnet,
with minimum build 600 in both networks. Current peers reject clients below the
active hardfork minimum build during handshake. This explains why the
historical build-506 daemon can establish TCP connections to current seed hosts
but receives no successful protocol handshake.

Consequences:

- build 506 is no longer a valid mainnet compatibility target;
- exact-Zano P2Pool tests must be re-audited against an HF7-capable source pin;
- the beta.2 testnet daemon deployment requires an independent HF7-capable
  network-view check because its historical daemon build may still be connected
  to a legacy pre-HF7 peer partition;
- no mainnet template/mining validation may proceed until the Zano pin is
  updated and the exact-Zano suite passes against the new pin.


### Testnet follow-up observation

The two deployed testnet daemons both reported height 212480, eight outgoing
connections, eight synchronized connections, zero height delta, and
`daemon_network_state=2`. This proves the deployed daemons still have a live
peer set; it does not prove that peer set is the current HF7 network.

The historical build-506 RPC returns seven `is_hardfok_active` booleans because
that source revision defines hardfork IDs 0 through 6 only. The seven true values
therefore demonstrate activation through HF6, not HF7. Current upstream release
defines an eighth hardfork ID (HF7) and minimum build 600.

Accordingly, the previous working hypothesis that build 506 had already been
fully disconnected by the HF7 cutoff is too strong. The remaining ambiguity is
whether the current build-506 testnet nodes are on the authoritative network or
on a still-populated legacy partition. Resolve this by building an isolated
HF7-capable release daemon and comparing its network height/peer view before
changing either production testnet daemon.


### HF7 build integration finding

The first P2Pool build against Zano release `b400b93f5d8bae42d5f5ac643c804d30faf9f8de` reached final linking but failed because current Zano `src/crypto/crypto.cpp` uses `OPENSSL_cleanse()` and P2Pool's extracted static curve backend did not propagate `OpenSSL::Crypto` to final consumers. This is a build-integration dependency change, not evidence of a consensus incompatibility.

The audit branch now links the exact Zano curve backend transitively with `OpenSSL::Crypto`. Validation against HF7 build 603 passes: the final binary resolves `libcrypto.so.3`, the complete exact-Zano suite passes 48/48, the critical mining/payout subset passes 7/7, and the long-lived mainnet guard remains fail-closed before RPC startup.

CI and release workflows now pin the same HF7 commit and install the explicit OpenSSL development dependency. The packaged BUILD-INFO source pin and ProgPoWZ audit baseline were updated to the same commit. The next gate is CI/release-workflow validation followed by an isolated HF7-capable testnet daemon network-view comparison before either production testnet daemon is changed.
