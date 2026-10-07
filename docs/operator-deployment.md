# Linux operator deployment

This guide installs a released zano-p2pool binary as a hardened systemd
service on Ubuntu or Debian. It does not install or configure `zanod`, which
must run separately and expose JSON-RPC only on a trusted local interface.

zano-p2pool remains experimental. Use testnet until the project explicitly
announces mainnet readiness.

## 1. Verify and unpack a release

Download the Linux x86-64 archive and its `.sha256` file from the same GitHub
release, then verify before extracting:

```bash
sha256sum --check zano-p2pool-vVERSION-linux-x86_64.tar.gz.sha256
tar -xzf zano-p2pool-vVERSION-linux-x86_64.tar.gz
cd zano-p2pool-vVERSION-linux-x86_64
```

The archive includes `BUILD-INFO.txt` and `DEPENDENCIES.txt` so the audited Zano
commit, build inputs, and remaining system-library dependencies are visible.

## 2. Install the service files

Create a dedicated unprivileged account and install the binary, configuration,
and unit:

```bash
sudo useradd \
  --system \
  --user-group \
  --home-dir /var/lib/zano-p2pool \
  --shell /usr/sbin/nologin \
  zano-p2pool

sudo install -m 0755 bin/zano-p2pool /usr/local/bin/zano-p2pool
sudo install -d -m 0750 -o root -g zano-p2pool /etc/zano-p2pool
sudo install -m 0640 -o root -g zano-p2pool \
  share/zano-p2pool/systemd/zano-p2pool.env.example \
  /etc/zano-p2pool/zano-p2pool.env
sudo install -m 0644 \
  share/zano-p2pool/systemd/zano-p2pool.service \
  /etc/systemd/system/zano-p2pool.service
```

If the service account already exists, `useradd` will report that fact and can
be skipped. Systemd creates `/var/lib/zano-p2pool` with mode `0700` when the
service starts.

## 3. Configure the node

Edit `/etc/zano-p2pool/zano-p2pool.env` and replace the placeholder with a
standard public testnet Zano payout address:

```bash
sudoedit /etc/zano-p2pool/zano-p2pool.env
```

The supplied defaults keep Stratum, P2P, metrics, and `zanod` RPC on loopback.
Do not expose the daemon RPC or metrics endpoint publicly. To accept remote
miners or peers, change only the corresponding bind address and allow only the
required TCP port through the host and provider firewalls.

Both testnet and mainnet currently have empty built-in P2P seed lists. The
former beta.2 public testnet seed VPSs were retired after their validation role
completed, and mainnet remains intentionally empty until dedicated seed
infrastructure is deployed and validated.

Operators should add one or more explicit `--p2p-peer HOST:PORT` entries when
bootstrap peers are required. Explicit peers are de-duplicated. Use
`--no-seed-nodes` to guarantee explicit-peer-only operation if built-in defaults
are added again in a future release.

Persistent command-line changes under systemd should be made with
`systemctl edit zano-p2pool`. Clear the existing `ExecStart=` before replacing
it, then inspect the result with `systemctl cat zano-p2pool`.

## 4. Start and verify

Confirm that the local `zanod` RPC endpoint is listening, validate the unit,
then start the node:

```bash
sudo ss -lntp | grep ':12111'
sudo systemd-analyze verify /etc/systemd/system/zano-p2pool.service
sudo systemctl daemon-reload
sudo systemctl enable --now zano-p2pool
sudo systemctl status zano-p2pool --no-pager -l
sudo journalctl -u zano-p2pool -n 50 --no-pager
curl -fsS http://127.0.0.1:37890/healthz
```

A healthy endpoint returns `ok`. Inspect `/metrics` locally and confirm
`zano_p2pool_persistence_ok 1`. During initial daemon synchronization, RPC work
may be temporarily unavailable; the long-running node retries with bounded
backoff.

### Mainnet launch health gates

Do not enable public mining merely because the process is running. During the
initial mainnet rollout, a node passes the launch health gate only when all
applicable checks below pass.

For every seed or mining node:

- `GET /healthz` returns `ok`;
- `zano_p2pool_up` equals `1`;
- `zano_p2pool_persistence_ok` equals `1`;
- the local Zano daemon reports an online/synchronized mainnet state;
- `zano_p2pool_zano_height` is no more than one parent block behind the local
  daemon's current mainnet height after startup synchronization;
- `zano_p2pool_template_refresh_failures_total` does not increase during the
  bounded pre-mining observation window;
- the process is not crash-looping or accumulating unexpected systemd restarts.

For the two mainnet seed nodes after both are online:

- each seed has at least one P2P peer, normally including the other seed;
- neither seed remains at zero peers for more than five minutes after DNS,
  public TCP reachability, and explicit inter-seed wiring have been verified;
- if the sidechain is still empty during the P2P-only soak,
  `zano_p2pool_sidechain_tip_height 0` is valid and must not by itself be
  treated as a failure.

After the mining canary begins producing accepted shares:

- `zano_p2pool_sidechain_connected_shares` becomes non-zero;
- `zano_p2pool_sidechain_tip_height` advances while shares are being accepted;
- after pausing the canary long enough for propagation, the seed nodes converge
  on the same canonical sidechain tip;
- `zano_p2pool_block_submission_failures_total` must not increase;
- if a full-network-difficulty block candidate occurs, its submission outcome
  must be accounted for as an accepted main-chain block or an accepted
  alternative-chain block. The absence of a block candidate during a bounded
  canary test is not itself a failure.

Metrics counters are process-local, so compare counter deltas within the same
process lifetime rather than comparing raw counter values across restarts.

## 5. Upgrade and roll back

Keep the prior binary until the replacement is healthy:

