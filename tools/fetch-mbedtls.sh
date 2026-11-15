#!/bin/bash
# Downloads mbedTLS 2.28.10 (the version devkitPro ships as switch-mbedtls)
# into host/third_party/mbedtls for the PC build, verifying its checksum.
set -euo pipefail
VERSION=2.28.10
SHA256=0f2e0525903a89ae1d39ce439d858be66933bda54c5b6102b72a29ed8fe7c088
DEST=$(cd "$(dirname "$0")/.." && pwd)/host/third_party
[ -f "$DEST/mbedtls/include/mbedtls/ssl.h" ] && exit 0
mkdir -p "$DEST"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
curl -fsSL -o "$TMP/mbedtls.tgz" "https://github.com/Mbed-TLS/mbedtls/archive/refs/tags/v$VERSION.tar.gz"
echo "$SHA256  $TMP/mbedtls.tgz" | sha256sum -c --quiet
tar -xzf "$TMP/mbedtls.tgz" -C "$TMP"
mv "$TMP/mbedtls-$VERSION" "$DEST/mbedtls"
echo "mbedTLS $VERSION -> $DEST/mbedtls"
