#!/bin/sh
# SPDX-License-Identifier: MIT -- Copyright (c) 2026 DezPanda
# Builds "Get Artwork" for Windows, Mac (Apple and Intel) and Linux into dist/Get Artwork/.
# Needs Go (1.21 or newer):  GOROOT=... sh build.sh      Output is built in /tmp first (exFAT drives are slow for this).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT="$HERE/dist/Get Artwork"
TMP=${TMPDIR:-/tmp}/g36-artwork-build
rm -rf "$TMP" "$OUT"
mkdir -p "$TMP/bin" "$OUT/bin"
cd "$HERE"
LD="-s -w"
GOOS=windows GOARCH=amd64 go build -trimpath -ldflags "$LD" -o "$TMP/Get Artwork - Windows.exe" .
GOOS=darwin  GOARCH=arm64 go build -trimpath -ldflags "$LD" -o "$TMP/bin/g36-artwork-mac-arm64" .
GOOS=darwin  GOARCH=amd64 go build -trimpath -ldflags "$LD" -o "$TMP/bin/g36-artwork-mac-intel" .
GOOS=linux   GOARCH=amd64 go build -trimpath -ldflags "$LD" -o "$TMP/bin/g36-artwork-linux" .
cp "$TMP/Get Artwork - Windows.exe" "$OUT/"
cp "$TMP/bin/"* "$OUT/bin/"
cp "$HERE/launchers/Get Artwork - Mac.command" "$HERE/launchers/Read me.txt" "$OUT/"
chmod 755 "$OUT/Get Artwork - Mac.command" "$OUT/bin/"*
find "$OUT" -name '._*' -delete      # macOS adds these on exFAT; they must not end up on a card
ls -l "$OUT" "$OUT/bin"
