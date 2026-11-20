#!/bin/sh
# Decode every sample in tests/assets with the given eac_play binary.
#
#   ./tests/smoke.sh /path/to/eac_play [assets_dir] [alsa_device]
#
# Default ALSA device is "null" (present on any ALSA installation, no
# hardware required). On the board pass plughw:0,0 instead.
set -u

BIN=${1:?usage: $0 <eac_play> [assets_dir] [alsa_device]}
ASSETS=${2:-$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/assets}
DEVICE=${3:-null}

if [ ! -d "$ASSETS" ]; then
    echo "assets directory not found: $ASSETS" >&2
    echo "generate them with: tests/gen_assets.sh" >&2
    exit 2
fi

rc=0
for f in "$ASSETS"/*.wav "$ASSETS"/*.mp3 "$ASSETS"/*.aac "$ASSETS"/*.m4a \
         "$ASSETS"/*.flac "$ASSETS"/*.ogg "$ASSETS"/*.opus "$ASSETS"/*.wv; do
    [ -f "$f" ] || continue
    printf '== %s ==\n' "$(basename "$f")"
    if timeout 60 "$BIN" "$f" --device "$DEVICE" --quiet; then
        echo "   OK"
    else
        echo "   FAILED (rc=$?)"
        rc=1
    fi
done
exit $rc
