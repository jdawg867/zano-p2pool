# Mining work evidence capture

With share persistence enabled, the node creates `<share-store>.work/`. For
example, the soak node uses `/var/lib/zano-p2pool-soak/shares.dat.work/`.
`--no-share-store` disables both share persistence and work capture.

The final template (including the replacement PPLNS miner transaction when
present) is serialized as a P2pMiningContextProposal and archived **before** it
is installed as local peer context or published to Stratum. Bootstrap templates
are captured too. The payload includes the exact block template, miner_tx_tgc
JSON and the proposal's network/reward metadata. Older job evidence is retained;
share header hashes do not suffice to recreate these payloads.

## Record format

Each `<context-id>.work` file contains:

- 8 bytes magic `ZP2WORK1`;
- 32 bytes canonical SidechainId;
- 4 bytes big-endian payload length;
- the serialized mining-context payload (existing P2P size bounds apply);
- 32 bytes cn_fast_hash of all preceding record bytes.

The context ID is cn_fast_hash(payload), the same ID used for mining-context
proposals. Identical payloads deduplicate. Records are written to exclusive
0600 temporary files, fsynced, atomically published with a no-clobber hard link,
and followed by a directory fsync. A local filesystem supporting these POSIX
operations is required. Startup verifies complete records individually without
retaining all payloads in RAM. Unpublished `.tmp-*` remnants are ignored; complete
corrupt, truncated, misnamed or wrong-sidechain records cause startup failure.
An archive write failure sets persistence unhealthy and stops the runtime before
publishing the new work. Earlier already-issued work may finish concurrently.

No records are automatically pruned. Disk usage grows with distinct issued
proposals; preserve the archive with the corresponding share store. Empty-run
archives can contain templates that produced no accepted shares.

## Trust boundary and rollout

This is evidence capture, not a fresh-node synchronization fix. Archive bytes
are not promoted to trusted work on startup. Checksums detect corruption, not
malicious local modification. Retrieval still needs independent historical Zano
anchoring, proof verification, and payout validation against verified ancestry.
Peer-received templates are not archived by this capture-only change.

Existing shares.dat files are neither rewritten nor imported into the archive.
The recorded 940-share test history has 891 work headers but no template payloads;
starting capture now does not fill that historical gap. Do not copy another
node's share store into a fresh seed as a substitute for verification.

Validation: standalone archive tests cover reopen/dedup, concurrent publication,
size bounds, private permissions, truncated records, corrupted metadata/payload,
wrong sidechain and wrong filename. The mining-context test additionally checks
an exact serialized proposal round-trip. Before deployment, run the full
Zano-enabled CTest suite; capture has no changes to peer trust or share admission.
