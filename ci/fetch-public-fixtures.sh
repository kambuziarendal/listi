#!/usr/bin/env bash
# Both inputs must be public, immutable and exclusively for disposable guests.
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
fetch "${IMAGE_URL:-}" "${IMAGE_SHA256:-}" "$output/haiku.qcow2"
fetch "${IDENTITY_URL:-}" "${IDENTITY_SHA256:-}" "$output/guest-identity"
