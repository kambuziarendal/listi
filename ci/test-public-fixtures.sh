#!/usr/bin/env bash
# Exercise the fetch gate with a local curl stand-in, never a network service.
set -euo pipefail
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
mkdir "$scratch/bin" "$scratch/out"
cat > "$scratch/bin/curl" <<'MOCK'
#!/usr/bin/env bash
set -eu
while [[ "$1" != --output ]]; do shift; done
printf '%s\n' "$3" >> "$REQUESTS"
printf fixture > "$2"
MOCK
chmod +x "$scratch/bin/curl"
export PATH="$scratch/bin:$PATH" REQUESTS="$scratch/requests"
unset IMAGE_URL IMAGE_SHA256 IDENTITY_URL IDENTITY_SHA256
run_fetch() { bash ci/fetch-public-fixtures.sh "$scratch/out"; }
reject() {
  if run_fetch > "$scratch/log" 2>&1; then
    echo "Unexpectedly accepted $1" >&2
    exit 1
  fi
}

reject "incorrect public asset bytes"
grep -Fx 'https://github.com/anyvm-org/haiku-builder/releases/download/v2.0.3/haiku-r1beta6.qcow2.zst' "$REQUESTS"
test ! -e "$scratch/out/provision-public-fixture"
: > "$REQUESTS"
export IMAGE_URL=https://example.invalid/image
reject "partial fixture override"
test ! -s "$REQUESTS"
hash=$(printf fixture | sha256sum | cut -d' ' -f1)
export IMAGE_SHA256="$hash" IDENTITY_SHA256="$hash" IDENTITY_URL=http://example.invalid/identity
reject "non-HTTPS identity"
test ! -e "$scratch/out/guest-identity"
export IDENTITY_URL=https://example.invalid/identity
run_fetch
test -s "$scratch/out/guest-identity"
test ! -e "$scratch/out/provision-public-fixture"
echo "PASS public pin, checksum rejection, override completeness, HTTPS and prepared-fixture checks"
