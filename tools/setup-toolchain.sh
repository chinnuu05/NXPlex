#!/bin/bash
# devkitA64 r30 (GCC 16) needs glibc >= 2.36; Ubuntu 22.04 ships 2.35.
# This builds a symlink copy of /opt/devkitpro/devkitA64 under
# ~/.local/share/nxplex-toolchain in which only the binaries that need the
# newer glibc are copied and patched to use one from Nix. /opt is untouched.
#
# Then build with:  DEVKITA64=~/.local/share/nxplex-toolchain/devkitA64 make
set -euo pipefail

NIXF="--extra-experimental-features nix-command --extra-experimental-features flakes"
GLIBC=$(nix $NIXF build --no-link --print-out-paths nixpkgs#glibc.out | tail -1)
PATCHELF=$(nix $NIXF build --no-link --print-out-paths nixpkgs#patchelf | tail -1)/bin/patchelf
SRC=${DEVKITPRO:-/opt/devkitpro}/devkitA64
DST=$HOME/.local/share/nxplex-toolchain/devkitA64

mkdir -p "$(dirname "$DST")"
[ -e "$DST" ] || cp -as "$SRC" "$DST"
cd "$DST"
n=0
while IFS= read -r f; do
    real=$(readlink "$f")
    file -L "$real" | grep -q "ELF 64-bit LSB.*x86-64.*dynamically" || continue
    strings "$real" | grep -q -E "GLIBC_2\.3[6-9]|GLIBC_2\.4" || continue
    rm "$f"
    cp "$real" "$f"
    chmod u+w "$f"
    "$PATCHELF" --set-interpreter "$GLIBC/lib/ld-linux-x86-64.so.2" --set-rpath "$GLIBC/lib" "$f"
    n=$((n + 1))
done < <(find . -type l)
echo "patched $n binaries"
"$DST/bin/aarch64-none-elf-g++" --version | head -1
