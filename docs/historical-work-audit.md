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
