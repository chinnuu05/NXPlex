#!/bin/bash
# Regenerates core/cacerts.inc: the root CAs that plex.tv, clients.plex.tv,
# *.plex.direct and Plex relays chain to, taken from the system CA store.
set -euo pipefail
OUT=$(dirname "$0")/../core/cacerts.inc
CERTS=/etc/ssl/certs
ROOTS="DigiCert_Global_Root_G2 DigiCert_Global_Root_G3 ISRG_Root_X1 ISRG_Root_X2
USERTrust_RSA_Certification_Authority USERTrust_ECC_Certification_Authority
Amazon_Root_CA_1 GTS_Root_R1 GTS_Root_R4"
{
    echo "// Generated from the Mozilla CA bundle: roots that plex.tv, clients.plex.tv,"
    echo "// *.plex.direct and Plex relays chain to. Regenerate with tools/gen_cacerts.sh."
    echo "static const char kCaBundle[] ="
    for r in $ROOTS; do
        echo "    // $r"
        sed 's/^/    "/; s/$/\\n"/' "$CERTS/$r.pem"
    done
    echo "    ;"
} > "$OUT"
echo "wrote $OUT ($(grep -c BEGIN "$OUT") roots)"