```bash
sudo systemctl stop zano-p2pool
sudo cp -a /usr/local/bin/zano-p2pool /usr/local/bin/zano-p2pool.previous
sudo install -m 0755 bin/zano-p2pool /usr/local/bin/zano-p2pool
sudo systemctl start zano-p2pool
curl -fsS http://127.0.0.1:37890/healthz
```

If validation fails, restore it without deleting the share store:

```bash
sudo systemctl stop zano-p2pool
sudo install -m 0755 \
  /usr/local/bin/zano-p2pool.previous \
  /usr/local/bin/zano-p2pool
sudo systemctl start zano-p2pool
```

Before an upgrade, back up the complete durable P2Pool recovery set while
`zano-p2pool` is stopped:

- `shares.dat` — canonical share-store records;
- `shares.dat.validation` — replay-validation cache associated with that store;
- `shares.dat.work/` — durable mining-work archive associated with that store;
- the active `zano-p2pool` binary;
- the previous known-good binary;
- the active systemd unit and any environment/configuration files used by it.

Treat these files as one recovery generation. Never combine `shares.dat` with
validation/work state from a different backup generation and never replace a
store with one from a different `SidechainId`.

### Mainnet backup and rollback policy

For mainnet operation:

1. Create a quiesced backup before every binary, unit, environment, firewall,
   seed-policy, or consensus-affecting configuration change.
2. During the initial mainnet soak, create one quiesced backup at least once per
   day and retain the most recent seven daily recovery generations.
3. Retain every pre-upgrade recovery generation until the replacement has
   completed its bounded soak and has passed restart/recovery validation.
4. Keep at least one verified recovery copy off the node being protected.
5. Record a SHA-256 manifest for every recovery generation and verify the
   archive after copying it off-host.
6. A restore drill must stop P2Pool, verify the archive manifest, restore the
   complete matching recovery generation, verify ownership/permissions, start
   the previous known-good binary, and confirm health, persistence, sidechain
   identity, daemon synchronization, and P2P state before miners are enabled.
7. Roll back immediately if the new runtime cannot maintain daemon/template
   synchronization, persistence health, canonical SidechainId, expected P2P
   state, or safe restart recovery. Do not delete the share store merely because
   a binary rollback is required.

The Zano blockchain database is not part of the P2Pool recovery generation.
It may be independently rebuilt or restored according to the Zano daemon
operator policy.

### Mainnet rollback triggers and procedure

Pause miner access immediately while investigating any of these conditions:

- `zano_p2pool_persistence_ok` changes to `0`;
- startup reports a wrong-network or wrong-SidechainId persistence boundary;
- restart history recovery reports an unrecoverable/fatal condition;
- P2Pool remains more than one parent block behind a healthy synchronized local
  Zano daemon for more than five minutes;
- template refresh remains continuously failed for more than two minutes;
- a mainnet seed remains unable to establish expected P2P connectivity for more
  than five minutes after DNS and TCP reachability are independently confirmed;
- the seed nodes fail to converge on canonical sidechain state after share
  production is paused for a propagation window;
- `zano_p2pool_block_submission_failures_total` increases during the mining
  canary;
- the service enters a crash/restart loop, defined for launch purposes as three
  or more unexpected restarts within ten minutes.

A trigger requires rollback when evidence points to the newly deployed P2Pool
binary, service configuration, or accompanying persistence generation. If the
root cause is clearly external, such as an upstream daemon outage or network
partition, keep mining disabled but do not replace a known-good binary merely
to mask the external failure.

Rollback procedure:

1. Disable Stratum/public miner access before changing runtime state.
2. Capture the failing node's current logs, `/metrics`, service status, binary
   checksum, unit/environment configuration, and a quiesced recovery archive
   where safe to do so.
3. Stop `zano-p2pool`.
4. Restore the previous known-good binary and matching service configuration.
5. Restore an older persistence generation only if the current durable state is
   itself incompatible or damaged; when restoration is required, restore the
   complete matching `shares.dat`, `.validation`, and `.work/` generation.
6. Verify ownership, permissions, binary checksum, unit contents, and the
   canonical mainnet SidechainId before restart.
7. Start P2Pool with Stratum still disabled.
8. Re-run the mainnet launch health gates and a bounded P2P-only observation.
9. Re-enable mining only after the rollback state passes those checks.

Never solve a failed launch by deleting the share store and silently starting a
new sidechain history.

### Mainnet activation-guard policy

The initial public mainnet release, mainnet seed deployment, P2P-only soak, and
first mining canary must continue to require the explicit
`--experimental-mainnet` operator opt-in.

Do not remove the guard merely because HF7 compatibility or the first template
audit passed. Removal or replacement is a separate post-canary release decision
and requires all of the following first:

- permanent mainnet seed infrastructure is deployed and independently
  reachable;
- the bounded P2P-only soak passes;
- the controlled restart/recovery drill passes;
- one mining canary passes the launch health and block-submission gates;
- the release archive and metadata are independently verified;
- the readiness checklist contains no unresolved safety-critical launch gate.

Any later guard removal must be made in a separately reviewed commit with its
CLI regression tests updated deliberately and with the change called out in the
release notes.

## Firewall surface

| Service | Default bind | Expose publicly? |
|---|---|---|
| `zanod` JSON-RPC | `127.0.0.1:12111` on testnet | No |
| miner Stratum | `127.0.0.1:3333` | Only when remote miners require it |
| zano-p2pool P2P | `127.0.0.1:37888` | Only for an intentional peer listener |
| metrics/health | `127.0.0.1:37890` | No; use a protected monitoring path |

SSH access and host hardening remain operator responsibilities. Open firewall
ports only after confirming the effective listener with `ss -lntp`.
