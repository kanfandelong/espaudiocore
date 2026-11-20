#!/bin/sh
# Build espaudiocore (native Linux branch).
#
#   ./scripts/build.sh              host build (good for quick tests)
#   ./scripts/build.sh --cross      RK3506 ARMv7 cross build
#   ./scripts/build.sh --clean      wipe the work directory first
#
# The 9p /mnt/d mount is slow, so sources are rsynced to ~/espaudiocore-linux
# and compiled there (same convention as hmi_shell/build.sh).
set -eu

SRC=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORK=${WORK:-$HOME/espaudiocore-linux}
CROSS=0
CLEAN=0

for arg in "$@"; do
    case "$arg" in
        --cross) CROSS=1 ;;
        --clean) CLEAN=1 ;;
        -h|--help)
            sed -n '2,12p' "$0"
            exit 0
            ;;
        *)
            echo "unknown option: $arg" >&2
            exit 2
            ;;
    esac
done

if [ "$CLEAN" = 1 ]; then
    rm -rf "$WORK"
fi

mkdir -p "$WORK"
echo "== sync $SRC -> $WORK/src =="
rsync -a --delete \
    --exclude .git --exclude 'build' --exclude 'build-*' \
    "$SRC/" "$WORK/src/"

if [ "$CROSS" = 1 ]; then
    BUILD="$WORK/build-arm"
    echo "== configure (RK3506 cross) =="
    cmake -S "$WORK/src" -B "$BUILD" \
        -DCMAKE_TOOLCHAIN_FILE="$WORK/src/cmake/toolchain-rk3506.cmake" \
        -DCMAKE_BUILD_TYPE=Release
else
    BUILD="$WORK/build-host"
    echo "== configure (host) =="
    cmake -S "$WORK/src" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
fi

echo "== build =="
cmake --build "$BUILD" -j"$(nproc)"

echo
find "$BUILD" -maxdepth 1 -type f \( -name eac_play -o -name 'libespaudiocore.a' \) -exec ls -l {} \;
if command -v file >/dev/null 2>&1; then
    find "$BUILD" -maxdepth 1 -type f -name eac_play -exec file {} \;
fi
