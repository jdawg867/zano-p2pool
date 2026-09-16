# zano-p2pool

Experimental decentralized P2Pool-style mining software for **Zano**.

> Status: early development. Do not use with funds or production mining yet.

## Goal

Build a native Zano P2Pool network where:

- miners connect to a local node over Stratum;
- P2Pool nodes exchange and validate shares peer-to-peer;
- each node maintains/verifies the share chain itself;
- ProgPoWZ shares are independently verified;
- full-difficulty solutions are surfaced as block candidates for controlled submission;
- payout accounting is derived deterministically from the share chain;
- a later protocol phase investigates direct non-custodial miner payouts.

## Milestone 0.1

The first milestone covers Zano daemon integration:

- connect to `zanod` JSON-RPC;
- call `getblocktemplate`;
- parse the live PoW template;
- expose height, previous hash, difficulty, reward, seed, and blob;
- provide a `submitblock` RPC method;
- unit-test block-template parsing;
- validate against Zano testnet first.

Milestone 0.1 was validated against live Zano testnet and merged to `main`.

## Milestone 0.2

The second milestone implements the consensus-critical local PoW verification layer:

- parse Zano's decimal `uint128` difficulty;
- compute the full 256-bit target as `floor((2^256 - 1) / difficulty)`;
- compare 32-byte hashes using Zano-compatible byte ordering;
- derive Zano's canonical mining header directly from RPC `blocktemplate_blob`;
- implement CryptoNote fast hash and transaction tree hashing needed by the mining blob;
- wrap the exact ProgPoWZ implementation embedded in audited Zano source;
- distinguish P2Pool share difficulty from full network difficulty;
- classify local candidates as `Invalid`, `Share`, or `Block` without submitting them;
- validate the standalone header derivation byte-for-byte against Zano on live testnet.

Milestone 0.2 was validated against live Zano testnet and merged to `main`.

## Milestone 0.3

The third milestone establishes the deterministic in-memory P2Pool share chain:

- canonical versioned share serialization and deterministic IDs;
- parent linkage and root rules;
- fixed-width share/network difficulty commitments;
- checked cumulative 256-bit share work;
- orphan retention/promotion and stale-fork tracking;
- deterministic best-tip selection and cumulative-work reorgs;
- trusted Zano work-context matching;
- explicit timestamp rules;
- mandatory exact local ProgPoWZ verification before production-admitted work contributes to the chain;
- explicit share vs. full-network block-candidate classification.

Milestone 0.3 passed local exact-Zano Release tests and CI and was merged to `main`.

## Milestone 0.4

The fourth milestone adds the local miner-facing Stratum foundation:

- Zano-compatible JSON-RPC 2.0 codec for `eth_submitLogin`, `eth_getWork`, `eth_submitHashrate`, and `eth_submitWork`;
- exact `[header, seed, target, height]` work formatting;
- deterministic worker/session state;
- monotonic versioned work-template registry;
- per-session requested-difficulty clamping and target calculation;
- share difficulty capped at current Zano network difficulty;
- current/stale/unknown header classification per session;
- per-session `(job_version, nonce)` duplicate suppression;
- `eth_submitWork` routing through exact local ProgPoWZ and `ShareChain::submit_share()`;
- accepted-share vs. full-network block-candidate classification without automatic daemon submission;
- loopback TCP listener with line-delimited JSON-RPC framing;
- persistent executable Stratum mode with live daemon template refresh;
- deterministic suppression of randomized same-tip `getblocktemplate` churn.

Milestone 0.4 was validated with a local exact-Zano Release build passing all 13 tests, live Zano testnet template refresh on `127.0.0.1:3333`, and a real SRBMiner-MULTI `progpow_zano` session submitting repeatedly accepted shares through the verified Stratum path. It is merged to `main`.

## Milestone 0.5

The fifth milestone completes the core node-to-node P2P network:

- deterministic versioned binary framing and capability negotiation;
- sidechain/network identity binding during handshake;
- configurable TCP listener/client sessions with bounded framing;
- `ShareAnnounce`, `ShareRequest`, `ShareResponse`, and tip synchronization;
- exact parent/orphan recovery by `ShareId`;
- independently verified mining-context exchange;
- trusted historical mining-work retrieval;
- managed reconnect/backoff;
- deterministic peer scoring and temporary bans;
- consensus/reorg convergence tests;
- adversarial parser fuzzing;
- live two-node Zano testnet validation.

