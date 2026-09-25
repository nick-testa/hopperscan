#!/usr/bin/env bash
#
# Builds hopperscan using the lab37 build tree. The lab37 checkout supplies the
# toolchain and every library; nothing in it is modified.
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LAB37_DIR="${LAB37_DIR:-$HOME/Projects/lab37}"

if [[ ! -d "$LAB37_DIR" ]]; then
    echo "error: no lab37 checkout at $LAB37_DIR (set LAB37_DIR to override)" >&2
    exit 1
fi

# The attach point lives in lab37's CMake cache, so it survives ordinary rebuilds
# but not an expunge or a deleted build directory. Re-add it when it goes missing.
if ! grep -q "CMAKE_PROJECT_Lab37_INCLUDE:.*=$HERE/inject.cmake" "$LAB37_DIR/build/CMakeCache.txt" 2>/dev/null; then
    echo "==> attaching hopperscan to the lab37 build"
    (cd "$LAB37_DIR" && ./make.sh --setup-only --cmake-arg "-DCMAKE_PROJECT_Lab37_INCLUDE=$HERE/inject.cmake")
fi

(cd "$LAB37_DIR" && ./make.sh hopperscan)

mkdir -p "$HERE/bin"
cp "$LAB37_DIR/build/hopperscan" "$HERE/bin/hopperscan"
echo "==> $HERE/bin/hopperscan"
