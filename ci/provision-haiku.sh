#!/bin/bash
# Runs only on the disposable upstream Haiku guest, before candidate export.
# Base system/compiler packages come from the pinned image. Direct additions
# are versioned and content-pinned. pkgman resolves their runtime dependencies.
set -euo pipefail
test "$(uname -s)" = Haiku
test "$(uname -m)" = x86_64
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"

cat > packages.sha256 <<'PINS'
0692823b73a2cf7a41cb8e68d3b082559402739276284c567642017a0440c136 arj-3.10.22-2-x86_64.hpkg
22cffbaed05ad28f474c6dc3e4f6ad93617f4193cb0a365282bb2b30cd997e34 curl_devel-8.23.0-1-x86_64.hpkg
f4794d267036b97cafb5011614934a708a8ad7d934b00236542777e7420e984b lha-1.14i.ac20211125-1-x86_64.hpkg
ccd35cc312d27ebf4312561a98579c3fb927010604cc78376b490c794bdd3e9a libarchive_devel-3.7.9-2-x86_64.hpkg
1afc573c2682bdad5e678609158dfb1a83605b70f8979468671897bd528753dd libssh2_devel-1.11.1-1-x86_64.hpkg
f12c31901e68c923bd6ae51f76e0b4516d2ccfb880d765454dea199f873e2ff7 p7zip-17.05-2-x86_64.hpkg
2b2648e67b5cc7d6f154aedef103c472f7955311761a8bc927168bbe723f0ac3 poppler25.12_tools-25.12.0-2-x86_64.hpkg
9f1abe57e4ea0651f0daff18928e23ce9e8d52aff044a36c4ba585904452c2ca samba4_devel-4.22.0-1-x86_64.hpkg
190dab2d922573dcb426845ddf52ebe774956c54fc465948b0ab3e25629875c4 sqlite-3.53.4.0-1-x86_64.hpkg
427272dd7e631d6a72d32e9f8c001001ac7c6430de0cc3425224380958c17246 sqlite_devel-3.53.4.0-1-x86_64.hpkg
813a8bd1f934bee607936f0028d67f14c8c939da6447a351430c67ea66df7901 zlib_devel-1.3.2-1-x86_64.hpkg
PINS
repository=https://eu.hpkg.haiku-os.org/haikuports/r1beta6/x86_64/current/packages
packages=()
while read -r hash name; do
  if [[ -f /boot/system/packages/$name ]]; then
    # Validate the base's copy too. Do not ask pkgman to reinstall it.
    ln -s "/boot/system/packages/$name" "$name"
  else
    curl -fsSL --retry 2 --max-time 180 --proto '=https' --proto-redir '=https' \
      -o "$name" "$repository/$name"
    packages+=("$work/$name")
  fi
done < packages.sha256
sha256sum -c packages.sha256
if (( ${#packages[@]} )); then
  pkgman refresh
  pkgman install -y "${packages[@]}"
fi
# Ensure the solver did not silently substitute an unpinned direct addition.
(cd /boot/system/packages && sha256sum -c "$work/packages.sha256")
for command in g++ make python3 arj lha pdftoppm; do
  command -v "$command"
done
