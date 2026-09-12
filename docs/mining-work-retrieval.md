# Mining work retrieval

Peers with a local mining-work archive advertise capability bit 3. When share
validation reports unknown work, the node can request the corresponding archived
proposal by Zano height and mining-header hash. Legacy peers receive no requests.

Message types 7 and 8 carry requests and responses. A request contains an
8-byte height, 32-byte header hash and 4-byte chunk offset (integers big-endian).
A response repeats the request, adds a 4-byte total length and the chunk bytes.
Total length zero means not found. Otherwise proposals are at most 64 KiB,
transferred sequentially in chunks of at most 16 KiB. Offsets must be aligned,
responses must match a pending peer and work key, and the declared total cannot
change mid-transfer. Truncated, unsolicited and out-of-order responses fail.

Requests expire after 60 seconds. There are at most 16 pending entries globally
and two per peer. Completed or failed requests retain their entry until expiry
to limit repeated requests. The receiver retains at most eight completed
proposals in an in-memory untrusted cache. The local archive index grows with
archived work keys; received proposals are not added to that index or to disk.

The receiver parses the assembled proposal and derives its mining header to
check the requested key. This is only structural validation. It does not prove
historical canonical-chain membership, payout correctness, or valid transaction
proofs. Retrieval never inserts trusted work or admits shares. The runtime logs
`untrusted; historical validation pending` when retrieval completes. A future
historical validator must establish trust before using this evidence.

Both peers need this capability for retrieval. Only proposals captured by the
previous mining-work-archive feature are available: older shares without saved
templates remain unavailable. This checkpoint does not resolve seed bootstrap
or historical share synchronization by itself.

Tests cover maximum-size transfer, malformed and mismatched replies, capability
negotiation, expiry and pending limits, and a real two-node socket transfer that
leaves the receiving trusted-work registry and share chain empty. Protocol fuzz
seeds include both new message types.
