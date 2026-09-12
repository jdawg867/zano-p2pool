# Historical payout-window validation

Historical payout validation must reconstruct the PPLNS accounting state that
existed at a candidate share's explicit parent. It must not derive historical
payouts from the node's current best tip.

This checkpoint adds branch-relative payout-window primitives and a conservative
ancestry requirement for historical payout reconstruction.

## Branch-relative PPLNS

`build_pplns_window_at_parent()` and
`build_sidechain_pplns_window_at_parent()` traverse backward from an explicit
connected parent share.

A zero parent represents empty history. A nonzero parent that is not connected
is rejected.

The existing best-tip PPLNS APIs remain wrappers around the branch-relative
functions, preserving their previous behavior.

This makes it possible to reconstruct a payout window for a stale branch after
another fork has become the selected best chain.

## Validated ancestry

`ConnectedShare::validated_ancestry` is true only when the share entered through
the checked `submit_share()` path with:

- an available ProgPoWZ validation result,
- PoW meeting the share difficulty,
- configured sidechain-difficulty enforcement, and
- either no parent, or a parent whose ancestry was already validated.

A separately PoW-validated child cannot convert an unchecked ancestor into
validated history.

Duplicate submission also does not upgrade a share that was previously inserted
through an unchecked path.

Orphans retain their validation provenance while waiting for their parent. Once
their parent arrives, ancestry becomes validated only if the entire connected
ancestry satisfies the same rule.

## Persistence recovery

The existing share store intentionally restores records with
`add_share_unchecked()`.

Therefore checksum-valid, structurally connected records recovered from disk do
not automatically acquire `validated_ancestry`.

This is deliberate. Persistence integrity is not equivalent to fresh historical
PoW, trusted-work, or consensus validation.

A future historical replay pipeline may explicitly revalidate recovered history.
Until such a pipeline exists, restored unchecked ancestry cannot be used by this
historical payout primitive as trusted payout history.

## Historical payout plan

`derive_historical_payout_plan()` requires:

- the canonical sidechain parameters used by the chain,
- the candidate share's explicit parent ID,
- independently checked parent-network difficulty, and
- independently checked block reward.

It rejects:

- sidechain-parameter mismatch,
- zero-parent/bootstrap history,
- a missing parent,
- unverified ancestry, and
- a PPLNS coinbase plan that cannot be constructed.

On success it derives a `PplnsCoinbasePlan` from the selected historical branch.

A successful plan derivation is not itself a trust crossing.

It does not:

- authenticate a peer,
- verify the candidate's miner transaction,
- verify miner-transaction consensus proofs,
- populate `P2pTrustedWorkRegistry`,
- admit a historical share,
- reconstruct missing daemon observations, or
- define the first-share bootstrap payout rule.

The caller must still bind the candidate share to the exact parent used here,
source difficulty and reward from an independently checked historical anchor,
recheck the parent-chain anchor when trust is crossed, and use the existing
miner-transaction payout/proof verification before promoting work.

## Bootstrap limitation

A candidate with a zero parent currently returns
`BootstrapHistoryRequired`.

No bootstrap payout history is invented by this checkpoint. The first-share rule
must be explicitly defined and independently validated before historical
bootstrap work can become trusted.

## Verification

The historical payout tests cover:

- bootstrap and missing-parent rejection,
- sidechain-parameter mismatch,
- unchecked ancestry rejection,
- explicit historical-parent traversal,
- PPLNS work caps,
- checked root and child payout plans,
- stale-branch reconstruction after a competing fork overtakes,
- mixed checked/unchecked ancestry,
- duplicate no-upgrade behavior, and
- orphan-promotion validation provenance.

With the real Zano ProgPoWZ backend enabled, these tests exercise the successful
checked ancestry and payout-plan paths rather than returning early through the
backend-unavailable case.
