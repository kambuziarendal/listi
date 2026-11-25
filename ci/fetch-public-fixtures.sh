#!/usr/bin/env bash
# All inputs are public fixtures exclusively for disposable CI guests.
set -euo pipefail
umask 077
output=${1:?supply the runner temporary directory}
fetch() {
  local url=$1 hash=$2 file=$3
  [[ "$url" == https://* && "$hash" =~ ^[a-f0-9]{64}$ ]] || {
    echo "Missing HTTPS fixture URL or SHA-256 for ${file##*/}; native qualification incomplete." >&2
    return 1
  }
  curl --fail --silent --show-error --location --proto '=https' --proto-redir '=https' \
    --retry 2 --max-time 900 --output "$file" "$url"
  printf '%s  %s\n' "$hash" "$file" | sha256sum --check
}

if [[ -n "${IMAGE_URL:-}${IMAGE_SHA256:-}${IDENTITY_URL:-}${IDENTITY_SHA256:-}" ]]; then
  # Never mix a public key with a partially configured custom base.
  if [[ -z "${IMAGE_URL:-}" || -z "${IMAGE_SHA256:-}" || -z "${IDENTITY_URL:-}" || -z "${IDENTITY_SHA256:-}" ]]; then
    echo "Override all four fixture variables together, or leave all four empty." >&2
    exit 1
  fi
  echo 'Using configured pre-provisioned fixture.'
  fetch "$IMAGE_URL" "$IMAGE_SHA256" "$output/haiku.qcow2"
  fetch "$IDENTITY_URL" "$IDENTITY_SHA256" "$output/guest-identity"
else
  echo 'Using pinned public AnyVM Haiku R1/beta6 x86_64 CI image (v2.0.3).'
  base=https://github.com/anyvm-org/haiku-builder/releases/download/v2.0.3/haiku-r1beta6
  fetch "$base.qcow2.zst" df8be357c5d3795da80dbec0b56d1d4e50f182b2ddf5e272f657ec4b54a9b792 "$output/haiku.qcow2.zst"
  fetch "$base-host.id_rsa" 56d8a666bb7a77fbdf493893a82fde3d17e88e0d99ea02071005e37c796d86e0 "$output/guest-identity"
  zstd --decompress --sparse --rm "$output/haiku.qcow2.zst" -o "$output/haiku.qcow2"
  touch "$output/provision-public-fixture"
fi
