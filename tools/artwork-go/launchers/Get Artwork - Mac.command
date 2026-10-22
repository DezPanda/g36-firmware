#!/bin/sh
# Double-click this on a Mac. (If it says it can't be opened: right-click it, Open.)
cd "$(dirname "$0")" || exit 1
case "$(uname -m)" in
  arm64) PROGRAM=bin/g36-artwork-mac-arm64 ;;
  *)     PROGRAM=bin/g36-artwork-mac-intel ;;
esac
chmod +x "$PROGRAM" 2>/dev/null
exec "./$PROGRAM" "$@"
