#!/bin/sh
# Copy the ARM build and sample assets to the RK3506 board and run the smoke
# test there.
#
#   ./scripts/board_test.sh            # ALSA "null" device (safe, no sound)
#   ./scripts/board_test.sh --alsa     # also play 2 s through plughw:0,0
#
# BOARD / KEY can be overridden from the environment.
set -eu

BOARD=${BOARD:-hzhy@192.168.101.12}
KEY=${KEY:-$HOME/.ssh/board_ed25519}
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN=${BIN:-$HOME/espaudiocore-linux/build-arm/eac_play}
ASSETS=${ASSETS:-$ROOT/tests/assets}
DEVICE=${DEVICE:-null}

[ -x "$BIN" ] || { echo "missing ARM binary: $BIN (run scripts/build.sh --cross)" >&2; exit 2; }
[ -d "$ASSETS" ] || { echo "missing assets: $ASSETS (run tests/gen_assets.sh)" >&2; exit 2; }

echo "== upload =="
ssh -i "$KEY" "$BOARD" "mkdir -p /tmp/eac-test"
scp -q -i "$KEY" "$BIN" "$BOARD:/tmp/eac-test/eac_play"
scp -q -i "$KEY" "$ASSETS"/*.wav "$ASSETS"/*.mp3 "$ASSETS"/*.aac "$ASSETS"/*.m4a \
    "$ASSETS"/*.flac "$ASSETS"/*.ogg "$ASSETS"/*.opus "$ASSETS"/*.wv \
    "$BOARD:/tmp/eac-test/" 2>/dev/null || true

rc=0
for f in t.wav t24.wav t.mp3 t.aac t.m4a t.flac t24.flac t.ogg t.opus t.wv; do
    [ -f "$ASSETS/$f" ] || continue
    printf '== %s (%s) ==\n' "$f" "$DEVICE"
    if ssh -i "$KEY" "$BOARD" "chmod +x /tmp/eac-test/eac_play; timeout 30 /tmp/eac-test/eac_play /tmp/eac-test/$f --device $DEVICE --quiet"; then
        echo "   OK"
    else
        echo "   FAILED"
        rc=1
    fi
done

if [ "${1:-}" = "--alsa" ]; then
    echo "== plughw:0,0 (2 s, root) =="
    ssh -i "$KEY" "$BOARD" "sudo -n timeout 10 /tmp/eac-test/eac_play /tmp/eac-test/t.wav --device plughw:0,0 --seconds 2" || rc=1
    echo "== plughw:0,0 full EOS (root) =="
    ssh -i "$KEY" "$BOARD" "sudo -n timeout 20 /tmp/eac-test/eac_play /tmp/eac-test/t.flac --device plughw:0,0" || rc=1
fi

exit $rc
