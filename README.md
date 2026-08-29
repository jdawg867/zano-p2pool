# zano-p2pool

Experimental decentralized P2Pool-style mining software for **Zano**.

> Status: early development. Do not use with funds or production mining yet.

## Goal

Build a native Zano P2Pool network where:

- miners connect to a local node over Stratum;
- P2Pool nodes exchange and validate shares peer-to-peer;
- each node maintains/verifies the share chain itself;
- ProgPoWZ shares are independently verified;
- full-difficulty solutions are submitted directly to `zanod`;
- payout accounting is derived deterministically from the share chain;
- a later protocol phase investigates direct non-custodial miner payouts.

## Milestone 0.1

The first milestone established Zano daemon integration and was validated against live Zano testnet before merging to `main`.

## Milestone 0.2

The second milestone established the consensus-critical local PoW path and was validated against live Zano testnet before merging to `main`.

## Milestone 0.3

Current work builds the local P2Pool share chain:

- versioned canonical share serialization and deterministic share IDs;
- parent linkage with zero-parent roots;
- fixed-width share/network difficulty commitments;
- checked 256-bit cumulative share work;
- orphan retention and deterministic promotion;
- stale-fork tracking and cumulative-work reorgs;
- deterministic best-tip tie breaking;
- next: timestamp rules and mandatory local ProgPoWZ verification before a share can contribute production-safe work.

The current `ShareChain::add_share()` checkpoint is structural only. It is intentionally
not yet a production admission API because claimed share difficulty has not been gated
by the local ProgPoWZ verifier inside the chain path.

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

```bash
cmake -S . -B build
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

## Run

`getblocktemplate` needs a Zano payout address. Testnet is the development default:

```bash
./build/zano-p2pool \
  --network testnet \
  --wallet YOUR_TESTNET_ZANO_ADDRESS
```

This resolves to `http://127.0.0.1:12111/json_rpc`.

## Current Zano network defaults

| Network | Daemon RPC | Zano P2P | Zano Stratum |
|---|---:|---:|---:|
| mainnet | 11211 | 11121 | 11777 |
| testnet | 12111 | 11314 | 11888 |

## Current Zano references

- `getblocktemplate`: https://docs.zano.org/docs/build/rpc-api/daemon-rpc-api/getblocktemplate/
- `submitblock`: https://docs.zano.org/docs/build/rpc-api/daemon-rpc-api/submitblock/
- Zano source: https://github.com/hyle-team/zano
- Zano difficulty implementation: https://github.com/hyle-team/zano/blob/master/src/currency_core/difficulty.cpp
- Zano target tests: https://github.com/hyle-team/zano/blob/master/tests/hash-target.cpp
- Zano ProgPoWZ miner reference: https://github.com/hyle-team/progminer

## Development roadmap

See [`docs/roadmap.md`](docs/roadmap.md).

## Safety

This repository is experimental consensus/mining software. Until share validation,
difficulty calculations, block serialization, share-chain consensus, and payout rules
have extensive test coverage, it should only be used for development and controlled
testing.
