#!/bin/sh
# Build port/nanoapps/build/nanocdj.hbapp (then package it with package.py).
# make can't cope with spaces in paths, so this builds through a temporary
# space-free symlink to the repository. Extra arguments go to make (e.g. clean).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
NANOAPPS_DIR="${NANOAPPS:-$ROOT/third_party/NanoApps}"
if [ ! -f "$NANOAPPS_DIR/sdk/hb_app.mk" ]; then
    echo "NanoApps SDK not found at $NANOAPPS_DIR" >&2
    echo "run: git submodule update --init   (or set NANOAPPS=/path/to/NanoApps)" >&2
    exit 1
fi
LINK="${TMPDIR:-/tmp}/nanocdj-build-$$"
LINK_SDK="${TMPDIR:-/tmp}/nanocdj-sdk-$$"
rm -f "$LINK" "$LINK_SDK"
ln -s "$ROOT" "$LINK"
ln -s "$NANOAPPS_DIR" "$LINK_SDK"
trap 'rm -f "$LINK" "$LINK_SDK"' EXIT
make -C "$LINK/port/nanoapps" NANOAPPS="$LINK_SDK" "$@"
