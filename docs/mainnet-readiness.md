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
- [x] re-audit every consensus-relevant parameter against the intended mainnet
      launch profile
- [x] record the exact mainnet sidechain ID and parameter encoding in the
      operator launch checklist

### 3. Wallet and payout address semantics

- [x] standard payout-address decoding is checksum- and prefix-validated
- [x] verify from the pinned Zano source that classic standard addresses use
      base58 prefix `0xc5` for both mainnet and testnet at the audited revision
- [x] add a pinned non-secret real-world mainnet standard-address vector
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
- [x] confirm the synchronized mainnet daemon is beyond the active hardfork
      activation and reports the expected HF7 runtime rules
- [x] confirm mainnet `getblocktemplate` fields exercise the same canonical
      mining-header path used by the current exact-Zano tests
- [x] run a non-mining mainnet template/header compatibility check against a
      synchronized local mainnet daemon
- [x] do not submit a mainnet block during compatibility validation

### 5. Persistence and restart recovery

- [x] durable share stores are bound to the canonical sidechain ID
- [x] production testnet backup/restore and restart recovery have been exercised
- [x] add a regression proving a testnet store cannot be opened as a mainnet
      store and vice versa; exact-Zano validation passes in both directions
- [x] define mainnet backup cadence, retention, restore drill, and rollback
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

CI and release workflows now pin the same HF7 commit and install the explicit OpenSSL development dependency. The packaged BUILD-INFO source pin and ProgPoWZ audit baseline were updated to the same commit. CI/release-workflow validation and the isolated HF7-capable testnet network comparison were subsequently completed; the results are recorded below.


### HF7 isolated testnet daemon build

The isolated Zano testnet daemon built from `b400b93f5d8bae42d5f5ac643c804d30faf9f8de` completed successfully as `Zano_testnet v2.2.3.603[testnet-b400b93]`. The audited binary is `/home/jdawg/work/projects/zano-hf7-audit-b400b93/build-testnet-p2pool-audit/src/zanod` with SHA-256 `d2bc2da109e8945f1e4724a0a360c77712fd0371d9ab1bf6273d5d49ba2ed507`. Testnet RPC/P2P defaults remain 12111/11314 and no production VPS was modified. The isolated live network-view comparison was subsequently completed; the result is recorded below.

## HF7 isolated testnet network-view result

The isolated build-603 testnet daemon established the authoritative HF7
boundary behavior without modifying either production VPS. The build-603 node
validated the same canonical history as the deployed build-506 nodes through
height 1050. HF7 becomes active for block height 1051.

At that transition the build-603 daemon rejected the legacy branch with a
transaction whose `hardfork_id` remained 6 while the current hardfork was 7.
It also rejected build-506/build-513 peers as below the minimum client build
for the current hardfork era. The two deployed production testnet daemons agree
with each other on a different height-1051 block and therefore remain healthy
inside a populated legacy HF6 partition rather than on the authoritative HF7
testnet chain.

Consequences:

- production testnet has not been mutated;
- the existing build-506 testnet deployment must not be used as evidence of
  HF7 compatibility;
- upgrading only one of the two production daemons would intentionally split
  the current P2Pool test deployment and is therefore not an appropriate
  canary procedure;
- the authoritative HF7 behavior is now understood well enough to continue
  the independent mainnet-readiness audit.


## HF7 synchronized mainnet and live-template validation

An isolated mainnet daemon built from exact Zano commit
`b400b93f5d8bae42d5f5ac643c804d30faf9f8de` was validated as
`Zano v2.2.3.603[b400b93]`.

The daemon binary used for the audit had SHA-256:

`d41cdaf6bf962a0b05f87d8cf4547236a9b1700c5daabd9ec04d542a953ff765`

The official LMDB predownload snapshot at height 3833000 was installed into an
isolated workstation data directory. The resulting LMDB reached approximately
25.64 GiB and the daemon synchronized forward onto the current HF7 mainnet
chain. A later online-state gate reported:

- local height: 3849193;
- network height: 3849193;
- `daemon_network_state=2`;
- eight outgoing peers;
- five synchronized peers;
- all eight `is_hardfok_active` entries true.

A dedicated P2Pool exact-Zano audit build was then configured against the same
HF7 Zano source and Boost 1.84. Static Boost.Serialization was used and the
resulting `zano-p2pool` binary had SHA-256:

`08cb29b1c9d908452032e300e82296b13838574a758ba11de151cad6606b9171`

The complete exact-Zano regression suite passed 48/48 before the live mainnet
template check.

The live non-mining template audit was then performed against the synchronized
loopback-only daemon. At the start of that audit the daemon reported height and
network height 3849231, `daemon_network_state=2`, eight outgoing peers, eight
synchronized peers, and all eight hardfork flags active.

P2Pool successfully fetched and parsed a current mainnet `getblocktemplate` and
derived the canonical ProgPoWZ mining work:

