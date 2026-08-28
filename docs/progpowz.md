# ProgPoWZ compatibility notes

This document records the consensus-facing mining path observed in the current Zano source before zano-p2pool implements share hashing.

Source audit baseline: Zano `master` around commit `1508cf6ae3ef44a52d66137d30f800b06ce917ee` (2026-08-28 audit).

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
- The full epoch context is obtained for the calculated epoch before hashing.
- Zano's block hashing blob stores the 64-bit mining nonce starting at byte offset `1`.
- Zano zeroes that nonce before computing the `cn_fast_hash` that becomes the ProgPoW header hash.
- The actual submitted nonce is passed separately into ProgPoW.
- The final `result.final_hash` is the value checked against worker/share difficulty and network difficulty.

## Zano Stratum behavior

Zano's built-in Stratum server follows the same path. It keeps the nonce-zeroed block hashing blob, hashes it once with `cn_fast_hash`, and distributes that resulting header hash as mining work. A submitted share contains a nonce associated with that work. The server recomputes the ProgPoW final hash from height, header hash, and nonce, then checks the worker difficulty.

This separation is useful for P2Pool: a share job can carry an immutable header hash plus height/epoch information, while each miner varies only the nonce.

## Implementation policy

Consensus behavior must be tested against Zano vectors before a share is accepted.

The ProgPoW implementation embedded in Zano identifies itself as Apache-2.0 licensed code. zano-p2pool will not import a large implementation blindly; we will either use a pinned compatible upstream dependency or a small reviewed integration of the exact required components, with license notices retained.

The GPL-licensed `hyle-team/progminer` repository is useful as an interoperability reference, but its code should not be copied into this project unless the project's licensing strategy explicitly permits that dependency.
