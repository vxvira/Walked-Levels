#!/usr/bin/env bash
# builds WalkedLevels.cpp against the vendored AZBacktest
#   bash build.sh        build -> build/walkedLevels.exe
#   bash build.sh -run   build + run (one pass over the data, ~16 min)
set -e

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AZ="$ROOT/vendor/azbacktest"

mkdir -p "$ROOT/build"
g++ -std=c++17 -O2 -I"$ROOT/src" -I"$AZ" -I"$AZ/vendor/glfw/include" \
    "$ROOT/WalkedLevels.cpp" -o "$ROOT/build/walkedLevels.exe" \
    -L"$AZ/vendor/glfw/lib" -lglfw3 -lopengl32 -lgdi32 -lshell32 -lwinmm -limm32 \
    -static-libgcc -static-libstdc++ -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic
echo "built build/walkedLevels.exe"

if [[ "${1:-}" == "-run" ]]; then
    cd "$ROOT"   # config.toml is here
    ./build/walkedLevels.exe
fi
