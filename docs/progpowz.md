# ProgPoWZ compatibility notes

This document records the consensus-facing mining path observed in the current Zano source before zano-p2pool accepts shares.

Source audit baseline: Zano commit `1508cf6ae3ef44a52d66137d30f800b06ce917ee` (2026-08-28 audit).

## Difficulty and target

Zano defines mining difficulty as an unsigned 128-bit integer.

For a positive difficulty `D`, the full 256-bit boundary is:

```text
target = floor((2^256 - 1) / D)
```

`currency::check_hash()` validates the final 32-byte PoW hash against that rule. The incoming hash bytes are interpreted as one big-endian 256-bit integer for the numeric comparison.

zano-p2pool exposes targets as 64-character big-endian hexadecimal strings. This is a presentation choice; Zano's `difficulty_to_boundary_long()` stores the same numeric boundary into its `crypto::hash` memory in little-endian limb order.

## Zano ProgPoWZ mining pipeline

Zano's canonical block PoW path is:

```text
block
  -> get_block_hashing_blob(block)
  -> set serialized 64-bit nonce to 0
  -> cn_fast_hash(hashing_blob)
  -> ProgPoW 0.9.2 with (height, header hash, nonce)
  -> final 256-bit ProgPoW hash
  -> check_hash(final_hash, difficulty)
```

Important compatibility details:

- Zano's Ethash epoch length is `30000` blocks.
- Epoch number is `height / 30000` using integer division.
- The ProgPoW revision declared by Zano is `0.9.2`.
- The canonical entry point is `progpow::hash(context, height, header_hash, nonce)`.
- Zano's production core obtains a full epoch context for the calculated epoch.
- Zano's block hashing blob stores the 64-bit mining nonce starting at byte offset `1`.
- Zano zeroes that nonce before computing the `cn_fast_hash` that becomes the ProgPoW header hash.
- The actual submitted nonce is passed separately into ProgPoW.
- The final `result.final_hash` is the value checked against worker/share difficulty and network difficulty.

## Zano Stratum behavior

Zano's built-in Stratum server follows the same path. It keeps the nonce-zeroed block hashing blob, hashes it once with `cn_fast_hash`, and distributes that resulting header hash as mining work. A submitted share contains a nonce associated with that work. The server recomputes the ProgPoW final hash from height, header hash, and nonce, then checks the worker difficulty.

This separation is useful for P2Pool: a share job can carry an immutable header hash plus height/epoch information, while each miner varies only the nonce.

## Exact-source backend

During testnet development, zano-p2pool can compile against the exact ProgPoW implementation embedded in a local Zano source checkout instead of copying miner code into this repository.

Configure with an absolute Zano source path:

```bash
cmake -S . -B build-zano \
  -DCMAKE_BUILD_TYPE=Release \
  -DZANO_P2POOL_ZANO_SOURCE_DIR="$HOME/path/to/zano"

cmake --build build-zano -j"$(nproc)"
ctest --test-dir build-zano --output-on-failure
```

When enabled, `progpowz_hash()` supports two context modes:

- `Light`: uses the Ethash light cache. It computes the same result with far less memory and is used for deterministic compatibility tests.
- `Full`: uses Zano's full epoch context/DAG and is intended for high-throughput pool share verification.

The normal build remains available without a Zano checkout. In that configuration the ProgPoWZ API exists but reports the backend unavailable and hashing calls fail explicitly rather than silently using a different algorithm.

## Exact-Zano compatibility vector

The compatibility test pins the output of the exact ProgPoWZ implementation embedded in the audited Zano source commit above. We intentionally use Zano's own output as the consensus reference instead of assuming that a vector copied from another ProgPoW implementation has identical parameterization or semantics.

For block number `0`:

```text
header     ffeeddccbbaa9988776655443322110000112233445566778899aabbccddeeff
nonce      123456789abcdef0
mix hash   1476a46ba81f00a5acd854e603c79a219fcb128db00b1809718855128471eb71
final hash 4feba8deef1ac892ee334cf258d029cc8651f037215f1767b8ce5c704a4fd68b
```

That final hash meets Zano difficulty `3` and fails difficulty `4`, which also makes it a deterministic boundary vector for local Share / Invalid / Block classification tests.

GitHub CI checks out only `contrib/ethereum/libethash` from the audited Zano commit and runs this exact vector with always-on test checks. If the pinned consensus dependency or our wrapper changes the output, CI fails explicitly even in Release builds.

## Licensing policy

The ProgPoW/libethash implementation embedded in Zano identifies itself as Apache-2.0 licensed code. zano-p2pool does not import the GPL-licensed `hyle-team/progminer` source. `progminer` remains useful as an interoperability/Stratum reference only unless the project's licensing strategy explicitly changes.