Peer-provided height, cumulative work, mining headers, payout data, and mining
contexts are never accepted as consensus authority on their own. Local
verification and trusted-work promotion remain mandatory before peer shares can
affect the sidechain.

Milestone 0.5 is complete and merged to `main`. By completion of the core P2P
phase, the exact-Zano regression suite had grown to 33 tests and the complete
two-node share-propagation path had been validated with real
SRBMiner-MULTI `progpow_zano` shares.

## Milestone 0.6

The sixth milestone completes deterministic PPLNS accounting and direct
non-custodial payout construction:

- deterministic best-chain PPLNS window construction;
- exact work-weighted reward allocation;
- deterministic remainder handling;
- payout-capable share v2 identity binding;
- current Zano HF6 miner-transaction consensus audit;
- direct multi-recipient coinbase construction;
- canonical balance/range-proof generation and verification;
- P2P payout-plan verification;
- live multi-recipient Zano testnet block submission.

The current HF6 consensus rules support the required direct multi-recipient
coinbase path, so a temporary custodial payout layer is not required.

Milestone 0.6 is complete and merged to `main`.

## Milestone 0.7

The seventh milestone hardens the node for persistent multi-node operation:

- canonical mainnet/testnet sidechain parameters and `SidechainId`;
- branch-relative share-difficulty consensus;
- restart-safe append-only share persistence;
- Prometheus-style metrics and health endpoints;
- bounded Stratum and P2P admission/rate limits;
- adversarial runtime regression tests;
- seed-node bootstrap framework;
- published P2P protocol-v2 specification;
- packaged Linux systemd/operator assets;
- release-archive installation smoke testing.

The built-in seed framework is complete, but permanent public seed
infrastructure remains intentionally deferred until mainnet readiness.

Core Milestone 0.7 hardening is merged to `main`; operator/release readiness
continues on testnet.

## Milestone 0.8

Current work hardens restart-history trust recovery and automatic miner-service
activation after replay.

A persisted share record proves durable structural history, but structural replay
alone does not make old consensus history trusted. The restart path therefore
distinguishes connected shares from shares whose ancestry has been independently
revalidated.

Completed checkpoints include:

- replayed shares are initially structural unless historical work can be
  revalidated;
- historical mining work is bound to exact Zano height/header context;
- local archived observations are re-audited against canonical daemon headers;
- missing historical work can be retrieved from an authenticated P2P peer;
- recovery walks ancestry parent-first until it reaches a validated boundary;
- connected replay shares are revalidated in place rather than re-admitted;
- PPLNS authority remains unavailable while required ancestry is unvalidated;
- Stratum stays fail-closed for non-empty replay history that has not completed
  ancestry recovery;
- finished mining-work cooldown entries no longer consume active request
  capacity for unrelated historical keys;
- deferred historical ancestry is bounded by the consensus history horizon
  instead of the much smaller concurrent-request limit;
- once replay ancestry becomes canonical in the same running process, the block
  submitter and Stratum server activate automatically.

The Milestone 0.8 implementation passes the complete
**45/45 exact-Zano regression suite**.

Runtime validation used an isolated immutable testnet fixture containing
96 accepted/connected shares and 106 archived work contexts. A controlled
receiver restart with selected work temporarily unavailable produced:

- `revalidated=62`;
- `missing-work=18`;
- `parent-unvalidated=16`;
- `rejected=0`;
- Stratum deferred with no listener.

The exact original work was then restored byte-for-byte while the receiver stayed
running. After authenticated P2P recovery, the node crossed
`historical-trust=trusted`, revalidated the replayed share in place, kept
`p2p_admitted_shares_total=0`, advanced the Stratum job sequence from 0 to 1,
and opened the Stratum listener automatically.

This provides the positive runtime proof for late Stratum activation without
weakening the historical trust boundary.

## Requirements

Ubuntu/Debian:

```bash
sudo apt update
sudo apt install -y \
  build-essential \
  cmake \
  libboost-dev \
  libcurl4-openssl-dev \
  libjson-c-dev \
  pkg-config
```

## Build

Lightweight build:

```bash
cmake -S . -B build
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

For Stratum and exact ProgPoWZ verification, configure against an audited Zano source tree:

```bash
cmake -S . -B build-zano \
  -DCMAKE_BUILD_TYPE=Release \
  -DZANO_P2POOL_ZANO_SOURCE_DIR=/path/to/zano
