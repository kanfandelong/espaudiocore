#!/bin/sh
# Verify text-tag and cover-art parsing on generated assets.
#
#   python3 tests/gen_cover_assets.py tests/assets   # once, needs mutagen
#   ./tests/cover_check.sh /path/to/meta_dump [assets_dir]
#
# Each container stores pictures differently; meta_dump prints one line per
# metadata item, so the check is "the expected PICTURE/text line is present"
# (or absent, for malformed picture comments).
set -u

BIN=${1:?usage: $0 <meta_dump> [assets_dir]}
ASSETS=${2:-$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/assets}
COVERS="$ASSETS/covers"
rc=0

if [ ! -d "$COVERS" ]; then
    echo "cover assets not found: $COVERS" >&2
    echo "generate them with: python3 tests/gen_cover_assets.py tests/assets" >&2
    exit 2
fi

run() {
    file=$1
    out=$(timeout 120 "$BIN" "$COVERS/$file" 2>/dev/null)
    if [ $? -ne 0 ]; then
        echo "$file: decode FAILED"
        rc=1
        return 1
    fi
    printf '%s\n' "$out"
}

check() {
    file=$1
    pattern=$2
    what=$3
    out=$(run "$file") || return
    if printf '%s\n' "$out" | grep -q -- "$pattern"; then
        echo "$file: OK  $what"
    else
        echo "$file: MISSING  $what"
        printf '%s\n' "$out" | sed 's/^/    /'
        rc=1
    fi
}

reject() {
    file=$1
    pattern=$2
    what=$3
    out=$(run "$file") || return
    if printf '%s\n' "$out" | grep -q -- "$pattern"; then
        echo "$file: UNEXPECTED  $what"
        printf '%s\n' "$out" | sed 's/^/    /'
        rc=1
    else
        echo "$file: OK  $what"
    fi
}

check t_cover.wav     'type=APIC binary=1 .*mime=image/jpeg'    'WAV trailing id3 APIC'
check t_cover.wav     'type=TIT2 binary=0 .*Wav Title'          'WAV id3 TIT2'
check t_cover.wav     'type=TPE1 binary=0 .*Wav Artist'         'WAV id3 TPE1'
check t_cover_pre.wav 'type=APIC binary=1 .*mime=image/jpeg'    'WAV leading id3 APIC'
check t_list.wav      'type=Title binary=0 .*WAV LIST Title'     'WAV LIST/INFO INAM'
check t_list.wav      'type=Artist binary=0 .*WAV LIST Artist'   'WAV LIST/INFO IART'
check t_list.wav      'type=Album binary=0 .*WAV LIST Album'     'WAV LIST/INFO IPRD'
check t_cover.flac    'type=PICTURE binary=1 .*mime=image/jpeg' 'FLAC PICTURE block'
check t_cover.ogg     'type=PICTURE binary=1 .*mime=image/jpeg' 'Vorbis METADATA_BLOCK_PICTURE'
check t_cover.ogg     'type=TITLE binary=0 .*Ogg Title'         'Vorbis TITLE comment'
check t_cover.opus    'type=PICTURE binary=1 .*mime=image/jpeg' 'Opus METADATA_BLOCK_PICTURE'
check t_legacy.opus   'type=PICTURE binary=1 .*mime=image/png'  'Opus legacy COVERART'
reject t_badpic.ogg   'type=PICTURE'                            'malformed picture skipped'

if [ "$rc" -eq 0 ]; then
    echo "cover check OK"
else
    echo "cover check FAILED"
fi
exit $rc
