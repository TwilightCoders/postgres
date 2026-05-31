#!/usr/bin/env bash
# Build ProgreSQL into build/install/ relative to this script.
#
# Don't conflict with Postgres' own top-level Makefile — this is just a
# wrapper around the canonical ./configure + make -j install sequence,
# pinned to an in-tree --prefix so the install never leaks into
# /usr/local or wherever.
#
# Usage:
#   ./build.sh             # configure (idempotent) + parallel build + install
#   ./build.sh clean       # nuke build/ entirely (forces fresh configure)
#   ./build.sh verify      # build/install/bin/postgres --version round-trip
#   PROGRESQL_PREFIX=...   # override install prefix (default: build/install)

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${PROGRESQL_PREFIX:-$HERE/build/install}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

cmd="${1:-build}"

case "$cmd" in
    build)
        mkdir -p "$PREFIX"
        if [ ! -f "$HERE/Makefile.global" ] || [ "$HERE/configure" -nt "$HERE/Makefile.global" ]; then
            echo "==> configuring (--prefix=$PREFIX)"
            cd "$HERE" && ./configure --prefix="$PREFIX" --without-icu --without-readline
        fi
        echo "==> building (-j$JOBS)"
        cd "$HERE" && make -j"$JOBS"
        echo "==> installing"
        cd "$HERE" && make install
        echo "==> done: $PREFIX/bin/postgres --version"
        "$PREFIX/bin/postgres" --version
        ;;
    clean)
        echo "==> removing $HERE/build"
        rm -rf "$HERE/build"
        echo "==> make distclean"
        cd "$HERE" && make distclean 2>/dev/null || true
        ;;
    verify)
        if [ ! -x "$PREFIX/bin/postgres" ]; then
            echo "no install at $PREFIX/bin/postgres — run $0 build first" >&2
            exit 1
        fi
        "$PREFIX/bin/postgres" --version
        ;;
    *)
        echo "usage: $0 [build|clean|verify]" >&2
        exit 1
        ;;
esac
