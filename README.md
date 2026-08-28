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

Current work adds the consensus-critical PoW validation layer:

- parse Zano's decimal `uint128` difficulty;
- compute the full 256-bit target as `floor((2^256 - 1) / difficulty)`;
- compare 32-byte hashes using Zano-compatible byte ordering;
- add exact boundary tests, including a target vector from live testnet;
- next: integrate the canonical ProgPoWZ hashing path.

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

`getblocktemplate` needs a Zano payout address.

Testnet is the development default:

```bash
./build/zano-p2pool \
  --network testnet \
  --wallet YOUR_TESTNET_ZANO_ADDRESS
```

This resolves to:

```text
http://127.0.0.1:12111/json_rpc
```

You can also select mainnet explicitly:

```bash
./build/zano-p2pool \
  --network mainnet \
  --wallet YOUR_ZANO_ADDRESS
```

or override the RPC endpoint directly with `--rpc-url`.

Expected output:

```text
zano-p2pool v0.1.0-dev
Network: testnet
RPC: http://127.0.0.1:12111/json_rpc

Template status: OK
Height:          ...
Previous hash:   ...
Difficulty:      ...
Block reward:    ...
ProgPoWZ seed:   ...
Blob bytes:      ...
```

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

See [`docs/roadmap.md`](docs/roadmap.md).

## Safety

This repository is experimental consensus/mining software. Until share validation,
difficulty calculations, block serialization, and payout rules have extensive test
coverage, it should only be used for development and controlled testing.