- template height: 3849231;
- ProgPoWZ epoch: 128;
- previous block hash:
  `32f0f2c75f2dd27202b0d2834ddba2a22806563c0653a019a0eb593973999352`;
- difficulty: `36291792275876`;
- target:
  `000000000007c181b24008b05489e84c21b31f5c74ef8d910c17b41e6d5b68d9`;
- ProgPoWZ seed:
  `7c4fb8a5d141973b69b521ce76b0dc50f0d2834d817c7f8310a6ab5becc6bb0c`;
- serialized block-template size: 1873 bytes;
- regular transaction count: 15;
- canonical mining blob size: 81 bytes;
- derived mining header:
  `7b75ced7a55b80cb66157c317654bae4a15b3210501c201c658a5a8b4c3648b0`.

The template's `prev_hash` was independently queried from the daemon as the
canonical block hash at height 3849230 and matched exactly.

The one-shot P2Pool process exited with status zero. Stratum, P2Pool networking,
metrics, and persistent share storage were not started. Neither `submitblock`
nor `submitblock2` was invoked, and no mainnet block was submitted.

The captured one-shot template evidence file had SHA-256:

`ed0d875302598bdfa9d814dc77105b314d2ea6d406418d02b508d52c0d19e2a5`

This closes the synchronized-daemon and live
`getblocktemplate`/mining-header compatibility gates for the audited HF7
build. It does not authorize public mainnet mining: mainnet sidechain launch
parameters, seed infrastructure, operator security, observability, rollback,
and launch sequencing remain open gates.

## Mainnet v3 consensus launch profile

The mainnet sidechain consensus profile was independently re-audited after the
HF7 live-daemon compatibility pass. The canonical profile is frozen under
parameter domain `ZP2SIDV3` with parent-network tag `2`.

The launch profile is:

- parameter version: 3;
- parent network: mainnet (`2`);
- minimum share version: 2;
- maximum share version: 2;
- maximum future timestamp tolerance: 60 seconds;
- maximum parent timestamp backstep: 60 seconds;
- target share interval: 10 seconds;
- minimum share difficulty: 100000000;
- difficulty history: 2160 shares;
- PPLNS share-history cap: 32 shares;
- PPLNS work cap: 2x current Zano network difficulty.

The canonical 67-byte parameter encoding is:

`5a50325349445633020202000000000000003c000000000000003c000000000000000a0000000005f5e100000000000000087000000000000000200000000000000002`

The resulting canonical mainnet SidechainId is:

`8cfa674d782bfc0a3824d654617c814da98ea2ec64b5701a41ddd40a522225bf`

The encoding and SidechainId are pinned by `p2p_protocol_test`.
`runtime_network_test`, `persistence_network_test`, `pplns_test`,
`share_chain_test`, and `p2p_protocol_test` all passed during the independent
profile audit.

This profile is the v3 mainnet consensus identity. Any future
consensus-relevant parameter change must use a new parameter version/domain and
therefore derive a different SidechainId rather than silently altering the
existing network.


## Pinned public mainnet standard-address vector

Exact Zano HF7 source commit
`b400b93f5d8bae42d5f5ac643c804d30faf9f8de` publishes this classic standard
Zano address as an RPC example:

`ZxCSpsGGeJsS8fwvQ4HktDU3qBeauoJTR6j73jAWWZxFXdF7XTbGm4YfS2kXJmAP4Rf5BVsSQ9iZ45XANXEYsrLN2L2W77dH7`

The exact source defines the standard-address Base58 prefix as `0xc5`.
P2Pool decodes the public vector to:

- spend public key:
  `558d9e567d08964189c1d005ddf6a1b6d8c8b00a89b0223bae83990a1e7cb718`;
- view public key:
  `1f0468510235e5c46abd28e23da6e39e3dd3856664d71b060288f5a10f279aad`.

The synchronized `Zano v2.2.3.603[b400b93]` mainnet daemon independently
accepted this address as the reward destination during the successful
non-mining `getblocktemplate` compatibility audit.

The vector is pinned directly in `zano_address_test`. It contains public
address material only and no wallet secret keys.

## Mainnet recovery policy

The operator deployment guide now defines the mainnet recovery generation as
the matching `shares.dat`, `shares.dat.validation`, and `shares.dat.work/`
state together with the active/previous binaries and service configuration.

The initial-mainnet policy requires:

- a quiesced backup before every runtime/configuration change;
- at least daily backups during the initial soak;
- retention of the latest seven daily generations;
- preservation of each pre-upgrade generation until the replacement completes
  its bounded soak and restart/recovery validation;
- at least one verified off-node copy;
- SHA-256 verification after archival/copy;
- a controlled restore drill before miners are enabled.

Rollback ownership belongs to the node operator performing the canary change.
A failed canary must be restored to the complete matching recovery generation
and previous known-good binary rather than mixing persistence generations or
deleting durable sidechain history.

The Zano blockchain database remains outside the P2Pool recovery generation and
is managed independently.
