# zano-p2pool

Experimental decentralized P2Pool-style mining software for **Zano**.

> Status: early development. Testnet is the default. Do not use with funds or production mining yet.

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

The first milestone intentionally covers only Zano daemon integration:

- connect to `zanod` JSON-RPC;
- call `getblocktemplate`;
- parse the live PoW template;
- expose height, previous hash, difficulty, reward, seed, and blob;
- provide a `submitblock` RPC method;
- unit-test block-template parsing;
- validate first against Zano testnet.

## Zano network defaults

Current Zano source defines:

| Network | Daemon RPC | Zano P2P | Zano Stratum |
| --- | ---: | ---: | ---: |
| Mainnet | 11211 | 11121 | 11777 |
| Testnet | 12111 | 11314 | 11888 |

Zano testnet binaries are built with `TESTNET=TRUE`. This project's development default is testnet so early testing cannot accidentally target mainnet simply by omitting `--network`.

## Requirements

Ubuntu/Debian:

```bash
sudo apt update
sudo apt install -y \
  build-essential \
  cmake \
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

## Run on Zano testnet

`getblocktemplate` needs a Zano payout address.

```bash
./build/zano-p2pool \
  --network testnet \
  --wallet YOUR_TESTNET_ZANO_ADDRESS
```

That defaults to:

```text
http://127.0.0.1:12111/json_rpc
```

You can override the endpoint when needed:

```bash
./build/zano-p2pool \
  --network testnet \
  --rpc-url http://127.0.0.1:12111/json_rpc \
  --wallet YOUR_TESTNET_ZANO_ADDRESS
```

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

Mainnet mode exists for later development but currently emits a warning:

```bash
./build/zano-p2pool --network mainnet --wallet YOUR_ZANO_ADDRESS
```

## Current Zano references

- `getblocktemplate`: https://docs.zano.org/docs/build/rpc-api/daemon-rpc-api/getblocktemplate/
- `submitblock`: https://docs.zano.org/docs/build/rpc-api/daemon-rpc-api/submitblock/
- Zano source: https://github.com/hyle-team/zano
- Zano ProgPoWZ miner reference: https://github.com/hyle-team/progminer

## Development roadmap

See [`docs/roadmap.md`](docs/roadmap.md).

## Safety

This repository is experimental consensus/mining software. Until share validation,
difficulty calculations, block serialization, and payout rules have extensive test
coverage, it should only be used for development and controlled testing.