cmake --build build-zano -j"$(nproc)"
ctest --test-dir build-zano --output-on-failure
```

## Run

`getblocktemplate` needs a Zano payout address.

Testnet is the development default:

```bash
./build-zano/zano-p2pool \
  --network testnet \
  --wallet YOUR_TESTNET_ZANO_ADDRESS
```

This resolves to:

```text
http://127.0.0.1:12111/json_rpc
```

You can also select mainnet explicitly:

```bash
./build-zano/zano-p2pool \
  --network mainnet \
  --wallet YOUR_ZANO_ADDRESS
```

or override the RPC endpoint directly with `--rpc-url`.

Stratum development mode is opt-in and defaults to loopback:

```bash
./build-zano/zano-p2pool \
  --network testnet \
  --wallet YOUR_TESTNET_ZANO_ADDRESS \
  --stratum \
  --stratum-bind 127.0.0.1 \
  --stratum-port 3333 \
  --template-refresh-seconds 5
```

In canonical sidechain consensus mode, the effective miner share difficulty is
derived from the selected sidechain branch and parent-network difficulty.
`--stratum-difficulty` is therefore ignored in that mode. Miners should log in
with a standard Zano payout address so payout-capable v2 shares can be constructed.

Expected startup output includes the independently derived mining header and the local Stratum listener.

Runtime observability is also opt-in and defaults to loopback. Enable the Prometheus-style metrics and health endpoints with:

```bash
./build-zano/zano-p2pool \
  --network testnet \
  --wallet YOUR_TESTNET_ZANO_ADDRESS \
  --stratum \
  --p2p \
  --metrics \
  --metrics-bind 127.0.0.1 \
  --metrics-port 37890
```

The server exposes `GET /metrics` and `GET /healthz`. Metrics are intentionally label-free and do not expose wallet addresses, node IDs, share IDs, peer identities, or other payout-identifying values. The Stratum work publication counter is named `zano_p2pool_stratum_job_sequence`; runtime logs use `Stratum job #N` so this process-local sequence cannot be confused with the zano-p2pool, P2P, or sidechain-share protocol version.

Long-running Stratum, P2P, and metrics modes keep retrying if `zanod` is
temporarily unavailable. RPC retries use bounded exponential backoff from 1 to
30 seconds by default. Override the bounds with
`--rpc-reconnect-initial-seconds` and `--rpc-reconnect-max-seconds`.

No default seed nodes are currently active. Connect test nodes explicitly with
one or more `--p2p-peer HOST:PORT` entries; duplicate endpoints are removed.
The built-in seed framework remains available for permanent mainnet
infrastructure, and `--no-seed-nodes` can disable those defaults when added.

## Current Zano network defaults

From the current Zano source:

| Network | Daemon RPC | Zano P2P | Zano Stratum |
|---|---:|---:|---:|
| mainnet | 11211 | 11121 | 11777 |
| testnet | 12111 | 11314 | 11888 |

Zano testnet itself is a testnet build (`cmake -D TESTNET=TRUE ..`).

## Current Zano references

- `getblocktemplate`: https://docs.zano.org/docs/build/rpc-api/daemon-rpc-api/getblocktemplate/
- `submitblock`: https://docs.zano.org/docs/build/rpc-api/daemon-rpc-api/submitblock/
- Zano source: https://github.com/hyle-team/zano
- Zano difficulty implementation: https://github.com/hyle-team/zano/blob/master/src/currency_core/difficulty.cpp
- Zano target tests: https://github.com/hyle-team/zano/blob/master/tests/hash-target.cpp
- Zano ProgPoWZ miner reference: https://github.com/hyle-team/progminer

## Development roadmap

See the [`P2P protocol specification`](docs/p2p-protocol.md) for the current
public wire contract and [`docs/roadmap.md`](docs/roadmap.md) for development
status. The [`Linux operator deployment guide`](docs/operator-deployment.md)
covers the packaged systemd service, safe default binds, health validation,
upgrades, and rollback.

## Safety

This repository is experimental consensus/mining software. Until share validation,
difficulty calculations, block serialization, share-chain consensus, Stratum, P2P,
and payout rules have extensive test coverage, it should only be used for development
and controlled testing.
