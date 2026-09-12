# Historical parent audit (not work promotion)

This checkpoint adds `zano-p2pool-work-audit`, a separate diagnostic tool. It
reads one existing testnet `.work` record, verifies its archive checksum and
sidechain binding, validates its HF6 structural bindings (including the actual
blob parent and coinbase height), and queries the operator's daemon with
`getblockheaderbyheight` for template height minus one. It queries that height
twice and rejects a changed hash, wrong height, orphan response, malformed hash,
or non-OK status. A matched result is explicitly `matched-untrusted`.

Example on VPS2, after building this branch:

```bash
build-zano-boost184/zano-p2pool-work-audit --testnet \
  /var/lib/zano-p2pool-soak/shares.dat.work/CONTENT_ID.work \
  http://127.0.0.1:12111/json_rpc
```

Replace CONTENT_ID with an existing archive filename. Run as a user who can read
that archive. Exit zero means only that this limited audit matched; it is never
a mining authorization. Exit one means a mismatch or an operational/parsing
error. Exit two indicates incorrect arguments. No share store or trusted-work
registry is modified, and the running pool does not invoke this tool. Two RPC
samples can detect an intervening change but cannot prove finality or exclude
an A-to-B-to-A reorganization; any future trust crossing needs fresh chain
validation and invalidation on reorganization.

## Why this does not yet accept historical shares

The pinned Zano daemon (1508cf6ae3ef44a52d66137d30f800b06ce917ee) implements
`fill_block_header_response` using `block_difficulty(response.height)` and
`get_block_reward(blk, block_hash)`. These describe the returned block. They
must not be used as the next PoW template's difficulty and base reward: Zano
contains both PoW and PoS blocks. The standard getblocktemplate request has no
historical parent selector. The audit deliberately does not construct a
P2pMiningAnchor from peer-supplied difficulty, reward or seed.

Before historical work can be promoted, implement and verify historical PoW
anchor reconstruction against the daemon's consensus rules, locally derive the
payout plan from validated parent-share history, and run the existing miner-tx
binding and consensus proof verification. Verify share PoW and linkage only
with the resulting trusted context. Handle reorganization explicitly.

The first 940 recorded soak shares have no archived template evidence. The new
records do not repair that gap. A fresh bootstrap test with capture enabled
from the first share will be needed to demonstrate complete independent replay;
this tool does not reset or copy either node's store.

Upstream references:
- https://github.com/hyle-team/zano/blob/1508cf6ae3ef44a52d66137d30f800b06ce917ee/src/rpc/core_rpc_server.cpp
- https://github.com/hyle-team/zano/blob/1508cf6ae3ef44a52d66137d30f800b06ce917ee/src/rpc/core_rpc_server_commands_defs.h

Tests cover malformed local-RPC results, parent mismatch, changing parent,
lookup failure, inconsistent template metadata, and height-zero underflow.

## Compare with this node's historical local observation

An optional `--local-archive OWN_ARCHIVE_DIR` argument adds a comparison with
previously captured templates from this node's own synchronized daemon. It
matches observations by both Zano height and parent hash, derives the epoch
seed independently, and checks difficulty and base reward against the local
observation. All matching local observations must agree. Conflicting local
values, missing observations, mismatched candidate values, incorrect seeds,
and parent changes remain failures. No peer-supplied difficulty or reward is
used as the expected value. This is observation-based anchoring, not a
reimplementation of historical daemon consensus.

Run this comparison on the receiving seed (VPS1), using an individual candidate
record transferred from VPS2 into a separate directory, and VPS1's own archive:

```bash
./zano-p2pool-work-audit --testnet /home/node/candidate/CONTENT_ID.work \
  http://127.0.0.1:12111/json_rpc \
  --local-archive /var/lib/zano-p2pool/testnet/shares.dat.work
```

The local archive must have been populated by this node's capture code from its
own daemon. Never substitute the sender's archive or insert sender records into
it. A checksum proves record integrity, not independent provenance; the tool
cannot authenticate the source of an operator-supplied file. Using the candidate
archive directory itself as `--local-archive` is rejected, but copying it to a
different path does not create independent evidence.

The loader checks records one at a time, retains only anchor metadata, and
fails if more than 10,000 complete `.work` records are encountered. It never
uses partial scan results. Incomplete temporary files are ignored. Malformed
records and wrong-sidechain archives fail. This runs only in the audit tool,
not in the peer message handler.

A successful comparison prints:

```text
anchor_check=matched-local-observation-untrusted
matching_local_observations=1
trusted=false
pending=payout-history,consensus-proofs
```

The count may exceed one. Exit zero still means only the diagnostic comparison
passed. Payout reconstruction, consensus proofs, and fresh parent checks at any
future trust crossing remain required. Old work for which this node has no
local observation stays unresolved; it requires independent historical
consensus reconstruction or a complete fresh captured test history.

The seed calculation follows the pinned Zano `ethash_calculate_epoch_seed`:
start with 32 zero bytes and apply Keccak-256 once for each 30,000-block epoch.
Heights outside the signed-int range supported by the existing PoW backend are
rejected. Tests include epoch boundaries, the daemon-observed epoch-6 seed,
and (when the Zano backend is enabled) comparison with its seed implementation.

Seed implementation reference:
https://github.com/hyle-team/zano/blob/1508cf6ae3ef44a52d66137d30f800b06ce917ee/contrib/ethereum/libethash/ethash.cpp
