#!/usr/bin/env bash

set -euo pipefail

if [[ "$#" -ne 2 ]]; then
  echo "Usage: $0 ARCHIVE CHECKSUM_FILE" >&2
  exit 2
fi

archive="$(realpath "$1")"
checksum_file="$(realpath "$2")"
package="$(basename "$archive" .tar.gz)"
workspace="$(mktemp -d)"
trap 'rm -rf "$workspace"' EXIT

if [[ "$(basename "$checksum_file")" != "$(basename "$archive").sha256" ]]; then
  echo "Checksum filename does not match archive" >&2
  exit 1
fi

(
  cd "$(dirname "$archive")"
  sha256sum --check "$(basename "$checksum_file")"
)

while IFS= read -r member; do
  if [[ "$member" == /* || "$member" == *"/../"* || "$member" == ../* ]]; then
    echo "Unsafe archive member: $member" >&2
    exit 1
  fi
  if [[ "$member" != "$package" && "$member" != "$package/"* ]]; then
    echo "Archive member is outside the package root: $member" >&2
    exit 1
  fi
done < <(tar -tzf "$archive")

tar -xzf "$archive" -C "$workspace"
root="$workspace/$package"

required_files=(
  "BUILD-INFO.txt"
  "DEPENDENCIES.txt"
  "bin/zano-p2pool"
  "bin/zano-p2pool-header"
  "share/doc/zano-p2pool/README.md"
  "share/doc/zano-p2pool/operator-deployment.md"
  "share/doc/zano-p2pool/p2p-protocol.md"
  "share/doc/zano-p2pool/progpowz.md"
  "share/zano-p2pool/systemd/zano-p2pool.env.example"
  "share/zano-p2pool/systemd/zano-p2pool.service"
)

for path in "${required_files[@]}"; do
  if [[ ! -f "$root/$path" ]]; then
    echo "Release archive is missing $path" >&2
    exit 1
  fi
done

test -x "$root/bin/zano-p2pool"
test -x "$root/bin/zano-p2pool-header"

help_output="$("$root/bin/zano-p2pool" --help)"

grep -Fq 'testnet default seeds: none' <<<"$help_output"
grep -Fq 'mainnet default seeds: none' <<<"$help_output"

retired_seed_pattern='zano-pool\.ddns\.net|zano-pool2\.ddns\.net|45\.77\.77\.93|68\.232\.175\.242'

if grep -aEq "$retired_seed_pattern" "$root/bin/zano-p2pool"; then
  echo "Packaged binary contains a retired testnet seed endpoint" >&2
  exit 1
fi

if grep -q "not found" "$root/DEPENDENCIES.txt"; then
  echo "Release dependency report contains an unresolved library" >&2
  exit 1
fi
if grep -q 'libboost_serialization' "$root/DEPENDENCIES.txt"; then
  echo "Release dependency report contains dynamic Boost.Serialization" >&2
  exit 1
fi

env_file="$root/share/zano-p2pool/systemd/zano-p2pool.env.example"
unit_file="$root/share/zano-p2pool/systemd/zano-p2pool.service"
grep -qx 'ZANO_P2POOL_STRATUM_BIND=127.0.0.1' "$env_file"
grep -qx 'ZANO_P2POOL_P2P_BIND=127.0.0.1' "$env_file"
grep -qx 'ZANO_P2POOL_METRICS_BIND=127.0.0.1' "$env_file"
grep -q 'REPLACE_WITH_A_STANDARD_TESTNET_ZANO_ADDRESS' "$env_file"
packaged_operator_docs=(
  "$root/share/doc/zano-p2pool/README.md"
  "$root/share/doc/zano-p2pool/operator-deployment.md"
)

if grep -Eq \
  'zano-pool\.ddns\.net:37888|zano-pool2\.ddns\.net:37888|45\.77\.77\.93:37888|68\.232\.175\.242:37888' \
  "${packaged_operator_docs[@]}"; then
  echo "Packaged operator documentation contains a retired testnet seed endpoint" >&2
  grep -nE \
    'zano-pool\.ddns\.net:37888|zano-pool2\.ddns\.net:37888|45\.77\.77\.93:37888|68\.232\.175\.242:37888' \
    "${packaged_operator_docs[@]}" >&2 || true
  exit 1
fi

install_root="$workspace/install-root"
install -D -m 0755 \
  "$root/bin/zano-p2pool" \
  "$install_root/usr/local/bin/zano-p2pool"
install -D -m 0640 \
  "$env_file" \
  "$install_root/etc/zano-p2pool/zano-p2pool.env"
install -D -m 0644 \
  "$unit_file" \
  "$install_root/etc/systemd/system/zano-p2pool.service"

test "$(stat -c '%a' "$install_root/usr/local/bin/zano-p2pool")" = 755
test "$(stat -c '%a' "$install_root/etc/zano-p2pool/zano-p2pool.env")" = 640
test "$(stat -c '%a' "$install_root/etc/systemd/system/zano-p2pool.service")" = 644

# Parse the packaged unit after pointing ExecStart at the staged executable.
# systemd-analyze otherwise reports the intentionally absent production path.
sed -i \
  "s#/usr/local/bin/zano-p2pool#$install_root/usr/local/bin/zano-p2pool#" \
  "$install_root/etc/systemd/system/zano-p2pool.service"
sed -i \
  "s#User=zano-p2pool#User=$(id -un)#; s#Group=zano-p2pool#Group=$(id -gn)#" \
  "$install_root/etc/systemd/system/zano-p2pool.service"
systemd-analyze verify "$install_root/etc/systemd/system/zano-p2pool.service"

echo "Release archive installation smoke test passed: $package"
