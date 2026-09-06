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

There are currently no default seed nodes. To connect to a temporary test peer,
create a systemd override that reproduces `ExecStart` from the installed unit
and appends one or more `--p2p-peer HOST:PORT` arguments:

```bash
sudo systemctl edit zano-p2pool
```

Systemd requires `ExecStart=` to be cleared before replacing it in an override.
Run `systemctl cat zano-p2pool` afterward to review the effective command. Do
not add a test endpoint to the distributed unit or environment template.

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

Before an upgrade, back up `/var/lib/zano-p2pool/shares.dat` while the service
is stopped. Never replace a store with one from a different `SidechainId`.

## Firewall surface

| Service | Default bind | Expose publicly? |
|---|---|---|
| `zanod` JSON-RPC | `127.0.0.1:12111` on testnet | No |
| miner Stratum | `127.0.0.1:3333` | Only when remote miners require it |
| zano-p2pool P2P | `127.0.0.1:37888` | Only for an intentional peer listener |
| metrics/health | `127.0.0.1:37890` | No; use a protected monitoring path |

SSH access and host hardening remain operator responsibilities. Open firewall
ports only after confirming the effective listener with `ss -lntp`.
