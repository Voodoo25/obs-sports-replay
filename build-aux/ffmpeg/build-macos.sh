#!/usr/bin/env bash
# Builds the plugin's static FFmpeg for macOS as universal archives: one build
# per architecture with build.sh, merged with lipo.
#
# Usage: build-macos.sh <prefix>

set -euo pipefail

PREFIX=${1:?usage: build-macos.sh <prefix>}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$PREFIX"
PREFIX=$(cd "$PREFIX" && pwd)

for arch in arm64 x86_64; do
	"$HERE/build.sh" "$PREFIX/.arch-$arch" "$arch"
done

rm -rf "$PREFIX/include" "$PREFIX/lib"
mkdir -p "$PREFIX/lib/pkgconfig"
cp -R "$PREFIX/.arch-arm64/include" "$PREFIX/include"

for lib in "$PREFIX"/.arch-arm64/lib/*.a; do
	name=$(basename "$lib")
	lipo -create "$lib" "$PREFIX/.arch-x86_64/lib/$name" -output "$PREFIX/lib/$name"
done

# the .pc files only matter for their Libs.private, the same on both
for pc in "$PREFIX"/.arch-arm64/lib/pkgconfig/*.pc; do
	sed "s|$PREFIX/.arch-arm64|$PREFIX|g" "$pc" >"$PREFIX/lib/pkgconfig/$(basename "$pc")"
done

echo "universal FFmpeg installed to $PREFIX"
